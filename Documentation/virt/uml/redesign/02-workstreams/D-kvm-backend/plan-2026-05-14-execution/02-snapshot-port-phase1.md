# Snapshot v2 port — Phase 1 landed (2026-05-16)

Track B / `#168` snapshot port — Phase 1 + design memo committed.
Sub-sequencing source: PLAN-2026-05-14 §4.1, step 1+2 of the
seven-step ladder.

## What

Strategic Time-machine lift. Snapshot/capture/restore primitives
are the foundation `#169` (record/replay) and `#170` (syzkaller
backend) build on. v1 had a working 647 LoC implementation at
`arch/um/backend/kvm-v1-archive/snapshot.c`; this Phase 1
brings the public surface up against the v2 backend's vCPU-pool
+ singleton-memslot shape.

## What landed

  - **Design memo** `02-workstreams/D-kvm-backend/26-snapshot-
    v2-port.md` (~530 lines). Documents the v1 → v2 deltas, the
    Phase 1 design, the Phase 2-7 sub-sequencing, and the open
    questions (cross-task semantics, AMX, NMI shape, etc).
  - **`arch/um/backend/kvm-v2/snapshot.c`** (~280 LoC). Implements:
      * `kvm_v2_snapshot_alloc` / `_destroy` / `_free`
      * `kvm_v2_snapshot_capture_regs_only` (Phase 1 main payload)
      * `kvm_v2_snapshot_restore_full` (Phase 1 main payload)
      * `kvm_v2_snapshot_capture` (STUB → `-EOPNOTSUPP`, Phase 3)
  - **Header `kvm_v2_backend.h`** — `struct kvm_v2_snapshot` +
    prototypes + `KVM_V2_SNAPSHOT_MSR_COUNT`.
  - **Makefile** — `snapshot.o` under `CONFIG_UM_BACKEND_KVM_V2`.
  - **KUnit case `test_kvm_v2_snapshot_basic`** in
    `test_byteshape.c` — capture / mutate vCPU RAX / restore /
    assert round-trip. `kunit_skip` when the vCPU pool isn't up
    (build-time KUnit env without a real v2 boot).

## v1 → v2 deltas the port handles

  1. **Per-task vCPU lookup → per-host-CPU pool walk.** v1 had
     `current->thread.arch.kvm.vcpu` directly; v2 walks `vcpus[]`
     for `vcpu->last_task == current` under `preempt_disable`,
     falling back to the per-host-CPU pool entry when no slot
     matches yet. Memo §3.1 / §4.3.
  2. **`KVM_GET_FPU` → `KVM_GET_XSAVE`.** SMP-T57 Phase A
     (commit `ab68bf077de3`) enabled CR4.OSXSAVE + XCR0.YMM, so
     YMM upper-128 state lives in the XSAVE extended area, NOT
     the legacy 512 B FXSAVE shape. Snapshot uses the XSAVE
     ioctls so the round-trip preserves AVX state.
  3. **New: `KVM_GET_XCRS` capture.** v1 didn't capture XCR0
     (always zero). v2 must so restore lands on a vCPU whose
     XSAVE-area interpretation matches the captured bytes.
  4. **SMP-T55 dirty-FPU + cross-task invariants.** Post-restore
     hygiene clears `vcpu->last_task`, `vcpu->last_mm`,
     `vcpu->fpu_owner_task` and sets `vcpu->fpu_dirty = true` so
     the next dispatch's cross-task arrival branch fires and
     unconditionally re-installs SREGS + FPU from the restored
     state. The lazy fast paths re-arm on the dispatch after.

## What's deferred (sub-sequenced in the memo)

  - **Phase 2** — stand up a UML KUnit env that runs a v2 boot
    end-to-end so the KUnit case actually asserts (currently
    `kunit_skip`s under the build-only gate).
  - **Phase 3** — `kvm_v2_snapshot_capture` full path: memslot
    memcpy + per-VM IDT/GDT capture + per-vCPU IST/TSS/gadget
    capture. ~300 LoC. Includes the explicit
    `kvm_v2_tlb_kick_others` call after memslot rewrite (memo
    §3.6).
  - **Phase 4** — cross-task snapshot semantics + `fpu_owner_
    task` capture for record/replay. ~150 LoC.
  - **Phase 5** — bench harness port (cmdline + debugfs
    entry from `kvm-v1-archive/snapshot.c:412-647`). ~250 LoC.
  - **Phase 6** — selftest re-plumb (`tools/testing/selftests/um/
    snapshot-{kvm-,}smoke`). ~50 LoC.
  - **Phase 7** — record/replay foundation (#169) builds on top.
    Separate memo `27-record-replay-v2-port.md` will track it.

## Build verification

```
make ARCH=um O=/home/mjbommar/src/uml-builds/uml-smp-t41fix -j$(nproc)
-> Exit 0
-> No warnings on the touched files (snapshot.c, header,
   test_byteshape.c, Makefile)
```

Also verified with `CONFIG_UM_BACKEND_KVM_V2_KUNIT=y` in a
scratch build dir — `test_byteshape.o` builds cleanly with the
new snapshot case included.

## checkpatch

```
git diff HEAD | scripts/checkpatch.pl --no-tree --strict
-> 1 warning: "added, moved or deleted file(s), does MAINTAINERS
   need updating?" (generic new-file warning — kvm-v2/ has no
   per-file MAINTAINERS entry; the UM arch entry covers it)
-> 0 errors
-> 0 strict warnings
```

## NOT done (Phase 1 gate)

  - **No kernel boot.** Per the Phase 1 acceptance gate from
    PLAN-2026-05-14 §4.1, this commit is build-only. The KUnit
    case is `kunit_skip`-aware so it's safe under both a
    build-only env (current) and a future Phase 2 boot-time
    env.
  - **No selftest run.** v1-era `snapshot-{kvm-,}smoke` selftests
    haven't been re-plumbed yet (Phase 6).
  - **No bench numbers.** Phase 5 picks up the v1
    `kvm_snapshot_bench` cmdline + debugfs harness.

## Refs

  - D123 (this port decision, just landed in `decisions-log.md`).
  - D119 (SMP-T55 — `fpu_dirty` + `fpu_owner_task`).
  - D121 (SMP-T57 Phase A — XSAVE plumbing).
  - PLAN-2026-05-14 §4.1 (#168 seven-step sub-sequencing).
  - Memo `12-snapshot-forkserver-kvm.md` (original design).
  - Memo `26-snapshot-v2-port.md` (this port's design, landed
    alongside the code).
