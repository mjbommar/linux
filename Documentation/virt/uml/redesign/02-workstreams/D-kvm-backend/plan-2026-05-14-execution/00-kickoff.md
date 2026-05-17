# PLAN-2026-05-14 execution — kickoff diary

Goal: complete all vision/goals from
`Documentation/virt/uml/redesign/06-sequencing/PLAN-2026-05-14.md`.

## Kickoff state (2026-05-14)

- HEAD `1ff6f3a34715` on `umlctl-deploy`, pushed.
- Substrate proven via 2 h soak: kvm-v2 200/200 = 100 % on
  memcheck/iocheck/stress-ng/tier1-pylibs.
- T57 Phase A landed; T55 closed at gate-ceiling.
- Phase J: daemon + Tier 1 done; Tier 2/3/LTP + 24 h pending.
- Time-machine: NOT STARTED on v2; v1-archive primitives intact.

## Execution order

Per PLAN §8 Month 1, sequenced for this session:

1. **Polish quick wins** (Track C). The 2-line iocheck timeout
   (#23), branch hygiene + worktree cleanup. Day 0-1.

2. **Tier 2 `uv`** (Track A §3.1). Install `uv`, warm cache,
   write `tier2-uv-*` templates + scoreboard validation. Day 1.

3. **Numpy retest under `uv`** — close #17 by-construction if
   the isolated venv path resolves the import-chain bug. Day 1.

4. **Start #168 snapshot port** (Track B §4.1). The kernel-side
   strategic lift. Sub-sequencing inside #168:
   - Port `struct kvm_snapshot` + alloc/free skeleton (~100 LoC).
   - Implement `kvm_snapshot_capture_regs_only()` + unit test
     (~150 LoC).
   - Full `kvm_snapshot_capture()` (~300 LoC).
   - `kvm_snapshot_restore_full()` (~200 LoC).
   - KUnit cases (~100 LoC).
   - Selftest re-plumbing (~50 LoC).
   - Memo `26-snapshot-v2-port.md`.
   Multi-day; expect 1-2 weeks of focused effort across these.

5. **Tier 3 + LTP** (Track A §3.2/§3.3). Designed; needs
   `CONFIG_UML_NET_VECTOR` rebuild + TAP IP resolution + LTP
   curation walk. ~1-2 weeks. Can parallelise with #168.

6. **Pre-stage Series 1 cover letter** for operator sign-off
   (Track D §6.1). Operator sends to LKML; my role is packaging.

## Diary cadence

One entry per substantive milestone or surprise. Files numbered
NN-<topic>.md in this directory. The diary is the operational
record between commits.

## Commit/push cadence

Every functional milestone gets its own commit. Push after every
commit. Use the existing decisions-log for any architectural choices
that surface during execution.
