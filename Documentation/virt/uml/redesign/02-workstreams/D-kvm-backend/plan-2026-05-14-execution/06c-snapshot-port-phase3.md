# Snapshot v2 port — Phase 3 landed (2026-05-16)

Track B / `#168` snapshot port — Phase 3 implements the full
capture/restore primitive: `kvm_v2_snapshot_capture_full` walks
`vm->memslots` under `vm->lock` and copies each slot's bytes into a
per-entry `kvmalloc`'d buffer; `kvm_v2_snapshot_restore_full_vcpu`
memcpy's those bytes back and pushes the regs / sregs / xsave / xcrs
/ events / msrs through `KVM_SET_*`. The Phase 1 wrappers
(`kvm_v2_snapshot_capture` and `kvm_v2_snapshot_restore_full` with
NO explicit vcpu argument) keep working — they forward to the
new helpers with `vcpu = NULL`, which falls back to
`kvm_v2_snapshot_pick_vcpu()`.

Sub-sequencing source:
`02-workstreams/D-kvm-backend/26-snapshot-v2-port.md` §Phase 3.

## What landed

  - **`arch/um/backend/kvm-v2/snapshot.c`** (399 → 759, +360):
    - `kvm_v2_snapshot_capture_full(snap, vcpu)` — new public.
      Captures regs+sregs+xsave+xcrs+events+7 MSRs (factored into
      the static helper `kvm_v2_snapshot_capture_vcpu_state` so the
      regs-only path reuses it) PLUS per-memslot data.
    - `kvm_v2_snapshot_restore_full_vcpu(snap, vcpu)` — new public.
      Restores the memslot bytes first (so descriptor-table pages
      are valid before SREGS reads sregs.idt/.gdt/.tr), then runs
      the KVM_SET_* sequence (factored into
      `kvm_v2_snapshot_restore_vcpu_state`).
    - Static helpers:
      - `kvm_v2_snapshot_capture_memslots` — two-pass walk
        (count under vm->lock, then snapshot descriptors under
        vm->lock, then drop lock and alloc per-entry data buffers
        via `kvmalloc(GFP_KERNEL | __GFP_NOWARN)`).
      - `kvm_v2_snapshot_restore_memslots` — memcpy each
        captured slot back into its host_va.
    - Phase 1 `kvm_v2_snapshot_capture_regs_only` now forwards to
      the shared `_capture_vcpu_state` helper (no behaviour
      change).
    - Phase 1 `kvm_v2_snapshot_capture` / `_restore_full` now
      delegate to the `_full` / `_full_vcpu` variants with NULL.
    - `kvm_v2_snapshot_free` extended to free `snap->memslots[]`
      data buffers via `kvfree` + the array itself.
  - **`arch/um/backend/kvm-v2/kvm_v2_backend.h`** (843 → 926, +83):
    - `struct kvm_v2_memslot_snapshot` — per-slot capture entry
      with `region` (replayable KVM_SET_USER_MEMORY_REGION
      descriptor), `data` (kvmalloc'd buffer), `data_size`.
    - `struct kvm_v2_snapshot` extended with `memslots` +
      `memslot_count`. Legacy `mem_backing` / `mem_size` kept for
      ABI continuity (always NULL/0 under Phase 3).
    - New prototypes `kvm_v2_snapshot_capture_full` +
      `kvm_v2_snapshot_restore_full_vcpu`.
  - **`arch/um/backend/kvm-v2/test_snapshot.c`** (164 → 382, +218):
    - New test case `test_kvm_v2_snapshot_full`.
    - Test slot registration helper (inline in the test): allocate
      a scratch page from buddy, register it as a dedicated
      single-page memslot at GPA 4 TiB (above any plausible UML
      mem= setting → no overlap with the giant physmem slot),
      write a marker, capture_full, write garbage, restore, assert
      marker is back.
    - Includes added: `<linux/gfp.h>`, `<linux/mm.h>`,
      `<linux/slab.h>`.

## KUnit verification

Build dir: `/home/mjbommar/src/uml-builds/uml-smp-t41fix`.

Boot incantation (Phase 3 acceptance gate, per task spec — mem
bumped from Phase 2's 256M because full capture touches more
memslots; the spec calls out 512M):

```
timeout 60 .../linux backend=force=kvm-v2 mem=512M ncpus=1 \
    init=/bin/echo rootfstype=hostfs root=/dev/root rw \
    con=null con0=fd:0,fd:1 panic=-1 </dev/null
```

Result:

```
    # Subtest: kvm_v2_marshal
    # kvm_v2_marshal: pass:8 fail:0 skip:0 total:8
ok 1 kvm_v2_marshal
    # Subtest: kvm_v2_byteshape
    # kvm_v2_byteshape: pass:9 fail:0 skip:0 total:9
ok 2 kvm_v2_byteshape
    # Subtest: kvm_v2_snapshot
um: kvm-v2 snapshot: captured regs+sregs+xsave+xcrs+events+7 msrs (regs-only)
um: kvm-v2 snapshot: restored vCPU + 0 memslots
    ok 1 test_kvm_v2_snapshot_basic
um: kvm-v2 snapshot: memslot[0] slot_id=0 kvmalloc(536870912) failed; metadata-only capture for this slot
um: kvm-v2 snapshot: captured regs+sregs+xsave+xcrs+events+7 msrs + 2 memslots
um: kvm-v2 snapshot: restored vCPU + 2 memslots
    ok 2 test_kvm_v2_snapshot_full
# kvm_v2_snapshot: pass:2 fail:0 skip:0 total:2
ok 3 kvm_v2_snapshot
```

Both snapshot cases PASS (2/2). Boot proceeds normally; `/bin/echo`
runs, exits 0, kernel panics on `Attempted to kill init` (expected
under `panic=-1`).

## The hardest snapshot-restore quirk Phase 3 hit

**The giant physmem memslot copy is fundamentally infeasible at
typical UML configs.** The Phase 3 memo specifies "copy on capture
(simpler); COW deferred to Phase 4" — and v1's archive used
`kvmalloc(physmem_size)` with the same scheme. But on a `mem=512M`
config the kernel has 512 MB of physmem TOTAL, and a single
`kvmalloc(512M)` cannot fit another 512 MB buffer alongside the
running kernel + buddy + everything else. The `kvmalloc` path tries
`kmalloc` first (fails — no contiguous 512 MB physical block) then
falls back to vmalloc (fails — UML's `VMALLOC_QUARTER_SIZE` doesn't
budget for a snapshot-of-physmem allocation that's the same size as
physmem itself).

v1's snapshot.c at `kvm-v1-archive/snapshot.c:200` had the same
issue but was only ever exercised under tiny `mem=64M` / `mem=128M`
configs where the math marginally worked. Phase 3's selftest needs
a passing path on the standard `mem=512M` build the rest of the
backend-v2 work assumes.

**The fix:** capture_full graceful-degrades on per-slot allocation
failure. Instead of failing the whole capture, the slot's metadata
(region descriptor) is preserved but `data = NULL`. The matching
restore_memslots step skips entries with `data == NULL`. This keeps
Phase 3 useful for the realistic case (multiple small slots, e.g.
the per-region slots region.c will reinstate post-"Codex CLAIM C"
decision reversal) while admitting that the whole-physmem
checkpoint really does need Phase 4's dirty-bitmap optimization to
fit on a real config.

The KUnit test then registers a dedicated single-page memslot at
GPA 4 TiB (above any plausible UML mem= → no overlap with the giant
physmem slot) and runs the round-trip against THAT slot. The giant
physmem slot's metadata still flows through capture (so we see 2
slots in the post-capture log: physmem + test) but only the test
slot's PAGE_SIZE buffer is small enough to allocate. The
`memslot_data_count > 0` assertion in the test gates this:
PAGE_SIZE always fits, regardless of mem=.

**A secondary quirk** the implementation also navigates: the
`kvm_xsave` struct is 4 KB on its own. The first cut of
`_restore_vcpu_state` took stack-local copies of every snapshot
field (regs + sregs + xsave + xcrs + events + msrs) to handle the
const-correctness mismatch between `const struct kvm_v2_snapshot
*snap` and `os_ioctl_generic(..., unsigned long arg)`. That blew
`-Wframe-larger-than=` at 5136 bytes. Fix: cast through `(unsigned
long)` directly — C-level const erases at that cast and the
underlying ioctl path doesn't actually modify the buffer. Documented
in the helper's comment block so a future maintainer doesn't
re-introduce the stack copies.

## v1 → v2 invariants Phase 3 documents

  - **vm->memslots traversal under spinlock**: capture walks the
    list under `scoped_guard(spinlock, &vm->lock)` twice — once to
    count, once to snapshot descriptors. The per-slot data buffer
    alloc + memcpy run outside the lock (kvmalloc may sleep,
    memcpy is unbounded). Memslot stability across the lock drop
    is the caller's responsibility — Phase 3's KUnit usage is
    single-threaded so this is moot, but Phase 4's record/replay
    callers will need an "all vCPUs quiesced" predicate (memo §3.6
    / §Q5).
  - **Memslot metadata is part of the snapshot**: even slots whose
    data couldn't be allocated still have their region descriptor
    (slot_id / gpa / hva / size / flags) captured. A future Phase
    4 restore could replay `KVM_SET_USER_MEMORY_REGION` from these
    descriptors to re-establish slots that were removed between
    capture and restore. Phase 3 doesn't do this replay (the
    memslots are still registered at restore time because the test
    runs in a single boot session), but the descriptor capture is
    the foundation.

## What Phase 3 does NOT do

  - **No COW / dirty-bitmap optimization** — the giant physmem
    slot's contents aren't captured under realistic mem= configs.
    Phase 4 (#168 sub-task).
  - **No cross-vCPU snapshot semantics** — restore_full_vcpu's
    cpuid_primed clear is conservative (doesn't fire for same-vCPU
    restore which is all the KUnit test does). Phase 4 (#169
    deterministic record/replay needs this).
  - **No bench harness** — Phase 5.
  - **No selftest re-plumbing** — Phase 6.

## Acceptance for Phase 3

  - [x] `struct kvm_v2_memslot_snapshot` + the `memslots` /
        `memslot_count` fields in `struct kvm_v2_snapshot`.
  - [x] `kvm_v2_snapshot_capture_full(snap, vcpu)` IMPLEMENTED.
  - [x] `kvm_v2_snapshot_restore_full_vcpu(snap, vcpu)` IMPLEMENTED.
  - [x] Phase 1 wrappers (`kvm_v2_snapshot_capture` and
        `_restore_full`) keep working via delegation.
  - [x] `kvm_v2_snapshot_free` extended to free `memslots[]`.
  - [x] `test_kvm_v2_snapshot_full` exists and PASSES.
  - [x] `test_kvm_v2_snapshot_basic` still PASSES (no regression).
  - [x] Build clean under `make ARCH=um O=/home/mjbommar/src/uml-
        builds/uml-smp-t41fix linux`.
  - [x] Boot under `backend=force=kvm-v2 mem=512M ncpus=1`: case
        PASSES (2/2 in kvm_v2_snapshot suite).
  - [x] checkpatch clean (`scripts/checkpatch.pl`) on the diff.
  - [x] Diary entry (this file).

## LoC delta

  - `snapshot.c`        : +360
  - `test_snapshot.c`   : +218 (heavy doc)
  - `kvm_v2_backend.h`  : +83

Net: +661 LoC, of which +578 is in production text (snapshot.c +
header) and +218 is the test TU expansion (new full-round-trip
case + supporting infra to register the dedicated test slot).
Heavy on the doc side per the kvm-v2 codebase style.

## Recommendation for Phase 4

**Phase 4 should land COW / dirty-bitmap memslot optimization BEFORE
record/replay (#169).** Two reasons:

  1. Phase 3 has a known gap: the giant physmem slot's data isn't
     captured under realistic `mem=` configs. Until COW or
     `KVM_GET_DIRTY_LOG`-based delta capture lands, the snapshot
     can't actually round-trip whole-guest state — only small
     per-region slots. Record/replay (#169) presupposes a working
     whole-guest snapshot as its initial checkpoint (memo 13
     §3.1's "checkpoint then replay deltas"); without COW it would
     inherit Phase 3's gap.
  2. The COW / dirty-bitmap path is independently useful for the
     fuzz/forkserver use-case (memo 12 §3.4's "<1ms iter target"),
     which is the original `#168` motivation. Record/replay can be
     scheduled separately once the snapshot primitive is whole.

The Phase 4 work is essentially: enable `KVM_MEM_LOG_DIRTY_PAGES`
on the giant physmem slot, capture the dirty bitmap at the start
of each iteration, and on restore only memcpy the dirty pages back
(plus reset the dirty bitmap). Memory cost drops from O(mem) to
O(per-iter dirty footprint), which on the fuzzer workload is
typically <1 MB / iter. v1 archive's `record.c` has the
`KVM_GET_DIRTY_LOG`-based pattern but never landed on the snapshot
side; Phase 4 ports both halves at once.

The "cross-task semantics" sub-piece (memo §Phase 4) — capture
`fpu_owner_task` + per-task `iotrap_fpu` so restore re-binds — is
NOT required for #168's COW work but IS required for #169's
record/replay. Schedule it WITH the record/replay work, not
before.

## References

  - `02-workstreams/D-kvm-backend/26-snapshot-v2-port.md` §Phase 3
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/
    06b-snapshot-port-phase2.md` (the Phase 2 diary)
  - Phase 1 commit `aa4cd328102c`
  - Phase 2 commit `9baf6a1e9838`
