# Record/replay v2 port — design memo landed (2026-05-16)

Track B / `#169` record/replay port — design memo committed.
Sibling to `02-snapshot-port-phase1.md` (#168, snapshot Phase 1
landed at commit `aa4cd328102c`). Sub-sequencing source:
PLAN-2026-05-14 §4.1, the #169 ladder.

## What

Strategic Time-machine lift. Record/replay primitives are the
layer above snapshot — they capture every source of host-side
nondeterminism (RDTSC, vvar, RDRAND, syscall results, signal
timing) at record time so replay can feed them back byte-
identically. v1 had a working 1597 LoC implementation at
`arch/um/backend/kvm-v1-archive/record.c` through memo 13's
Step 1+2+3+3.5+5 (per-NR routing covering 5 NRs, debugfs
control, strict + loose replay, KUnit round-trip). This memo
designs the port onto v2's vCPU pool + LSTAR gadget +
signal-queue shape.

## What landed

  - **Design memo** `02-workstreams/D-kvm-backend/27-record-
    replay-v2-port.md` (~1010 lines). Documents the v1 → v2
    deltas, the seven-phase sub-sequencing, the open
    questions (RDTSC API, PMU silicon coverage, cross-kernel
    replay, #170 unblock gate, signal precision), and the
    Phase 1 entry-point design.

## v1 → v2 deltas the port handles (memo §3)

  1. **Observe hook site.** v1: `kvm_decode_syscall` in
     `syscall_class.c`. v2: post-`handle_syscall` in
     `kvm_v2_handle_io_trap` (`syscall_trap.c:2110+`), gated
     by `static_branch_unlikely(&um_kvm_v2_record_enabled)`.
  2. **Gadget-shadowed syscalls.** v2's LSTAR gadget services
     11 hot-path NRs (getpid family + clock_gettime) without
     a vmexit. Record mode forces gadget-disable via a per-
     vCPU `KVM_V2_GADGET_OFF_RECORD` byte; the gadget body
     reads the byte at `gadget_entry` and branches to the
     fallback path.
  3. **RDTSC trap.** v2 passes RDTSC through. Record-mode
     traps via `KVM_X86_*_RDTSC_EXIT` (API choice open — Q1
     in §6).
  4. **vvar determinism.** Capture seqlock-protected vvar
     fields on each refresh via a host vdso_update tail hook;
     restore on replay before each guest entry.
  5. **SIGALRM determinism.** Syscall-count-driven injection
     (decision (b) per memo §3.6) — coarser than memo 13's
     PMU-driven (a), but doesn't require Zen 4 PMU validation.
  6. **XSAVE under SMP-T55.** Gate the lazy-FPU skip on
     `!um_kvm_v2_record_enabled` so record-mode always re-
     installs FPU. Defeats SMP-T55 only when record is on.
  7. **Memslot deltas.** Phase 1 mirrors v1 — full restore
     per replay iteration. Dirty-bitmap optimization deferred.

## Sub-sequencing (memo §5)

  - **Phase 1** — skeleton + static_key + alloc/destroy.
    ~350 LoC. Lands next commit.
  - **Phase 2** — `observe_syscall` hook in
    `handle_io_trap`. Inline-only path. Gates SMP-T55 lazy-
    FPU skip on the static_key.
  - **Phase 2.5** — per-NR side-buffer routing
    (getrandom/read/pread64/recvfrom/readv).
  - **Phase 3** — replay primitive (`consume_syscall` +
    payload restore). The big acceptance: 100-syscall record
    → replay = bit-identical state.
  - **Phase 4** — gadget-disable in record mode (per-vCPU
    flag byte in the gadget state page).
  - **Phase 5** — RDTSC + vvar determinism.
  - **Phase 6** — SIGALRM determinism (syscall-count-driven
    injection). Stress-ng IPC acceptance.
  - **Phase 7** — KUnit + selftest re-plumb.

## Open questions (memo §6)

  - **Q1.** KVM API for RDTSC exit configuration —
    `KVM_TSC_CONTROL` (TSC offset) vs explicit per-vCPU exit
    toggle. Phase 5 spike resolves.
  - **Q2.** PMU instruction-retired counter availability under
    Zen 4 vs Intel — affects Phase 6.5/7 if (b)'s precision
    proves inadequate.
  - **Q3.** Cross-kernel-version replay with per-pool vCPU
    layout drift — record format must version-tag the
    snapshot bytes; refuse cross-version replay v1.
  - **Q4.** #170 syzkaller fork-server minimum shape — needs
    snapshot Phase 3 + record/replay Phase 2 + Phase 4
    (gadget-disable for clock_gettime visibility).
  - **Q5.** Sub-instruction-precision signal arrival — (b)
    achieves "±100 syscalls"; sufficient for fuzzing /
    debugging, inadequate for tight benchmarks.

## checkpatch

```
./scripts/checkpatch.pl --no-tree -f \
    Documentation/virt/uml/redesign/02-workstreams/\
D-kvm-backend/27-record-replay-v2-port.md
-> 0 errors, 0 warnings, 1012 lines checked
-> "ready for submission"
```

## NOT done (gate for this commit)

  - **No code.** Pure design memo. Phase 1 (skeleton +
    static_key + alloc/destroy + state machine) lands as a
    separate commit.
  - **No KUnit.** Lands with Phase 1.
  - **No selftest re-plumb.** Phase 7.
  - **No `kvm-v2/` source changes.** Constraint per the
    execution-prompt — another sub-agent is doing snapshot
    Phase 2 KUnit work there.
  - **No `upstream-patches/kvm-backend-series/` changes.**
    Constraint per the execution-prompt — another sub-agent
    is rewriting the Series 7 cover letter.

## Refs

  - D101-D105 (v1 record/replay landings — `decisions-log.md`).
  - D119 (SMP-T55 — `fpu_dirty` + `fpu_owner_task`).
  - D121 (SMP-T57 Phase A — XSAVE plumbing).
  - D123 (snapshot v2 port Phase 1).
  - PLAN-2026-05-14 §4.1 (#169 seven-step sub-sequencing).
  - Memo `13-record-replay-determinism.md` (the strategic
    pitch this port implements).
  - Memo `26-snapshot-v2-port.md` (the sibling port memo
    structural pattern).
  - Memo `27-record-replay-v2-port.md` (this port's design,
    landed alongside this diary entry).
