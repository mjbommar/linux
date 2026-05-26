# UML kvm-v2 snapshot port — design memo (2026-05-16)

Strategic Time-machine lift per
`Documentation/virt/uml/redesign/06-sequencing/PLAN-2026-05-14.md`
§4.1 (#168). This memo plans the port of the v1 archive's
`arch/um/backend/kvm-v1-archive/snapshot.c` (647 LoC, working under
v1) onto the v2 backend's vCPU pool + memslot list shape.

The Phase 1 code (struct + alloc/free + `kvm_v2_snapshot_capture_
regs_only` + `kvm_v2_snapshot_restore_full` + a build-time KUnit
case) lands alongside this memo in one commit; this document
captures the deltas the port has to handle, the sub-sequencing for
Phases 2-N (full capture + memslot + IDT/GDT/IST), and the open
questions that will be settled by integration testing in Phase 2.

## 1. Why a port (vs reimplementation)

Memo 12 (`12-snapshot-forkserver-kvm.md`) already justified the
v1-era design: KVM ioctls (`KVM_GET_REGS` / `KVM_SET_REGS` /
…) are the natural API for capture/restore, the memslot host-VA
divergence makes `fork()` unworkable, and the snapshot is the
foundation the record/replay path (#169, memo 13) builds on. The
v1 archive proved the design; lifting the file with deltas is
cheaper and lower-risk than designing from scratch a second time.

The deltas below are surgical, not structural — the snapshot
container shape, the ioctl ordering, the MSR list, and the
memslot copy strategy all carry over with only naming + per-task
plumbing changes.

## 2. v1 archive surface (the lift target)

`arch/um/backend/kvm-v1-archive/snapshot.c` exports:

```c
struct kvm_snapshot;
struct kvm_snapshot *kvm_snapshot_alloc(void);
void  kvm_snapshot_destroy(struct kvm_snapshot *snap);
int   kvm_snapshot_capture(struct kvm_snapshot *snap);
int   kvm_snapshot_capture_regs_only(struct kvm_snapshot *snap);
int   kvm_snapshot_restore_full(struct kvm_snapshot *snap);
void  kvm_snapshot_free(struct kvm_snapshot *snap);
```

Internal shape (v1 `struct kvm_snapshot`, ~80 B + the variable-
size memslot buffer):

  - `struct kvm_regs regs`         — GP regs (RIP/RSP/RFLAGS + 16 GPRs)
  - `struct kvm_sregs sregs`       — segs + CR0/2/3/4 + IDT/GDT/TR/LDT
  - `struct kvm_fpu fpu`           — legacy 512 B FXSAVE area
  - `struct kvm_vcpu_events events`— pending exception/IRQ window/NMI state
  - `struct { __u32 nmsrs; __u32 pad; struct kvm_msr_entry entries[7]; } msrs`
  - `void *mem_backing` + `size_t mem_size` — full memslot copy (Policy A)

The seven MSRs captured: `MSR_LSTAR`, `MSR_STAR`, `MSR_FMASK`
(`MSR_SYSCALL_MASK` in the upstream kernel header — `0xC0000084`),
`MSR_KERNEL_GS_BASE`, `MSR_FS_BASE`, `MSR_GS_BASE`, `MSR_EFER`.

v1's capture ordering:

  1. `KVM_GET_REGS`
  2. `KVM_GET_SREGS`
  3. `KVM_GET_FPU`
  4. `KVM_GET_VCPU_EVENTS`
  5. `KVM_GET_MSRS` (7-entry list)
  6. memslot memcpy (skipped by `capture_regs_only`)

v1's restore ordering (NOTE the sregs-first rule):

  1. memslot memcpy (if `mem_backing != NULL`)
  2. `KVM_SET_SREGS`  (must precede `KVM_SET_REGS` — KVM validates
     RIP/RSP against the post-SREGS segment cache)
  3. `KVM_SET_REGS`
  4. `KVM_SET_FPU`
  5. `KVM_SET_VCPU_EVENTS`
  6. `KVM_SET_MSRS`

The "sregs before regs" ordering carries over verbatim — same
KVM invariant.

## 3. v1 → v2 deltas (the surgical changes)

### 3.1 Per-task vCPU lookup → per-host-CPU pool

**v1:** `current->thread.arch.kvm.vcpu` was the task's own vCPU
handle. `current->thread.arch.kvm.vcpu->fd` was the snapshot
target.

**v2:** vCPUs are pooled per host CPU (`struct kvm_v2_vcpu vcpus
[NR_CPUS]` in `vcpu.c`). Tasks dispatch onto whichever pool
member matches the current host CPU index. A given task may have
last run on a different pool entry across its lifetime; the
authoritative "vCPU that last ran current" record is the
`vcpu->last_task` field added by SMP-T16 (D106 / D107 / commit
documented in `kvm_v2_backend.h:387`).

**Phase 1 invariant the snapshot uses:** walk the pool and return
the entry whose `last_task == current`. This is the simpler of
the two candidates ("via `iotrap_fpu`'s owning vCPU" being the
other). The walk is bounded by `nr_cpu_ids` (≤ 64 on UML), runs
under `preempt_disable` so the lookup result remains valid for
the duration of the capture/restore, and falls back to "the vCPU
pinned to the current host CPU" if no pool entry has `last_task
== current` yet (fresh task, never dispatched).

**Phase 1 limitation we accept:** capture only works for the
calling task. Cross-task snapshot ("snapshot the vCPU that ran
task X from task Y") is deferred to Phase 4 along with the
record/replay deterministic-driver work that actually needs it.
The KUnit case at Phase 1 doesn't need it (test runs in the
calling task's context).

### 3.2 FPU: `KVM_GET_FPU` → `KVM_GET_XSAVE`

**v1:** `struct kvm_fpu` (512 B legacy FXSAVE shape).
`KVM_GET_FPU` + `KVM_SET_FPU` round-tripped X87 + SSE state only.

**v2 (since SMP-T57 Phase A, commit `ab68bf077de3`):** guest
CR4.OSXSAVE = 1, XCR0 = 0x7 (FP|SSE|YMM), CPUID advertises
AVX/AVX2/FMA/F16C. The upper-128 of YMM0..YMM15 lives in the
XSAVE area, NOT in the legacy 512 B FXSAVE area.

**The trap:** `KVM_GET_FPU` against an OSXSAVE-enabled vCPU
captures only the legacy header — the YMM upper-128 is silently
dropped. Restoring via `KVM_SET_FPU` resets YMM upper to zero,
which is a deterministic bug under any AVX-using guest code
(glibc IFUNC dispatch lands on `vpxor`).

**The fix the snapshot needs:** use `KVM_GET_XSAVE` /
`KVM_SET_XSAVE` (`0xa4` / `0xa5` in `<uapi/linux/kvm.h>`).
`struct kvm_xsave` is the 4 KB `__u32 region[1024]` shape; the
ioctl is fixed-size (`KVM_GET_XSAVE2` is the variable-size
follow-up needed for dynamic XCR0 features like AMX, NOT needed
under v2's curated CPUID).

The static 4 KB allocation per snapshot is cheap (one struct on
the heap, no separate vmalloc). Phase 1 wires `struct kvm_xsave
xsave` as a normal field of `struct kvm_v2_snapshot`.

Note: `current->thread.arch.kvm_v2.iotrap_fpu` (still
`struct kvm_fpu`, 512 B legacy) is independent — that's the
per-task per-dispatch FPU slot for the cross-task isolation path
(memo §H.1b smoking-gun fix). The snapshot does NOT touch
`iotrap_fpu`; capture is direct from the vCPU via the XSAVE
ioctl. Restore likewise writes the vCPU directly, leaving
`iotrap_fpu` untouched. See §4.4 below for the cross-task
semantics.

### 3.3 New: capture XCR0 via `KVM_GET_XCRS`

v1 didn't capture XCR0 (it was always 0 under v1's curated
masking). v2 sets XCR0 = 0x7 lazily on first dispatch; the
snapshot has to preserve it so restore-then-run lands on a vCPU
with the matching XCR0 the saved XSAVE area was computed under.

Snapshot field: `struct kvm_xcrs xcrs` (one slot, `xcrs[0]`,
`xcr=0`, `value=` whatever the vCPU has at capture time).
`KVM_GET_XCRS` / `KVM_SET_XCRS` (`0xa6` / `0xa7`) are the round-
trip pair.

### 3.4 SMP-T55 per-vCPU FPU dirty flag

**Context.** D119 (`kvm_v2_backend.h:391-423`) added
`vcpu->fpu_dirty` + `vcpu->fpu_owner_task` per-vCPU bookkeeping:
the pre-dispatch `KVM_SET_FPU` is skipped when the vCPU's guest
FPU is bit-identical to `current->thread.arch.kvm_v2.iotrap_fpu`,
which is the case when (a) `fpu_dirty == false` AND (b)
`fpu_owner_task == current`. Saves ~14% perf on Python startup.

**Question for the port:** does the snapshot need to capture +
restore `fpu_dirty` / `fpu_owner_task` ?

**Answer:** NO at the scalar level, YES at the documented
semantic level.

The snapshot is scalar vCPU + memslot state — fields owned by
the host's KVM, not host-side UML-private accounting. The
`fpu_dirty` flag is a per-vCPU optimization HINT for the next
dispatch; if we corrupt it (mistakenly clear it after restore,
forcing one extra `KVM_SET_FPU`) the worst that happens is a
single wasted ioctl with no semantic impact. Conversely if we
spuriously set it (forcing one extra `KVM_GET_FPU` after the
next vmexit) same — no semantic impact.

**What we DO need:** clear `fpu_dirty` semantics across the
restore boundary. Specifically, restore_full sets `fpu_dirty =
true` and `fpu_owner_task = NULL` for the target vCPU so the
NEXT dispatch unconditionally re-installs the FPU from the
restored XSAVE area. The "lazy skip" optimization re-arms on
the dispatch after that, once we have an established
`fpu_owner_task` for the post-restore configuration. The
alternative (leave the flag set the way restore found it) risks
running a dispatch on a vCPU whose XSAVE area we just
overwrote with snapshot bytes, but `fpu_dirty == false` says
"trust the vCPU's view" — the snapshot bytes get masked by
KVM's `iotrap_fpu`-based skip.

Same reasoning for `last_task` / `last_mm`: clear them
post-restore so the next dispatch's `cross_task` gate fires and
re-installs SREGS unconditionally.

### 3.5 CR4.OSXSAVE + XCR0 ordering in restore

**Constraint:** `KVM_SET_SREGS` rejects `cr4.OSXSAVE = 1`
unless CPUID's OSXSAVE bit is installed first. `KVM_SET_XCRS`
rejects a non-trivial XCR0 unless guest_supported_xcr0 (derived
from CPUID leaf 0xD) advertises the bits.

The post-SMP-T57 first-dispatch arming sequence (`vcpu.c`
around line 2027-2066, the `cpuid_primed = false` block) is:

  1. `KVM_SET_CPUID2` (curated mask, includes OSXSAVE bit)
  2. `KVM_GET_SREGS` (read current cr4)
  3. `KVM_SET_SREGS` (cr4 |= OSXSAVE)
  4. `KVM_SET_XCRS` (xcr0 = 0x7)

**Restore implication:** the snapshot CANNOT restore against a
vCPU whose `cpuid_primed == false`. If we hit that case, the
restore has to either (a) drive the first-dispatch path itself
or (b) refuse with `-EAGAIN`.

Phase 1 takes (b): restore_full asserts the target vCPU has
already been dispatched at least once (`vcpu->last_task` or
similar non-NULL sentinel). The KUnit case satisfies this by
construction (it manually dispatches before capturing).

Phase 2+ (`kvm_v2_snapshot_capture_full` w/ memslot) will
revisit — the v1 archive ordered sregs before the regs/fpu
writes, which is sufficient under v1 because v1's first-run
arming ran inside `vcpu_create_one` (no lazy first-dispatch
chicken-and-egg). v2's restore on a fresh vCPU needs to
either prime CPUID/OSXSAVE first or sequence the restore
strictly after first dispatch. Memo TODO §6.1.

### 3.6 TLB / mmu_notifier coherence

**v1:** snapshot.c's `restore_full` did NOT explicitly flush
guest TLB. v1's snapshot worked because v1's per-task vCPU
lifecycle meant the restore happened during a quiescent
boundary (between fuzz iterations, vCPU not running).

**v2:** SMP-T31/T32/T33 fixes (D109-style mmu_notifier
coordination, kvm-v2 `region.c` + `tlb.c`) imply that after a
memslot rewrite, ALL pool vCPUs may have stale TDP entries.
The restore must either:

  - explicitly call into `kvm_v2_tlb_kick_others()` after the
    memslot memcpy + `KVM_SET_USER_MEMORY_REGION` (Phase 3 work,
    when `kvm_v2_snapshot_capture` lands the memslot path), or
  - rely on TDP's normal mmu_notifier flow if the snapshot
    happens through the same VA range the existing memslot
    covers (Policy A: one singleton memslot at uml_physmem,
    same gpa, same userspace_addr — mmu_notifier fires
    naturally on the memcpy).

Phase 1 doesn't restore memslot bytes; this question is filed
for Phase 3.

### 3.7 IDT / GDT / IST + per-vCPU trampoline state

v1's snapshot didn't capture these (v1 had a different layout
strategy). v2 has per-VM IDT/GDT pages + per-vCPU IST stacks +
per-vCPU TSS + per-vCPU gadget state page — all written to
guest VAs that are valid as long as the VM lives. A snapshot
that's used for record/replay across a longer time window (#169)
needs to preserve these too.

Phase 1 ignores them (a regs-only snapshot doesn't need them;
the IDT/GDT pages aren't modified during normal dispatch).
Phase 3 will add them to `kvm_v2_snapshot_capture`'s full path.

## 4. Phase 1 design (what's landing in this commit)

### 4.1 Headers + struct

In `arch/um/backend/kvm-v2/kvm_v2_backend.h`:

```c
struct kvm_v2_snapshot {
    /* vCPU state — captured via KVM_GET_* ioctls. */
    struct kvm_regs        regs;
    struct kvm_sregs       sregs;
    struct kvm_xsave       xsave;     /* SMP-T57 — replaces v1's kvm_fpu */
    struct kvm_xcrs        xcrs;      /* new in v2: XCR0 snapshot */
    struct kvm_vcpu_events events;

    /* 7-MSR list — same shape as v1, inline storage. */
    struct {
        __u32 nmsrs;
        __u32 pad;
        struct kvm_msr_entry entries[KVM_V2_SNAPSHOT_MSR_COUNT];
    } msrs;

    /* Memslot snapshot (Phase 3 — not used by Phase 1). */
    void  *mem_backing;
    size_t mem_size;
};

struct kvm_v2_snapshot *kvm_v2_snapshot_alloc(void);
void  kvm_v2_snapshot_destroy(struct kvm_v2_snapshot *snap);
void  kvm_v2_snapshot_free(struct kvm_v2_snapshot *snap);
int   kvm_v2_snapshot_capture_regs_only(struct kvm_v2_snapshot *snap);
int   kvm_v2_snapshot_capture(struct kvm_v2_snapshot *snap);          /* Phase 3 stub */
int   kvm_v2_snapshot_restore_full(struct kvm_v2_snapshot *snap);
```

`KVM_V2_SNAPSHOT_MSR_COUNT == 7` (same list as v1).

### 4.2 Implementation files

  - `arch/um/backend/kvm-v2/snapshot.c` — Phase 1 implementation.
  - `arch/um/backend/kvm-v2/Makefile` — add `snapshot.o` under
    `obj-$(CONFIG_UM_BACKEND_KVM_V2)`.
  - `arch/um/backend/kvm-v2/test_byteshape.c` — append the
    Phase 1 KUnit case `kvm_v2_snapshot_basic` (single case;
    the byte-shape suite already has the build glue).

### 4.3 vCPU lookup helper

Phase 1 inline static helper in `snapshot.c`:

```c
static struct kvm_v2_vcpu *kvm_v2_snapshot_pick_vcpu(void)
{
    int cpu;
    /* Prefer the vCPU whose last_task == current — that's the
     * one whose KVM-owned state matches what we want to snapshot.
     * Fall back to the per-host-CPU vCPU if no entry matches yet
     * (fresh task, never dispatched).
     */
    for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
        struct kvm_v2_vcpu *v = kvm_v2_vcpu_get(cpu);
        if (v && v->last_task == current)
            return v;
    }
    return kvm_v2_vcpu_get(smp_processor_id());
}
```

Called from inside `preempt_disable` so `current` and the
`last_task` field are stable for the duration of the lookup.

### 4.4 Cross-task semantics — documented limitations

`vcpu->fpu_dirty` + `vcpu->fpu_owner_task` are properties of the
vCPU, not the task. The snapshot captures the SCALAR XSAVE
state, which is the FPU contents the vCPU currently holds —
i.e. the FPU state of whichever task last dispatched on that
vCPU. Under Phase 1 the "calling task last ran on this vCPU"
invariant (§4.3) means this is current's FPU state in practice.

Cross-task snapshot (snapshot from task A of task B's vCPU)
will need the snapshot to also capture `fpu_owner_task` so
restore re-installs to the right task. Deferred to Phase 4 —
the record/replay layer is where this matters because it's the
caller that introduces cross-task snapshot.

### 4.5 Restore-side post-restore invariants

Per §3.4, after a successful restore the snapshot code:

  - sets `vcpu->fpu_dirty = true`
  - sets `vcpu->fpu_owner_task = NULL`
  - sets `vcpu->last_task = NULL`
  - sets `vcpu->last_mm = NULL`

This forces the next dispatch's `load_user_sregs` to take the
"cross-task arrival" branch and re-install SREGS + FPU from
scratch, which is the conservative-correct behaviour. The lazy
fast paths re-arm on the dispatch after.

### 4.6 What Phase 1 does NOT do

  - **No `kvm_v2_snapshot_capture` implementation** — the full
    capture (XSAVE + XCR0 + memslot + IDT/GDT/IST) is Phase 3.
    Phase 1 ships the function as a stub returning `-EOPNOTSUPP`
    (checkpatch dislikes `-ENOSYS` outside the syscall-dispatch
    path, and `-EOPNOTSUPP` is the closer-fit POSIX errno for "not
    implemented in this build").
  - **No memslot copy** — Phase 3.
  - **No debugfs bench harness** — Phase 5 (selftest re-plumb).
  - **No selftests integration** — Phase 6 (`tools/testing/
    selftests/um/snapshot-{kvm-,}smoke`).
  - **No `EXPORT_SYMBOL_GPL` consumers** — Phase 2+ when the
    snapshot becomes a building block for record/replay.

## 5. Phase 1 KUnit case

`kvm_v2_snapshot_basic` lives in `test_byteshape.c` next to the
existing byte-shape suite. The case:

  1. Skips if the vCPU pool isn't initialised (build-time-only
     environment, no real KVM_RUN has happened — `kvm_v2_vcpu_
     get(0)` returns NULL → `kunit_skip`).
  2. Otherwise: allocates a snapshot, calls `capture_regs_only`,
     modifies the guest RAX via `KVM_SET_REGS`, calls
     `restore_full`, asserts the snapshot's regs.rax matches the
     post-restore `KVM_GET_REGS` value.

The skip path is critical — the case has to be runnable under
`kunit_um.py` (which DOES have a vCPU pool) AND under a normal
build that has CONFIG_UM_BACKEND_KVM_V2_KUNIT=y but no actual
running guest. The `kunit_skip` keeps the build green either way.

Under Phase 1's "no kernel run" gate (build verification only),
the test compiles but the assertion code is dead until Phase 2
wires up a runnable KUnit env. The build-time win is the type-
check pass: any signature mismatch between the snapshot API and
the test trips compile.

## 6. Sub-sequencing the future Phases

Per PLAN-2026-05-14 §4.1's seven-step ladder, the post-Phase-1
work is:

### Phase 2 — make the KUnit case actually run

  - Stand up a UML harness that runs the v2 backend to KVM_RUN
    completion at boot time, so the KUnit suite has a live vCPU
    pool to work against.
  - The byte-shape suite is currently build-time only because
    no test environment runs a v2 boot end-to-end yet; Phase 2
    fixes that.
  - Re-enable the `kvm_v2_snapshot_basic` assertion path.

### Phase 3 — full capture (`kvm_v2_snapshot_capture`)

  - Implement the memslot copy. Phase 1 leaves
    `mem_backing/mem_size` zero; Phase 3 populates them via
    `kvmalloc(physmem_size, GFP_KERNEL)` + `memcpy` from the
    base physmem VA the v2 backend's singleton memslot covers.
  - Capture IDT/GDT pages (per-VM) + IST stack + TSS + gadget
    state pages (per-vCPU). The per-vCPU pieces snapshot ONLY
    the picked-vCPU's set (per §4.3's invariant).
  - Adjust restore_full to memcpy the memslot back + restore
    the per-vCPU pages BEFORE the `KVM_SET_*` register pushes.
  - Add the explicit `kvm_v2_tlb_kick_others()` call after the
    memslot memcpy (§3.6).

Sizing: ~300 LoC delta over Phase 1.

### Phase 4 — cross-task snapshot semantics

  - Capture `fpu_owner_task` + the per-task `iotrap_fpu` so
    restore can re-bind to the right task.
  - Wire the "snapshot from task A of task B's vCPU" path that
    record/replay (#169) needs.

Sizing: ~150 LoC.

### Phase 5 — bench harness

  - Port v1's `kvm_snapshot_bench` cmdline + debugfs entry from
    `kvm-v1-archive/snapshot.c:412-647`. Boot-time cmdline +
    debugfs write fire the same N-iteration capture/restore
    cycle, dmesg the cycle/p95/median.
  - Re-purpose the bench numbers for the `<50 ms cold / <1 ms
    iter` targets memo 12 set.

Sizing: ~200 LoC port + ~50 LoC test harness tweaks.

### Phase 6 — selftest re-plumbing

  - `tools/testing/selftests/um/snapshot-{kvm-,}smoke` exist
    under v1. Re-plumb them against the v2 API names
    (`kvm_v2_*` prefix). Most of the work is name-change +
    re-running the matrix.

Sizing: ~50 LoC.

### Phase 7 — record/replay foundation (#169)

  - `struct kvm_v2_record` reuses `struct kvm_v2_snapshot` for
    the initial checkpoint. The record/replay path (memo 13)
    layers nondeterminism capture (RDTSC / RDRAND / interrupts
    / syscall results / MMIO) on top of the snapshot
    primitive.
  - Out of scope for this memo; tracked at memo 13 + a future
    `27-record-replay-v2-port.md`.

## 7. Open questions (for Phase 2+)

### Q1: Should snapshot also capture `vcpu->kvm_run`'s sync_regs?

The `kvm_run->s.regs.{regs,sregs}` mmap area is dirty-flagged via
`kvm_valid_regs`. If a dispatch was interrupted between writing
the sync-regs and the next KVM_RUN, the dirty bits live in the
mmap. A snapshot captured at that moment via plain
`KVM_GET_REGS` would miss the pending writes.

**Tentative answer:** capture-after-dispatch boundary is what
matters; record/replay (#169) will checkpoint between iterations,
which IS a clean boundary. Defer the sync-regs question to
Phase 4 alongside the cross-task semantics work.

### Q2: AMX / dynamic XSAVE features

The static `KVM_GET_XSAVE` is 4 KB. AMX state (XCR0 bits
17/18) is larger and requires `KVM_GET_XSAVE2`. Phase 1 sticks
with the legacy ioctl because the curated CPUID mask leaves AMX
off; revisit if SMP-T57 Phase B (AVX-512) or a hypothetical
Phase C (AMX) lands.

### Q3: NMI / pending IRQ state

`KVM_GET_VCPU_EVENTS` covers pending exceptions / IRQ windows.
Pending NMIs may have additional state — see KVM's
`kvm_vcpu_events::nmi`. v1 captured it; v2 inherits the same
shape via the same struct. Build-time check: `struct
kvm_vcpu_events` should have the same layout v1 saw. No code
change needed; flagged for the Phase 2 assertion suite.

### Q4: Should `kvm_v2_snapshot_*` be EXPORT_SYMBOL_GPL'd from
the start?

v1's symbols were EXPORT_SYMBOL_GPL because the v1 record/replay
TU (`record.c`, 1597 LoC) was a separate file. v2 is more
modular — record/replay (#169) will likely be a separate .c too
(`arch/um/backend/kvm-v2/record.c`). To keep the option open,
Phase 1 ships `EXPORT_SYMBOL_GPL` on all public symbols, even
though there are no in-tree callers yet. This matches the v1
archive precedent and avoids a churn commit later. KUnit-only
callers don't strictly need the EXPORT (the test TU is in the
same module), but the exports are cheap.

### Q5: Snapshot during a paused KVM_RUN

The `pthread_kill` path UML uses for cross-vCPU TLB shootdown
can interrupt a KVM_RUN. If a snapshot races with a remote-CPU
TLB kick, the captured state could be partially-mid-dispatch.
For Phase 1 this can't happen (Phase 1's only caller is the
KUnit case, single-threaded). Phase 3+ will add an explicit
"all vCPUs quiesced" predicate before `kvm_v2_snapshot_capture`
proceeds.

## 8. References

  - `arch/um/backend/kvm-v1-archive/snapshot.c` (647 LoC, the
    lift target)
  - `arch/um/backend/kvm-v2/kvm_v2_backend.h` (struct
    `kvm_v2_vcpu` + `struct kvm_v2_vm` declarations)
  - `arch/um/backend/kvm-v2/vcpu.c` lines 1239-1248
    (`kvm_v2_vcpu_get`), 1438-1478 (`last_task` /
    `fpu_owner_task` semantics)
  - `02-workstreams/D-kvm-backend/12-snapshot-forkserver-kvm.md`
    (the design memo this port implements)
  - `02-workstreams/D-kvm-backend/13-record-replay-determinism.md`
    (the layer above this one — #169)
  - `06-sequencing/PLAN-2026-05-14.md` §4.1 (the seven-step
    sub-sequencing this memo elaborates)
  - `04-risks/decisions-log.md` D119 (SMP-T55 — `fpu_dirty` +
    `fpu_owner_task`), D121 (SMP-T57 Phase A — XSAVE plumbing),
    D123 (this port decision)

## 9. Acceptance for Phase 1 (this commit)

  - [x] Memo written (this file).
  - [x] `struct kvm_v2_snapshot` + alloc/destroy/free in tree.
  - [x] `kvm_v2_snapshot_capture_regs_only` IMPLEMENTED for v2.
  - [x] `kvm_v2_snapshot_restore_full` IMPLEMENTED for v2.
  - [x] `kvm_v2_snapshot_capture` STUB returns `-EOPNOTSUPP`.
  - [x] KUnit case `kvm_v2_snapshot_basic` exists (skip-aware).
  - [x] `arch/um/backend/kvm-v2/Makefile` builds `snapshot.o`.
  - [x] Build clean under `make ARCH=um O=…`.
  - [x] checkpatch clean.
  - [x] D123 added to `04-risks/decisions-log.md`.
  - [x] Diary entry `plan-2026-05-14-execution/02-snapshot-
        port-phase1.md`.

Phase 2+ acceptance lives in §6 above.
