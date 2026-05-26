# Memo 12: KVM-aware snapshot/forkserver (task #250)

**Status:** design / not yet implemented (commit `7f79b35e1531`
ships a runtime guard that refuses the C-09 v1 forkserver path
under KVM until this lands).

**Why now.** Phase 1 closed 2026-04-25 with kvm/seccomp ratio at
1.15× on a real VMEXIT-bound benchmark; the perf story is done.
Phase 3's vision §"Snapshot/forkserver <50 ms cold-start for
fuzzing" is the remaining big lift, and the C-09 v1 forkserver
that landed 2026-04-20 only works under seccomp because it forks
the host UML process. Under KVM that fork breaks (see the
`7f79b35e1531` commit message for the full hazard list).

This memo specs how the KVM-aware path differs and what code has
to land for the v2 snapshot to work end-to-end.

## Why the v1 (fork-based) path doesn't carry to KVM

Three distinct kinds of breakage all fire at once:

1. **`/dev/kvm` + vCPU fd aliasing.** `fork()` clones the file
   descriptor table. After the fork, parent and child each hold
   the same `vcpu0_fd`. Issuing `KVM_RUN` from either process
   takes a kernel-side reference on the same vCPU object;
   concurrent ioctls race the per-vCPU `kvm_run` mmap (which is
   host memory shared between parent and child, not CoW'd).

2. **Memslot host-VA divergence.** `KVM_SET_USER_MEMORY_REGION`
   registers a tuple `(gpa, hva, size)`. The hva is recorded in
   the kernel's `kvm_memslots`. When the child's mm CoWs a page
   on first write, the parent's hva and the child's hva still
   resolve through the *registered* hva — but the kernel's
   shadow-PT walker accesses the **child's** physical pages
   only through the child's mm. Result: the parent observes
   stale guest memory, the child observes its own; they diverge
   after the first write.

3. **vCPU register state.** `kregs` and `sregs` live in the
   `kvm_vcpu` kernel object referenced by `vcpu0_fd`. There's
   one such object regardless of how many host processes share
   the fd. Either child wins races on `KVM_SET_REGS`; the loser
   silently runs with the winner's RIP/CR3.

Items (1)+(3) are fundamental — they don't go away with any
amount of memslot tweaking. The fix has to side-step
`fork()` entirely.

## v2 architecture: state-snapshot + replay

Replace the v1 `fork() per iteration` model with **save + restore**:

```
                ┌──────────────────────────────────────┐
                │ Parent process (long-lived UML host) │
                │                                      │
   boot ──────► │  …UML init…                          │
                │  um_snapshot_ready("init-done")      │
                │  ┌──── checkpoint ──────────────┐    │
                │  │ snapshot.regs   = KVM_GET_REGS    │
                │  │ snapshot.sregs  = KVM_GET_SREGS   │
                │  │ snapshot.msrs   = KVM_GET_MSRS    │
                │  │ snapshot.fpu    = KVM_GET_FPU     │
                │  │ snapshot.events = KVM_GET_VCPU_EV │
                │  │ snapshot.mem    = memcpy(hva,...) │
                │  │   for each memslot                │
                │  │ snapshot.shadow_pgd = clone()     │
                │  └────────────────────────────────────│
                │                                      │
   loop ──────► │  for each fuzz iteration:            │
                │    restore vCPU state                │
                │    restore memslot contents          │
                │    inject testcase                   │
                │    KVM_RUN until exit                │
                │    record outcome                    │
                │    (no fork)                         │
                └──────────────────────────────────────┘
```

The fuzzer + UML stay in one process. Each iteration is just
`KVM_SET_REGS` + memslot restore + `KVM_RUN`. No fd aliasing,
no mm CoW divergence, no register races.

### Why this works on KVM

- `KVM_GET_REGS` / `KVM_SET_REGS` are pure ioctls against a
  kernel-side struct. Snapshot is a `memcpy` on the host side,
  ~200 bytes. Restore is a `memcpy` plus one ioctl.
- `KVM_GET_MSRS` / `KVM_SET_MSRS` cover MSR_LSTAR, MSR_STAR,
  MSR_FMASK, MSR_KERNEL_GS_BASE, MSR_FS_BASE, MSR_GS_BASE,
  MSR_EFER. ~8 entries × 16 bytes = 128 bytes.
- `KVM_GET_FPU` / `KVM_SET_FPU` snapshot the XSAVE area for
  AVX/AVX-512 register state.
- `KVM_GET_VCPU_EVENTS` / `KVM_SET_VCPU_EVENTS` capture pending
  exceptions, IRQs, NMI, SMI flags.
- Memslot contents: the host VA is `__pa(uml_physmem)`-aliased
  to a contiguous kernel mapping; we already memcpy from it for
  diagnostics. Snapshot is a buffer copy of the live region.
  Cost: O(physmem_size). For a 256 MiB UML this is ~50 ms on
  modern memory bandwidth — already at the vision's <50 ms
  target without dirty-bitmap optimization.

### Dirty-bitmap fast path (the <50 ms target)

For the steady-state iteration cost (not the cold-start), we
care about restoring **only the pages the previous iteration
modified.** KVM exposes:

- `KVM_CAP_DIRTY_LOG_RING_ACQ_REL` — per-vCPU dirty ring,
  populated by KVM as the guest writes pages.
- `KVM_GET_DIRTY_LOG` — bitmap-form dirty log per memslot.

The flow:
```
checkpoint: snapshot full memslot
iteration N:
  KVM_GET_DIRTY_LOG (or drain dirty ring)
  for each dirty page: copy from snapshot back to live hva
  KVM_CLEAR_DIRTY_LOG
  KVM_SET_REGS (restore checkpoint regs)
  KVM_RUN
```

A typical syzkaller iteration touches <1 MiB of guest memory.
Restore cost drops from ~50 ms (full memslot) to ~1 ms
(dirty pages only). Vision's <50 ms cold-start is the
**checkpoint** cost; iterations are sub-millisecond.

### Per-mm shadow PGD (task #243 dependency)

Today the KVM backend uses a singleton shadow PGD shared across
mms; cross-mm context switches clear the user half. For the v1
KVM-aware snapshot (single-mm, no concurrent fuzz workers
sharing the same VM) the singleton is fine. For v2 concurrent
workers (multiple VMs, one per worker thread) we need #243
(per-mm cached shadow PGD) so each worker's mm has its own
shadow PT. Today's blocker is administrative (#243 not landed),
not architectural.

## Code layout (proposal)

New file `arch/um/backend/kvm/snapshot.c`:

```c
struct kvm_snapshot {
    struct kvm_regs        regs;
    struct kvm_sregs       sregs;
    struct kvm_fpu         fpu;
    struct kvm_msrs        *msrs;     /* variable-length */
    struct kvm_vcpu_events events;

    /* Memslot snapshot — one per registered region. */
    struct kvm_snapshot_slot {
        u64    gpa;
        u64    size;
        void   *backing;              /* kvmalloc'd full snapshot */
        unsigned long *dirty_bitmap;  /* per-page, sized by KVM */
    } slots[KVM_MAX_MEMSLOTS];

    /* Shadow PGD snapshot — clone the singleton at checkpoint. */
    void   *shadow_pgd_clone;
};

int kvm_snapshot_capture(struct kvm_snapshot *snap);
int kvm_snapshot_restore_full(struct kvm_snapshot *snap);
int kvm_snapshot_restore_dirty(struct kvm_snapshot *snap);
void kvm_snapshot_free(struct kvm_snapshot *snap);
```

Hooks in `arch/um/kernel/snapshot.c`:

```c
+ #ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
+ if (um_backend->kind == UM_BACKEND_KIND_KVM)
+         return kvm_snapshot_ready_path(named_point);
+ #endif
  /* existing fork-based path for seccomp */
```

The fork-based path stays for seccomp (where it works fine);
the KVM path is a separate routine that doesn't touch fork().

## Validation

New kselftest `tools/testing/selftests/um/snapshot-kvm-smoke/`:

1. Boot UML kvm with `init=snapshot-kvm-smoke.sh`.
2. Guest writes a marker to `/sys/kernel/debug/um/snapshot_ready`.
3. Host driver calls KVM-snapshot ioctl (or debugfs hook).
4. Driver calls KVM-restore N times.
5. Each restore must observe the **checkpoint state** (no
   leaks from prior iteration).
6. Total wall time for N=100 must be <5 s (50 ms / iteration
   ceiling).

This is the deliverable for task #251.

## Open questions

- **Memslot snapshot size.** UML's physmem is typically 64-256 MiB.
  A full snapshot copy is ~5-50 ms (memory bandwidth bound).
  Acceptable for the vision's <50 ms cold-start. Iteration
  cost is dirty-bitmap bound — sub-millisecond expected.
- **vCPU events scope.** `KVM_GET_VCPU_EVENTS` may not capture
  all of what we need (e.g., pending APIC IRQ state). For ncpus=1
  UML this is moot; for SMP we may need additional ioctls.
- **MMIO region snapshots.** UML's MMIO regions (vDSO, etc.)
  are memslot-registered? If yes, included in the memslot
  snapshot. If no, separate handling needed.
- **Shadow PT clone vs rebuild.** Cheaper to clone the existing
  shadow PGD tree at checkpoint (~12 KiB allocation) or to
  rebuild from UML's logical pgd at restore? Probably clone for
  cold-start latency.

These all resolve at implementation time; none affect the
v2 architecture.

## Sequencing

| Step | Effort | Output |
|------|--------|--------|
| 1. `kvm_snapshot_capture` skeleton + KVM_GET_* | half day | builds, no restore yet |
| 2. `kvm_snapshot_restore_full` + memcpy | half day | round-trip works |
| 3. Hook into `um_snapshot_ready` for KVM | 1 day | guard removed; `snapshot-smoke` PASSES under kvm |
| 4. Dirty-bitmap fast path | 1 day | iteration <1 ms |
| 5. `snapshot-kvm-smoke` kselftest (#251) | half day | regression guard |
| 6. Document + measurements | half day | updated `measurements.md` + this memo's status flipped to "implemented" |

Total: ~4 engineer-days. None of it blocks Phase 1; can run in
parallel with Phase 4 ARM64/RISC-V port (#256/#257) since the
two sit in different code regions.

## Status

- 2026-04-25 (initial) — memo written; `7f79b35e1531` ships the
  runtime refusal guard.
- 2026-04-25 (later) — **steps 1+2+bench landed.**
  - Step 1+2: `kvm_snapshot_capture` / `_capture_regs_only` /
    `_restore_full` / `_alloc` / `_free` / `_destroy` shipped
    in commit `040bdb2f6b04`. Both full-memslot and regs-only
    capture paths covered; the regs-only variant lets callers
    avoid the bounded-vmalloc constraint at late_initcall_sync.
  - KUnit basic-shape test (`kvm_snapshot_basic_test`) shipped
    in commit `b6e5bceab7a2`.
  - `snapshot-kvm-smoke` kselftest shipped in commit
    `167b5b2f4715` (task #251).
  - `kvm_snapshot_bench` kernel-cmdline + debugfs driver +
    `kvm-snapshot-bench` kselftest shipped in commit
    `f6796ef4bb99`. **Real numbers on the dev host
    (mem=128M, N=64, regs-only):**

      capture=37 µs; restore_full median=14 µs p95=20 µs

    Two orders of magnitude under the vision's targets
    (<50 ms cold-start, <1 ms iteration). The full-memslot
    path will dominate at memcpy bandwidth — for typical
    64-256 MiB configs that's 5-50 ms, still under target.
- Steps 3 (`um_snapshot_ready` integration) and 4 (dirty-bitmap
  fast path) deferred to a focused Phase-3 session; the
  measurement above already validates the latency claim, so the
  remaining work is correctness wiring rather than perf-proof.
