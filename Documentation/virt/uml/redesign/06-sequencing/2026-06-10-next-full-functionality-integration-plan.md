# UML v2 next Full Functionality Integration Plan

Date: 2026-06-10

Branch target: `next`

Primary objective: make `next` the single authoritative UML v2 branch that
contains all intended functionality from the active and historical UML work,
with clean implementation, working user-facing tools, matching documentation,
and validation strong enough to declare the original plan complete.

## Executive Summary

The current `next` branch is cleanly based on Linus' tree and contains the
main KVM v2 core, KVM v2 snapshot source, vector2, launcher tooling,
syzkaller shim, and a large selftest/documentation surface. It is not yet the
complete UML v2 branch.

Several historical branches contain functionality that is either absent from
`next`, only partially represented in `next`, or represented in `next` by
tools/docs whose kernel-side implementation was later removed. The historical
branches also contain research notes, internal issue numbers, phase diaries,
branch-specific commit IDs, and debug/investigation code that should not be
merged directly.

The integration strategy is:

1. Keep `next` as the authority.
2. Inventory every historical feature and give it an explicit disposition.
3. Fix current `next` correctness and broken user-facing surfaces first.
4. Port or reimplement missing functionality into clean feature branches.
5. Land each feature into `next` only with tests and documentation that match
   the actual implementation.
6. Remove or archive stale docs, CLI surfaces, and selftests that refer to
   absent functionality.
7. Finish with one full integration gate that proves build, tests, tools,
   networking, snapshot/fork/pool, and KVM v2 behavior together.

## Current Plan State

This section is the short, current reading order for the plan after the
2026-06-10 pool replication, warm-ready, and sparse-copy updates.

Functional baseline facts:

- `next` is the active integration branch.
- The functional baseline for this plan update is `71eda3d9c0df`.
- At that baseline, `next` and `origin/next` both pointed at
  `71eda3d9c0df`.
- At that baseline, `next` was `0` commits behind and `64` commits ahead of
  the local `torvalds/master` ref used for the upstream comparison.
- Documentation-only plan commits may sit above this functional baseline.
- The latest landed pool commits are:
  - `a291aa71748c` - route the pool daemon to replicated members;
  - `f6dcf99b5c89` - add warm ready pool members;
  - `0da923133d82` - document the full pool benchmark result; and
  - `71eda3d9c0df` - sparse-copy pool member physmem.

Closed or substantially closed since the initial 2026-06-10 review:

- KVM v2 snapshot capture, restore, and ELF export are present on `next`,
  validated by KUnit, live mconsole-backed export, GDB/readelf parsing, and
  snapshot kselftest wrappers.
- Snapshot SMP behavior is explicitly bounded: capture and restore reject
  guests with more than one online CPU until all-vCPU quiescence exists.
- The known KVM v2 architectural restore error-handling hole is fixed for the
  reviewed restore paths.
- Template-pause single-shot, pivot, fork-on-resume smoke, fork stress,
  pool-member one-shot, and replicated sustained pool-member lifetime have
  all passed on the current production path.
- `umlctl pool serve` now uses replicated pool-member mode and returns live
  quiesced members; daemon-routed `exec` resumes a member before waiting for
  mconsole.
- `pool serve --min-warm` now keeps daemon-assigned ready members, exposes
  ready/taken/failed/min-warm status, supports `pool take --ready`, and
  cleans up ready members during destroy/shutdown.
- Full default `pool-bench` now passes all five gates with live quiesced
  replicated children: p50 1.8 ms, p99 2.5 ms, 100/100 live members at
  139.0 MiB PSS, 499.3 MiB summed RSS, and 17.6 MiB private dirty, 0.05%
  lifecycle drift, and 3000/3000 throughput takes in the 60-second gate.
- The forced-KVM dynamic-userspace blocker is closed at the focused smoke
  level. `/bin/true` now reaches the expected clean init-exit panic with
  `exitcode=0` under `backend=force=kvm`, and the dyn-loader kselftest's KVM
  row passes. The concrete bug was the user segment refresh running after the
  FS/GS-base write in `kvm_v2_load_user_sregs()`, replacing the segment cache
  with flat descriptors whose bases were zero.

The active blockers are now:

1. Keep the final daemon-routed guest exec ABI explicit. The current smoke proves
   successful commands through the final member mconsole path: `/bin/true`
   exits 0, captured stdout/stderr round-trip through NDJSON, guest exit code
   7 is preserved as a normal exec result rather than a daemon error, and a
   one-second timeout returns exit code 124 with `timed_out=true`, no late
   stdout, and no leaked guest `sleep` helper. `exec/1` is the current public
   ABI: callers send structured argv/env/cwd/timeout to the daemon and receive
   NDJSON frames. The mconsole backend deliberately lowers that request through
   a bounded shell command, and `--timeout` requires guest `timeout(1)`. A
   stricter kernel argv transport is future `exec/2` work, not required for the
   current completion claim. A naive child-side mconsole rebind was tested
   locally and rejected because it panicked before the member reached
   `MEMBER_DONE`; see `2026-06-10-pool-mconsole-exec-investigation.md`. The
   checked-in
   `pool-mconsole-path-probe` now passes as a focused socket-addressability
   gate.
2. Keep the final warm scheduling contract explicit. Request-specific takes
   stay lazy because MAC/TAP/IP/mconsole identity is applied before fork.
   `pool take --ready` is the separate daemon-assigned ready-member mode and
   cannot be combined with caller-supplied identity. Syzkaller remains on the
   request-specific lazy path because it needs deterministic TAP/IP identity.
3. Keep the historical per-take pool fd handoff decision explicit: it is
   retired from the current completion claim in favor of the validated pool TAP
   reopen path. Vector2 sandbox audit, launcher-owned fd handoff, and
   pool-member TAP handoff now have live smoke gates; the remaining vector2
   networking gates still need multiqueue/fairness, Tier 3 seccomp, and KVM v2
   coverage.
4. Complete live record/replay before counting it in the original completion
   claim. The experimental Kconfig-gated core, syscall-log state machine, and
   gadget bypass are present, and UML time-travel clock events now round-trip
   through the record log, but raw time/RDTSC, signal, device, and
   deterministic workload recording/replay are still open.
6. Decide whether the historical KVM v2 private state trace should be imported
   as clean optional diagnostics.
7. Curate source comments, selftests, reports, and status docs so upstream-
   facing code is free of internal issue numbers, phase diaries, random
   history, and stale claims. A focused active-source pass has removed the
   remaining internal audit label and temporary-policy comments found in
   active UML source/selftest/launcher paths. The May 2026 report/deck
   workspace is now explicitly archived and rebuilt with archive notices; any
   future report refresh must freeze a new `next` cutoff and regenerate data
   from the live inventory.

Execution discipline for the remaining work:

- Land one coherent workstream at a time on top of current `next`.
- Run the smallest meaningful test gate before each commit, and the broader
  gate before declaring a workstream closed.
- Update this plan, the inventory, and `STATUS.md` when a user-visible state
  changes.
- Commit and push `next` after each validated workstream.
- Do not direct-merge historical branches; port or reimplement their useful
  functionality with normal kernel comments and tests.

## Source Material Reviewed

This plan treats the following documents and branch families as source
material, not as automatically-current truth:

- `Documentation/virt/uml/redesign/00-vision.md`: original mission,
  success criteria, profiles, record/replay, snapshot/forkserver, KVM backend,
  instrumentation, and syzkaller goals.
- `Documentation/virt/uml/redesign/02-workstreams/`: architectural and
  workstream decomposition for backend abstraction, static keys, profiles,
  KVM v2, instrumentation, and validation.
- `Documentation/virt/uml/redesign/03-profiles/`: intended profile surface
  for research, fuzz, fuzz-deep, prod-fast, prod-with-hooks, sandbox,
  embedded, library, and time-travel builds.
- `Documentation/virt/uml/redesign/05-validation/a-plus-quality-plan.md`:
  quality bar for builds, static analysis, runtime validation, sanitizer
  profiles, KUnit, selftests, and performance evidence.
- `Documentation/virt/uml/redesign/report-presentation/PLAN.md`: reporting
  rule that vision, landed status, and evidence must be normalized before
  making completion claims.
- `Documentation/virt/uml/redesign/STATUS.md`: current top-level readiness
  view.
- `Documentation/virt/uml/redesign/06-sequencing/2026-06-10-next-functionality-inventory.md`:
  live feature-by-feature tracker for this plan.
- Historical branches listed below: evidence and source material for missing
  behavior, not merge bases.

The rule for conflicting documents is simple: implementation and fresh
validation on `next` override old reports, phase notes, presentations, and
historical branch status. Old material remains useful only after it is either
ported, retired with approval, or clearly marked historical.

## Definition Of Completion

`next` can be called complete only when all of the following are true:

- Every intended UML v2 feature from the original mission and historical UML
  branches is either implemented on `next` or explicitly retired in a dated
  historical note with rationale.
- No working feature exists only on a historical branch.
- No user-facing command documents or invokes a kernel/debugfs/API surface that
  is absent from `next`.
- KVM v2 core runs with checked architectural-state restore paths.
- Snapshot, snapshot ELF export, record/replay, fork-server, pool, vector2,
  syzkaller integration, profile configs, and launcher workflows have matching
  code, tests, and docs.
- Upstream-facing source and tests are free of random history, diary prose,
  internal issue numbers, stale phase notes, and branch-specific commit IDs.
- Historical and diagnostic material remains available only in clearly marked
  archival documentation.
- The final validation matrix passes or has explicitly documented, bounded,
  non-blocking skips.
- `next` is pushed to GitHub after the final integration.

## Current Calibration

This plan is based on the 2026-06-10 review of `next` and the broader UML
historical branches.

Current branch status at review time:

- Branch: `next`
- Remote tracking: `origin/next`
- Functional baseline: `71eda3d9c0df`
- Relative to local `torvalds/master` at the functional baseline: `0` behind,
  `64` ahead
- Baseline update: pool daemon live-member routing, warm-ready members, sparse
  physmem copying, quiesced returned members, and the full PSS-gated pool
  benchmark are landed and validated on `next`.

Approximate changed review surface relative to Linus:

- UML-related source/config/script files: about 487
- UML documentation files: about 442
- KVM v2 backend files: 17
- Vector2 files: 29
- Launcher Rust source files: 49
- UML selftest files: about 249

This is too large to treat as a single cleanup patch. It needs staged
integration with explicit acceptance gates.

## Historical Branch Inventory

### `next`

Current role:

- Authoritative integration branch.
- Contains cleaned KVM v2 core and the reimported snapshot
  capture/restore/export source.
- Contains an experimental record/replay core behind
  `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL`, but not full
  deterministic runtime replay.
- Does not yet contain private state trace source.
- Contains vector2 implementation.
- Contains launcher, pool, exec, port-forward, deploy, gates, and examples.
- Contains syzkaller UML shim.
- Contains broad selftests and redesigned documentation.

Disposition:

- Keep as the target branch.
- Fix correctness and surface mismatches before importing more code.

### `kvm-v2-snapshot-elf64`

Current role:

- Historical KVM snapshot and snapshot ELF export branch.
- Contains `arch/um/backend/kvm-v2/snapshot.c`.
- Contains `arch/um/backend/kvm-v2/snapshot_elf.c`.
- Contains `arch/um/backend/kvm-v2/record.c`.
- Contains `arch/um/backend/kvm-v2/state_trace.c`.
- Contains snapshot and record KUnit tests.
- Contains snapshot ELF docs and tooling assumptions.

Problems:

- Source comments include internal issue numbers, memo references, phase logs,
  branch-specific commit IDs, and investigation narrative.
- Some functionality is prototype-quality or phase-scoped.
- Snapshot capture, snapshot restore, and snapshot ELF export started as
  historical-only functionality and have now been reimported into `next` with
  KUnit, live export, and restore smoke coverage. SMP semantics are explicitly
  gated to one online CPU.
- Record/replay is now partially present as an experimental core on `next`;
  private state trace functionality remains historical-only.

Disposition:

- Do not merge wholesale.
- Port or reimplement feature-by-feature onto `next`.
- Rewrite comments and docs into normal kernel style.
- Preserve historical rationale only in archival docs.

### `fork-server-phase1c`

Current role:

- Historical fork-server/template-pause phase branch.
- Contains earlier fork-server status and validation state.
- Overlaps with current `next` template-pause/pool work.

Problems:

- Phase branch status does not represent final integrated behavior.
- Some code/tests are superseded by later `memo09-*` and current `next`.

Disposition:

- Use as evidence for missing behavior and tests.
- Do not merge directly.

### `memo09-phase2`

Current role:

- Historical identity-apply / fork-server phase.

Problems:

- Narrow phase branch, not complete product state.

Disposition:

- Compare identity blob semantics, guest apply path, and tests against `next`.
- Port only missing behavior.

### `memo09-phase3-pool-bench`

Current role:

- Historical pool benchmark and pool acceptance branch.

Problems:

- Some current `next` pool modules are newer.
- Some older branch deltas delete user-facing modules now present on `next`.

Disposition:

- Use primarily for benchmark logic and acceptance thresholds.
- Port missing tests/metrics, not whole tree state.

### `memo09-phase4`

Current role:

- Later historical pool/fork-server branch.
- Contains additional pool, snapshot, and smoke test surfaces.

Problems:

- Still divergent from current `next`.
- Contains branch-specific test and documentation assumptions.

Disposition:

- Compare against current launcher and selftests.
- Import missing functionality only after cleaning.

### `experiment-path-c`

Current role:

- Historical path-C experiment branch.
- Contains KVM v1 archive, snapshot/record experiments, and many diagnostic
  tests.

Problems:

- It is experimental, not a clean integration base.
- Includes archived KVM v1 source and branch-specific diagnostic materials.

Disposition:

- Mine for useful tests and design lessons.
- Keep KVM v1 archive out of upstream-facing `next` unless explicitly needed
  as historical documentation.

### `umlctl-deploy`

Current role:

- Launcher/deploy/gate branch.
- Contains many launcher, deploy, soak, vector2, and test harness updates.

Problems:

- Some of it is already on `next`.
- Some files are older than current `next`.
- Large overlap makes direct merge high-risk.

Disposition:

- Diff module by module.
- Import missing launcher features and examples after running cargo tests.

## Initial `next` Blockers And Disposition

These were the main blockers from the initial 2026-06-10 review. They are kept
here because they explain the sequencing, but the current active blocker list
is in `Current Plan State` and `Immediate Next Actions`.

### KVM v2 restore ioctls drop state on failure

File:

- `arch/um/backend/kvm-v2/vcpu.c`

Problem:

- `KVM_SET_XSAVE` and `KVM_SET_VCPU_EVENTS` restore calls are cast to `void`.
- The code clears `iotrap_fpu_valid` and `iotrap_events_valid` as if restore
  succeeded.
- On failure, a task can resume with stale or missing architectural state, and
  the saved copy is lost.

Required fix:

- Check both ioctl return values.
- Treat `KVM_SET_XSAVE` failure as fatal before guest entry, matching the
  nearby FPU install failure policy.
- Treat `KVM_SET_VCPU_EVENTS` failure as fatal or return an error to the
  dispatch path before guest entry.
- Preserve saved state until restore succeeds.
- Add a KUnit or fault-injection test if practical.

Acceptance:

- Build passes.
- KVM v2 KUnit passes.
- No `(void)os_ioctl_generic()` remains for required architectural restore
  state.

Current status:

- Closed for the reviewed restore paths. Keep the broader KVM v2 ioctl audit
  in Workstream A as a hardening task, but this specific blocker is no longer
  first in the execution order.

### `umlctl snapshot export` needed a real kernel control surface

Files:

- `tools/uml/uml-launcher/src/bin/umlctl/snapshot.rs`
- `Documentation/virt/uml/snapshot-elf-format.rst`
- `tools/uml/uml-gdb/uml-snapshot.py`
- `arch/um/backend/kvm-v2/snapshot_elf.c`
- `arch/um/drivers/mconsole_user.c`
- `arch/um/drivers/mconsole_kern.c`
- `arch/um/backend/kvm-v2/Makefile`

Problem:

- Earlier `umlctl snapshot export` logic assumed the host could drive the
  guest debugfs trigger through `/proc/<pid>/root/...`.
- For UML, `/proc/<pid>/root` is the host process root, not the guest VFS.
- The kernel needed a host-side control path that does not depend on entering
  the guest filesystem namespace.

Required fix:

- Keep direct debugfs export for inside-guest scripts.
- Add an mconsole command that accepts `snapshot_export <host-path>`.
- Make `umlctl snapshot export` resolve the running instance's mconsole
  socket and request the export through that control channel.
- Write the resulting ELF through UML host-file helpers from mconsole context.

Acceptance:

- No CLI command advertises functionality that cannot exist on `next`.
- Live `umlctl snapshot export` produces a non-empty ELF file.
- `readelf -h`, `readelf -l`, `readelf -n`, `gdb -c`, and the UML gdb helper
  parse the exported file.
- Current status: PASS on 2026-06-10 in the mconsole-backed export update.

### KVM v2 gadget validation/defaults need final decision

Files:

- `arch/um/backend/kvm-v2/Kconfig`
- `arch/um/backend/kvm-v2/lstar_gadget.S`
- `arch/um/backend/kvm-v2/vcpu.c`
- KVM v2 status docs

Problem:

- Historical docs recommended `CONFIG_UM_BACKEND_KVM_V2_GADGET=n` as a
  workaround for the Django flake.
- Current docs say the XSAVE/YMM fix closed that class.
- Kconfig defaults gadget to `y`.

Required fix:

- Re-run the decisive validation on current `next`.
- If gadget is stable, update stale docs and keep default.
- If not stable, default it off or gate publication accordingly.

Acceptance:

- No contradictory docs.
- KVM v2 Tier 3 result explicitly includes gadget state.

## Workstream A: Current KVM v2 Core Hardening

Purpose:

- Make the current KVM v2 core safe enough to receive snapshot/record imports.

Tasks:

- Audit all `os_ioctl_generic()` calls in `arch/um/backend/kvm-v2`.
- Categorize each ioctl as required, optional, or debug-only.
- Check return values for every required ioctl.
- Preserve architectural state until restore succeeds.
- Confirm CPUID, XCR0, XSAVE, FPU, VCPU events, MSR, SREGS, and signal-mask
  paths have consistent failure policy.
- Review `KVM_SET_MSRS` in the LSTAR EINTR recovery path and decide whether it
  is optional or required.
- Add comments that explain invariants, not investigation history.

Acceptance gates:

- `make ARCH=um -j$(nproc)`
- KVM v2 KUnit
- KVM smoke
- CPython parity smoke
- Targeted signal/FPU regression tests

## Workstream B: KVM Snapshot And Snapshot ELF Export

Purpose:

- Bring snapshot and snapshot-to-disk functionality into `next` so the CLI,
  docs, gdb helper, and selftests match real kernel code.

Source branch:

- `kvm-v2-snapshot-elf64`

Likely source files to port:

- `arch/um/backend/kvm-v2/snapshot.c`
- `arch/um/backend/kvm-v2/snapshot_elf.c`
- `arch/um/backend/kvm-v2/test_snapshot.c`
- `arch/um/drivers/mconsole_user.c`
- `arch/um/drivers/mconsole_kern.c`
- relevant declarations in `arch/um/backend/kvm-v2/kvm_v2_backend.h`
- relevant Makefile and Kconfig entries
- snapshot selftests
- `tools/uml/uml-gdb/uml-snapshot.py` updates if needed
- `tools/uml/uml-launcher/src/bin/umlctl/snapshot.rs` updates if needed

Clean rewrite requirements:

- Remove internal issue numbers such as `#168` and `#181` from source.
- Remove phase diaries and branch-specific commit IDs.
- Replace memo references with durable design comments.
- Keep only comments that explain kernel invariants, ABI shape, locking,
  ownership, memory layout, and failure policy.
- Move historical rationale to a `HISTORICAL` redesign doc if worth keeping.

Functional requirements:

- Capture vCPU state with full XSAVE, not legacy FPU-only state.
- Capture SREGS, GPRS, XCRS, VCPU events, and required MSRs.
- Capture memslot contents safely under the right locking discipline.
- Restore memslots and vCPU state in a deterministic order.
- Provide a host-side `umlctl snapshot export` path through mconsole.
- Preserve the debugfs trigger for inside-guest scripts.
- Keep snapshot code safe when KVM v2 is compiled but not selected at runtime.
- Clearly define SMP constraints. If all-vCPU quiescence is not complete, the
  feature must be gated as single-vCPU only.

Acceptance gates:

- KVM v2 snapshot KUnit tests pass. Current status: PASS 4/4 on 2026-06-10
  with `backend=force=kvm-v2`,
  `kunit.filter_glob=um_kvm_v2_snapshot`, and `kunit_shutdown=halt`.
- Live snapshot ELF export smoke passes. Current status: PASS on 2026-06-10
  against a disposable KVM v2 hostfs guest.
- `umlctl snapshot export <instance> --output dump.elf` works. Current
  status: PASS through mconsole `snapshot_export <path>`.
- `readelf -h`, `readelf -l`, and `readelf -n` parse the file. Current
  status: PASS.
- `gdb -c dump.elf` opens the file. Current status: PASS.
- `tools/uml/uml-gdb/uml-snapshot.py` helper loads and reports state. Current
  status: PASS.
- Snapshot restore runtime smoke passes. Current status: PASS on 2026-06-10
  through `kvm-snapshot-restore-smoke` with `kvm_v2_snapshot_bench=1`.
- SMP behavior is explicitly gated to one online CPU with `-EOPNOTSUPP`.

## Workstream C: KVM Record/Replay

Purpose:

- Complete or cleanly land the record/replay functionality promised by the
  original UML v2 plan.

Source branch:

- `kvm-v2-snapshot-elf64`
- `experiment-path-c` for older experiments and tests

Likely source files to port or reimplement:

- `arch/um/backend/kvm-v2/record.c`
- record/replay declarations in `kvm_v2_backend.h`
- syscall trap hooks
- record KUnit tests
- record smoke tests
- clock/RDTSC/vvar/SIGALRM tests from historical branches

Required design decisions:

- Is record/replay part of the publishable KVM v2 core, or an experimental
  Kconfig option?
- What does "complete" mean for the first landed version?
- Is replay required to be deterministic for real Python/Tier 3 workloads, or
  only for a bounded syscall subset?
- How does record mode interact with the LSTAR gadget?
- How are time, vvar, RDTSC, SIGALRM, randomness, network, and external I/O
  represented?

Minimum complete implementation:

- Single-active-record discipline.
- Explicit start, stop, destroy, and replay state machine.
- Hot-path static key disabled by default.
- Syscall observe path with bounded buffer policy.
- Replay consume path with strict mismatch behavior.
- Gadget bypass while recording if gadget would otherwise hide syscalls.
- Snapshot integration at record start.
- Time source handling for at least the supported replay tier.
- Tests that prove record and replay against a small deterministic workload.

If full replay is not ready:

- Land behind `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL`.
- Keep user-facing docs clear that it is not mission-complete.
- Do not count original record/replay mission as complete.

Current `next` checkpoint:

- `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL` exists and defaults
  off.
- `arch/um/backend/kvm-v2/record.c` implements the single-active container,
  lifecycle, strict replay flag, bounded syscall entries, FIFO consume,
  mismatch reporting, and dropped-entry accounting.
- `arch/um/backend/kvm-v2/syscall_trap.c` now wires the live KVM syscall
  dispatcher to that core: record mode appends post-syscall return values, and
  replay mode can serve recorded syscall returns without reissuing the live
  syscall.
- Record/replay now owns the LSTAR gadget-bypass byte in the per-vCPU gadget
  state page. Starting record or replay mode sets the byte across the live
  vCPU pool; stopping or destroying the active container clears it. Newly
  installed gadget state pages sync from the active static key.
- `arch/um/backend/kvm-v2/lstar_gadget.S` checks that byte after saving user
  scratch registers and forces the normal host fallback path when it is set,
  so gadget-handled syscalls cannot disappear from the future dispatcher log.
- `arch/um/backend/kvm-v2/test_record.c` validates the experimental core,
  time-travel clock-event FIFO replay, and the synthetic gadget-bypass page
  helper without requiring a live vCPU.
- `arch/um/backend/kvm-v2/record.c` now exposes the experimental debugfs
  singleton control/status files `kvm_v2_record_ctl` and
  `kvm_v2_record_status`. They drive `start`, `stop`, `replay`, `strict`, and
  `destroy` for validation. Debugfs `start` now captures and attaches a KVM v2
  task snapshot before enabling the record static key, and snapshot-backed
  `replay` restores it before entering the syscall replay core. Record start
  also enables the `record_replay` hook so UML time-travel clock advances can
  be appended and replayed with syscall-count anchors.
- Validation on 2026-06-10 after gadget-bypass wiring: `make ARCH=um -j16`,
  `um_kvm_v2_record` 10/10, `kvm_v2_byteshape` 9/9, both record and byteshape
  filters also PASS under `backend=force=kvm`, and the freestanding KVM
  `perf-getpid` gadget smoke remains PASS with `cyc_per_call=89`.
- Validation on 2026-06-10 after debugfs control wiring:
  `kvm-record-smoke` PASS, including `um_kvm_v2_record` 10/10 and a live KVM v2
  snapshot-backed record run with 3067 syscall entries, 245360 bytes used, and
  0 drops.
- Validation on 2026-06-10 after time-travel clock-event wiring:
  `kvm-record-clock-bench` PASS with `N=100`, `observed=100`, `replayed=100`,
  and `mismatches=0`.
- Still open: raw time/RDTSC/signal determinism, supported replay tier docs,
  and real deterministic workload replay smoke tests.

Acceptance gates:

- Record KUnit tests. Current status: PASS for the experimental core.
- Record smoke test. Current status: PASS through `kvm-record-smoke`.
- Replay smoke test.
- Buffer overflow behavior test.
- Gadget-on and gadget-off comparison. Current status: state-page bypass and
  gadget-on hot path are covered; full record-mode workload smoke remains open
  until live syscall dispatcher wiring lands.
- Documentation of supported determinism tier.

## Workstream D: KVM State Trace And Diagnostics

Purpose:

- Preserve useful state trace capability without carrying research diary prose
  into normal source.

Source branch:

- `kvm-v2-snapshot-elf64`

Likely source files:

- `arch/um/backend/kvm-v2/state_trace.c`
- `arch/um/backend/kvm-v2/state_trace.h`
- parser tools under selftests or tools

Required cleanup:

- Remove investigation round notes from source comments.
- Replace "why this investigation existed" with "what this debug facility
  provides".
- Bound default memory usage.
- Make debugfs ABI clear and stable.
- Make panic/dump path safe enough for diagnostic use.

Acceptance gates:

- Build with state trace disabled.
- Build with state trace enabled.
- Enable, capture, dump, clear smoke test.
- Parser smoke test.
- Documentation for debugfs files.

Current `next` decision:

- Do not import the historical `state_trace.c` / `state_trace.h` wholesale.
  The old implementation is an investigation artifact: it carries stale
  field assumptions, bug-trigger auto-freeze logic, and comments tied to
  specific debugging rounds.
- Current `next` keeps normal `TRACE_EVENT` coverage under
  `arch/um/include/asm/trace/um_backend.h` as the supported observability
  surface.
- Private state trace remains a future optional diagnostic, not a runtime
  functionality blocker. If it is restored, implement a clean debug-only
  version from the current KVM v2 state model with bounded memory use, stable
  debugfs controls, a minimal hook set, and parser/smoke coverage.
- Historical parser tools remain reference material until the kernel-side
  trace format is deliberately reintroduced.

## Workstream E: Template Pause, Fork Server, And Pool

Purpose:

- Consolidate all fork-server and pool functionality into current `next`.

Source branches:

- `fork-server-phase1c`
- `memo09-phase2`
- `memo09-phase3-pool-bench`
- `memo09-phase4`
- current `next`

Current `next` already contains:

- `tools/uml/uml-launcher/src/bin/umlctl/pool.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/pool_serve.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/pool_client.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/pool_take_status.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/exec.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/port_forward.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/tapfd.rs`
- template-pause selftests and pool selftests

Functional requirements:

- Direct `pool spawn` works.
- Daemon `pool serve` works.
- `pool take` produces members reliably.
- `pool list`, `pool status`, `pool destroy`, and daemon shutdown work.
- `umlctl exec` works through mconsole, or the command surface is explicitly
  marked incomplete until a successful guest exec path is validated.
- `port-forward` works for pool members.
- Vector2 TAP handoff works for live pool members.
- Launcher-owned vector2 fd handoff remains validated outside the pool-member
  identity path; per-take pool fd handoff is retired from the current
  completion claim.
- Identity blob layout is documented and tested.
- Per-member mconsole path synthesis is reliable.
- Warm pool support is implemented for daemon-assigned ready members. The final
  current contract keeps request-specific takes lazy and treats any
  predeclared slot/identity API as future work.
- The full-scale pool benchmark passes with live quiesced members. The memory
  gate uses PSS as the proportional host-memory metric and still prints summed
  RSS as diagnostic context.

Required historical comparison:

- Diff current `next` against each `memo09-*` branch for `tools/uml`,
  `arch/um/kernel/template_pause*`, `arch/um/include/asm/um-template-pause.h`,
  and `tools/testing/selftests/um/*pool*`.
- Import only missing behavior.
- Keep current newer launcher modules where they supersede older branch code.

Current comparison result:

- Completed in
  `2026-06-10-pool-fork-historical-comparison-plan.md`.
- Current `next` already contains the memo09 command surfaces, cleaned
  template-pause modes, identity application, daemon-routed exec,
  port-forward, pivot and pool-member smokes, and pool benchmark.
- `min_warm` now maintains a daemon-owned ready queue for pre-identified
  members and `pool take --ready` consumes that queue. Request-specific takes
  remain lazy so the daemon does not lie about caller-supplied identity; the
  kernel applies MAC/TAP/mconsole identity before the member is forked.
- Current template-pause, fork, spawn, serve, port-forward, syzkaller, and full
  pool benchmark smokes pass on the live replicated-member path.
- The remaining pool/fork gap is closed for the current completion claim.
  Vector2 pool-member TAP handoff and syzkaller-style take/exec/destroy have
  dedicated passing smoke gates. Per-take pool fd handoff is retired from the
  current completion claim in favor of the validated pool TAP reopen path.
- Historical snapshot test wrappers from `memo09-phase4` should be imported
  or replaced as cleaned kselftests because the underlying snapshot hooks now
  exist on `next`. Current status: imported and PASS on 2026-06-10.

Acceptance gates:

- `cargo fmt --check`
- `cargo test`
- `pool-spawn-smoke`
- `pool-serve-smoke`
- `pool-exec-smoke`
- `pool-port-forward-smoke`
- `pool-bench`
- template-pause fork smoke and stress

## Workstream F: Vector2 Completion

Purpose:

- Bring vector2 from experimental to replacement-ready, or adjust all claims
  so it is clearly not complete.

Current `next` contains:

- typed config parser
- command-line collection
- lifecycle model
- queue ownership helpers
- fd backend
- tap backend
- fake host tests
- netdev integration
- ethtool stats
- sandbox gating

Required functionality:

- Keep the vector2 runtime transport claim limited to TAP and inherited fd.
  GRE and L2TPv3 are parser/header-helper coverage only; raw, proxy, VDE,
  BESS, and hybrid are unsupported by the current netdev datapath and require
  new backends plus live smokes before they can re-enter the runtime claim.
- Keep multiqueue fd handoff validated and complete fairness/performance
  validation.
- Keep sandbox validation for untrusted mode green.
- Keep in-process trusted host validation green.
- Keep launcher-owned fd handoff validated. Per-take pool fd handoff is
  retired from the current completion claim: current pool takes carry string
  identity through the identity memfd and use the vector2 TAP reopen path for
  per-member TAP isolation.
- Confirm failure injection is test-only or clearly documented. Current
  status: `fail_open_after=N` is runtime-available for the live open-unwind
  gate, but documented as validation-only and not part of normal workload
  configuration.
- Align Kconfig wording with actual readiness.

Publication rule:

- `CONFIG_UML_NET_VECTOR_V2` can stay `default n`, but documentation must not
  say legacy vector is superseded until v2 passes replacement gates. Current
  status: Kconfig/help text now keeps v2 opt-in and names TAP/fd as the
  current vec2 runtime netdev scope.

Acceptance gates:

- vector2 KUnit suites.
- vector2 sandbox audit. Current status: PASS on 2026-06-10 through
  `vector2-sandbox-audit`.
- launcher fd handoff smoke. Current status: PASS on 2026-06-10 through
  `vector2-fd-handoff-smoke`.
- vector2 pool-member TAP smoke. Current status: PASS on 2026-06-10 through
  `vector2-pool-tap-smoke`.
- multiqueue smoke. Current status: PASS on 2026-06-10 through
  `vector2-fd-multiqueue-smoke`; fairness/perf coverage remains open.
- trusted in-process TAP smoke. Current status: PASS on 2026-06-10 through
  `vector2-inproc-tap-smoke`.
- parser-only transport boundary. Current status: KUnit guards raw, GRE,
  L2TPv3, hybrid, BESS, VDE, and proxy returning `-EOPNOTSUPP` from the
  netdev open path; GRE/L2TPv3 header-helper KUnit remains present.
- failed-open open-unwind gate. Current status: PASS on 2026-06-10 through
  `vector2-failed-open`: fd-handoff TAP with `fail_open_after=2`, successful
  host gateway ping, injected second `ndo_open()` failure, `open_delta=1`,
  `fail_delta=1`, `close_delta=1`, closed/registered `vec2.0`, and TAP
  cleanup.
- vector2 validation docs. Current status: the validation-gates memo is the
  current gate tracker for `next`; the post-May-19 default-flip README is
  marked historical and no longer competes with the June 10 completion
  inventory.
- seccomp backend vector2 Tier 3 networking.
- KVM v2 backend vector2 Tier 3 networking.
- Long soak.

## Workstream G: Launcher, Deploy, Gates, And Build Tooling

Purpose:

- Ensure all user-facing host tooling works with final kernel features.

Source branches:

- `umlctl-deploy`
- current `next`

Current surfaces:

- `umlctl up/down/ps/logs`
- deploy configs
- gates
- pool commands
- exec
- port-forward
- snapshot export
- transparency tooling
- syzkaller shim support
- `umlbuild`

Required cleanup:

- Remove commands that only work against historical kernel branches.
- Add feature detection for optional kernel surfaces.
- Improve error messages when a kernel lacks required Kconfig/debugfs support.
- Keep example TOML files synchronized with real command-line and Kconfig
  names.
- Keep Rust comments user-facing and durable, not phase/history oriented.

Acceptance gates:

- `cargo fmt --check`
- `cargo test`
- launcher smoke. Current status: PASS on 2026-06-10 through
  `umlctl-smoke` and `launcher-smoke`.
- deploy smoke. Current status: PASS on 2026-06-10. The current and
  historical `umlctl-deploy` example/profile file sets match, all 18 example
  Umlfiles pass `UML_KERNEL=$PWD/linux umlctl up --dry-run`, and all 5
  built-in profiles resolve with `umlbuild profile show`.
- gate dry run. Current status: PASS on 2026-06-10 through
  `umlctl gate run --dry-run` against
  `tools/testing/selftests/um/gates/launcher-cargo.toml`.
- transparency smoke. Current status: PASS on 2026-06-10 through
  `run-bpftrace-validate.sh`; all five bpftrace scripts attached, with
  syscalls and sched producing idle UML data.
- `umlbuild` MVP smoke. Current status: PASS on 2026-06-10 through
  `run-umlbuild-mvp.sh` with `UMLBUILD_SOURCE` pointing at a temporary clean
  worktree and prebuilt debug `umlbuild`/`umlctl`; direct boot and `umlctl up`
  both produced the expected guest sha256. The selftest now supports
  `UMLBUILD_SOURCE`, `UMLBUILD`, and `UMLCTL` overrides so developer trees with
  in-tree build products do not need destructive cleanup.
- pool/exec/port-forward smoke
- snapshot export smoke after Workstream B. Current status: live export PASS
  in the mconsole-backed export update on 2026-06-10.

## Workstream H: Syzkaller UML VM Shim

Purpose:

- Make the syzkaller `vm/uml` path work against final `next`.

Files:

- `tools/uml/syzkaller-vm-shim/uml.go`
- launcher pool/exec modules
- syzkaller-facing examples/docs

Functional requirements:

- Pool serve/take integration.
- Exec command returns stable stdout, stderr, exit status, signal, timeout.
- Crash and console output are captured.
- Instance cleanup is reliable.
- Networking works for the configured mode.
- Kernel command line and rootfs handling are documented.

Acceptance gates:

- Build shim.
- Unit or smoke test for take/exec/destroy. Current status:
  `syzkaller-shim-smoke` PASS on 2026-06-10.
- End-to-end syzkaller-style command execution smoke. Current status:
  `syzkaller-shim-smoke` PASS on 2026-06-10.
- Crash capture smoke if practical.

## Workstream I: Profiles And Instrumentation

Purpose:

- Close original plan promises around profiles and instrumentation.

Feature areas:

- `uml/research`
- `uml/fuzz`
- `uml/fuzz-deep`
- `uml/prod-fast`
- `uml/prod-with-hooks`
- `uml/sandbox`
- `uml/time-travel`
- KASAN
- KMSAN
- KCSAN
- KFENCE
- KCOV
- kprobes
- ftrace
- BPF JIT
- KGDB if still in scope

Tasks:

- Verify every profile config still builds.
- Verify instrumentation docs match Kconfig support.
- Verify smoke tests for kprobes, ftrace, KMSAN, hooks, and CVE repros.
- Decide whether time-travel profile maps to record/replay, snapshot, or a
  future-only placeholder.
- Remove claims for unsupported profiles.

Acceptance gates:

- Profile config build matrix. Current status: config-generation PASS on
  2026-06-10 for all 10 kernel Kconfig profiles in a clean temporary worktree;
  full per-profile kernel builds remain open. The `research-kmsan` profile is
  now documented, listed in `make ARCH=um help`, and included in the runtime
  profile-probe harness when its LLVM-built binary is present.
- `umlbuild` profile resolution. Current status: PASS on 2026-06-10 for all
  5 built-in `umlbuild profile show` profiles.
- ftrace smoke. Current status: PASS on 2026-06-10 for a clean-worktree UML
  build with normal dynamic function tracing and no function-graph tracer.
  Function graph tracing is not part of the current supported UML surface
  because its return-address rewriting still conflicts with UML task switching.
- Hooks flip smoke. Current status: PASS on 2026-06-10 against the current
  `./linux` build.
- kprobes stress. Current status: PASS on 2026-06-10 against a clean-worktree
  research-profile UML build with `samples/kprobes/kretprobe_example.ko`:
  `KPROBES_STRESS: PASS iters=1000 fires=1004 errors=0 graph=on`.
- KMSAN smoke. Current status: SKIP on 2026-06-10 against the current
  `./linux` build because it does not contain the KMSAN runtime; rerun with
  `LLVM=1 uml/research-kmsan`.
- BPF/JIT config/build slice. Current status: PASS on 2026-06-10 from a
  temporary clean worktree: `uml/research` enables `CONFIG_HAVE_EBPF_JIT=y`,
  `CONFIG_BPF_SYSCALL=y`, `CONFIG_BPF_JIT=y`,
  `CONFIG_BPF_JIT_ALWAYS_ON=y`, and `arch/x86/net/bpf_jit_comp.o` builds.
- BPF/JIT runtime smoke. Current status: PASS on 2026-06-10 through
  `bpf-jit-smoke` against a fresh research-profile UML binary:
  `bpf_jit_enable=1`, minimal `BPF_PROG_TYPE_SOCKET_FILTER` load succeeds,
  and `BPF_OBJ_GET_INFO_BY_FD` reports `xlated_len=16` and `jited_len=16`.
- KASAN runtime smoke. Current status: PASS on 2026-06-10 through
  `cve-repro` against a fresh research-profile UML binary plus
  `mm/kasan/kasan_test.ko`: `ok=10`, `not_ok=0`, `kasan_bugs=13`,
  `guest_wall_s=0`, and `host_wall=2.71s`.
- KFENCE runtime smoke. Current status: partial; the research runtime profile
  probe confirms `debugfs_kfence=PRESENT`, but a focused KFENCE report smoke
  is still required before final completion.
- KCSAN runtime smoke. Current status: open; build and boot the `race`
  profile with `ncpus=2`, validate `debugfs_kcsan`, and capture selftest
  evidence.
- KCOV runtime smoke. Current status: open; build and boot `fuzz` or
  `fuzz-deep` and run a focused `/sys/kernel/debug/kcov` mmap/ioctl smoke.
- Profile harness alignment. Current status: fixed for `research`, which now
  expects `debugfs_kcov=ABSENT` because KCOV is intentionally isolated to
  fuzzing profiles.
- Documentation table with pass/fail/skip and rationale. Current status:
  partially updated in the live inventory; final docs matrix remains open.

## Workstream J: Selftest Curation

Purpose:

- Turn the current large selftest tree into a clean test suite instead of a
  research notebook.

Categories:

- Upstreamable regression tests.
- Local stress and soak tests.
- Historical repros.
- Diagnostic-only scripts.

Tasks:

- For upstreamable tests, remove diary-style comments, internal issue labels,
  phase notes, and stale bug wording.
- For local stress tests, keep detailed rationale but place it in README files
  or `HISTORICAL` notes.
- For historical repros, move to clearly named archival directories or docs.
- Make Makefile targets match current functionality.
- Remove or skip tests for absent historical features until those features
  land.

Acceptance gates:

- `make -C tools/testing/selftests/um` target list is accurate.
- Each major feature has at least one smoke test.
- No test silently passes because a required kernel feature is absent.
- Expected failure lists are current and justified.

Current state:

- A focused 2026-06-10 cleanup removed planning document tags and phase labels
  from active launcher metadata, sandbox/research launcher examples, SELinux
  policy headers, gate metadata, and tier smoke fixture values.
- Gate `phase` values now use semantic groups such as `host-tools`, `build`,
  `substrate`, `python`, and `performance` instead of chronology labels.
- Validation passed with `cargo fmt --check`, `cargo test`, `python3 -m
  py_compile` for touched tier smoke scripts, `umlctl gate list --source-root
  .`, `umlctl gate run --dry-run` for all gate TOMLs, `uml-launcher run
  --dry-run --config` for touched legacy launcher examples, and
  `git diff --check`.
- A focused scan over `arch/um`, `tools/testing/selftests/um`, and
  `tools/uml/uml-launcher` now reports no standalone workstream, decision,
  memo, or phase-label patterns after excluding operational post-mortem
  wording and archived redesign material.
- A follow-up source/selftest pass removed the remaining internal audit label
  from `kvm-bounds`, temporary-policy wording from the SELinux launcher policy
  and deploy comments, and marked KVM FPU isolation reproducers as
  diagnostic-only. The residual active scan is limited to operational terms
  such as init/workload phases, sample BUG log text, proc/stat field numbers,
  and BPF filter syntax.

## Workstream K: Documentation And Reports

Purpose:

- Make docs reflect final `next`, not the path taken to get there.

Tasks:

- Update `Documentation/virt/uml/redesign/STATUS.md`.
- Update original vision completion matrix.
- Update report/presentation data or mark old reports as historical.
- Remove stale claims about snapshot/record/replay if not implemented yet.
- Remove stale claims about vector2 replacement readiness until proven.
- Move diary/history material into `HISTORICAL` files.
- Ensure user-facing docs describe commands that work on `next`.
- Ensure debugfs docs list real files.

Acceptance gates:

- `git grep` for stale trigger names maps to real implementation.
- `git grep` for internal issue patterns in source and upstreamable tests is
  clean or intentionally archived.
- `git diff --check torvalds/master...next` is clean, except for explicitly
  archived patch artifacts if kept.

Current state:

- The launcher examples README and the `launcher`, `sandbox`, and `research`
  user-facing docs no longer describe those paths in workstream/future-v2
  terms.
- The broader active-doc normalization pass also cleaned ftrace, kprobes,
  KMSAN, debugfs, section-split, snapshot/forkserver, launcher, APERF/MPERF,
  and profile docs.
- A focused scan excluding `Documentation/virt/uml/redesign/**` now has no
  standalone workstream, decision-log, memo, phase-history, or future-phase
  labels in `*.rst` files except a real OpenWrt sample kernel version line
  containing `#0`.
- The May 2026 report/deck workspace under `report-presentation/` is now
  marked historical in `README.md`, `PLAN.md`, the shared TeX metadata, report
  source, slide source, comprehensive report source, and `status_kpis.csv`.
  The tracked report, slide, and comprehensive PDFs were rebuilt from those
  sources with `make all`.
- Snapshot ELF/debugfs docs now match current `next`: the format doc is marked
  validated, `debugfs.rst` lists the KVM v2 snapshot bench/export files, and
  `umlctl snapshot export` help names mconsole `snapshot_export` instead of
  debugfs.
- Remaining documentation cleanup is now the broader redesign archive side:
  old research notes must either stay clearly historical or be refreshed from
  final `next` if they become user-facing evidence again.

## Workstream L: Upstream Series Readiness

Purpose:

- Make the final `next` branch split-able into coherent upstream series.

Series boundaries:

- Backend abstraction and seccomp preservation.
- Static-key and instrumentation work.
- Profiles and Kconfig support.
- KVM v2 core.
- KVM v2 optional snapshot/record features if suitable for upstream.
- Vector2 networking.
- Launcher and selftests, depending on upstream strategy.
- Documentation.

Tasks:

- Rebuild patch queue from final `next`, not from old branch state.
- Run checkpatch on candidate upstream patches.
- Refresh cover letters with final validation results.
- Ensure each series builds independently if submitted independently.

Acceptance gates:

- Patch queue regenerates.
- `checkpatch.pl` results are reviewed.
- Cover letters no longer reference stale phase status.
- Public docs match submitted code.

## Integration Order

### Phase 0: Inventory And Tracking

Create a live tracking document with a row for every historical feature.

Current status:

- Started in
  `2026-06-10-next-functionality-inventory.md`; keep it live as each
  workstream lands.

Columns:

- Feature.
- Historical branch.
- Historical files.
- Current `next` status.
- Disposition.
- Owner branch.
- Tests.
- Docs.
- Completion state.

Exit criteria:

- No historical branch has unclassified UML functionality.
- Missing features are grouped into the workstreams above.

### Phase 1: Current `next` Hardening

Fix current blockers:

- KVM v2 restore ioctl failure handling.
- Snapshot CLI/doc mismatch temporary guard if snapshot import is not first.
- KVM gadget status reconciliation.

Current status:

- The reviewed KVM v2 restore failure-handling issue is closed.
- Snapshot CLI/doc mismatch is closed by the mconsole-backed exporter.
- Gadget static validation is green on the cleaned tree with
  `CONFIG_UM_BACKEND_KVM_V2_GADGET=y`: `perf-getpid` reports
  `cyc_per_call=89`, `perf-pidfam` reports `cyc_per_call=94`,
  `kvm-bounds` passes 9/9, and the fallback static loop emits its expected
  result.
- The focused dynamic-userspace blocker is closed: forced-KVM `/bin/true`
  with `kunit.enable=0` reaches clean init exit with `exitcode=0`, and the
  dyn-loader kselftest reports `DYN_LOADER: backend=kvm PASS`.

Exit criteria:

- Current `next` has no known hard correctness bug from the 2026-06-10 review.

### Phase 2: Snapshot And Snapshot ELF

Port or reimplement snapshot and snapshot ELF export.

Current status:

- Closed for the current single-online-CPU scope.

Exit criteria:

- `umlctl snapshot export` works on `next`.
- Snapshot docs are true.
- Snapshot KUnit and live ELF export smoke pass. Current status: KUnit PASS
  4/4 and live `umlctl snapshot export` PASS on 2026-06-10.
- Snapshot restore runtime smoke passes. Current status: PASS on 2026-06-10
  through `kvm-snapshot-restore-smoke`.
- SMP snapshot semantics are explicitly gated to one online CPU.

### Phase 3: Fork Server And Pool Completion

Consolidate pool/fork-server behavior and close the remaining production
pool gaps.

Current status:

- Template-pause, fork-on-resume, one-shot pool member, sustained replicated
  pool member, direct pool spawn, daemon pool serve/take/status/destroy,
  warm-ready members, port-forward result handling, and full pool-bench pass on
  the current path.
- Daemon-routed guest exec passes for command success, stdout/stderr capture,
  exit-status preservation, timeout reporting, and helper cleanup. `exec/1` is
  the current public ABI; the bounded shell-backed mconsole lowering and guest
  `timeout(1)` helper dependency are documented implementation details.

Exit criteria:

- Pool serve/take/port-forward continues to pass on live members.
- Successful daemon-routed guest exec works through the final published
  `exec/1` ABI; any future stricter kernel argv transport uses a new schema.
- Full pool benchmark passes with the documented PSS memory gate and throughput
  remains green.
- Vector2 TAP handoff works through live pool members. Current status: PASS on
  2026-06-10 through `vector2-pool-tap-smoke`.
- Launcher-owned vector2 fd handoff works. Current status: PASS on 2026-06-10
  through `vector2-fd-handoff-smoke`; per-take pool fd handoff is explicitly
  retired from the current completion claim in favor of the validated pool TAP
  reopen path.
- Syzkaller-style take/exec/destroy works through the current `exec/1` path;
  keep future schema bumps explicit.
- Missing `memo09-*` functionality is either landed or explicitly retired.

### Phase 4: Record/Replay

Complete or land as explicitly experimental.

Exit criteria:

- If counted as complete: record and replay real supported workloads pass.
- If not complete: all docs and status say it remains experimental/future.

### Phase 5: Vector2 Replacement Gates

Finish vector2 functionality and validation.

Exit criteria:

- All intended transports/modes are implemented or pruned.
- Tier 3 networking gates pass on seccomp and KVM v2.
- Kconfig/docs accurately state readiness.

### Phase 6: Syzkaller And Profiles

Finish syzkaller shim and profile matrix.

Exit criteria:

- Syzkaller-style take/exec/destroy works.
- Profile build/test matrix is documented and passing.

### Phase 7: Selftest And Documentation Cleanup

Clean the public surface.

Exit criteria:

- Upstreamable source/tests are free of random history and diary prose.
- Historical notes are preserved only in archival docs.
- Reports and presentation data are refreshed or marked historical.

### Phase 8: Final Gate And Push

Run final validation and push.

Exit criteria:

- Full gate passes.
- `next` is pushed.
- Final status doc declares exact completion state.

## Branch Strategy

Use temporary integration branches to keep review manageable:

- `integrate/kvm-v2-hardening`
- `integrate/kvm-snapshot`
- `integrate/kvm-record`
- `integrate/fork-pool`
- `integrate/vector2-complete`
- `integrate/syzkaller-profiles`
- `integrate/docs-tests-cleanup`

Rules:

- Each branch starts from current `next`.
- Each branch imports only one coherent feature group.
- Historical branches are used as source material, not merge bases.
- Rebase onto `next` before landing.
- Squash or split into reviewable commits with clean commit messages.
- After landing each branch, push `next`.

## Code Hygiene Rules

For kernel C and headers:

- No internal issue numbers in comments.
- No phase diaries.
- No branch-specific commit IDs.
- No "for now" comments without a concrete invariant.
- No user-facing docs for missing code.
- Error handling must follow kernel norms for the state being protected.
- Required architectural state restore failures must not be ignored.
- Debug-only code must be behind Kconfig or debugfs gates.

For Rust launcher code:

- CLI commands must detect missing kernel support and fail clearly.
- Public schema comments should describe stable contracts.
- Internal history belongs in docs, not command implementation comments.
- `cargo fmt --check` and `cargo test` are mandatory before landing.

For selftests:

- Regression rationale is fine.
- Diary/investigation narratives move to READMEs or historical docs.
- Tests must not pass accidentally when prerequisites are missing.
- Expected failures must identify the current owner and removal condition.

## Final Validation Matrix

Required final commands and gates:

```sh
make ARCH=um -j$(nproc)
```

```sh
git diff --check torvalds/master...next
```

Launcher:

```sh
cd tools/uml/uml-launcher
cargo fmt --check
cargo test
```

Kernel/unit gates:

- KVM v2 KUnit.
- Vector2 KUnit.
- Backend contract KUnit.
- Snapshot KUnit. Current status: `um_kvm_v2_snapshot` PASS 4/4 on
  2026-06-10.
- Record KUnit if record/replay lands.

Runtime smoke:

- KVM smoke.
- KVM bounds.
- KVM mm smoke.
- CPython tier0.
- CPython parity.
- CPython full where practical.
- Snapshot restore smoke. Current status: PASS on 2026-06-10 through
  `kvm-snapshot-restore-smoke`.
- Snapshot ELF export roundtrip. Current status: live `umlctl snapshot export`
  plus `readelf`, `gdb`, and helper parse PASS; `snapshot-elf-roundtrip`
  kselftest PASS on 2026-06-10.
- Snapshot benchmark kselftest. Current status: `kvm-snapshot-bench` PASS on
  2026-06-10.
- Snapshot KUnit kselftest wrapper. Current status: `snapshot-kvm-smoke` PASS
  on 2026-06-10.
- Template-pause fork smoke.
- Template-pause fork stress.
- Pool spawn smoke.
- Pool serve smoke.
- Pool exec smoke.
- Pool mconsole path probe. Current status: PASS; member reaches userspace, no
  panic, requested mconsole socket present, and `version` replies.
- Pool port-forward smoke.
- Vector2 sandbox audit. Current status: PASS through
  `vector2-sandbox-audit`.
- Vector2 launcher fd handoff. Current status: PASS through
  `vector2-fd-handoff-smoke`.
- Vector2 pool-member TAP handoff. Current status: PASS through
  `vector2-pool-tap-smoke`.
- Vector2 tap/multiqueue. Current multiqueue smoke status: PASS through
  `vector2-fd-multiqueue-smoke`; fairness/perf coverage remains open.
- Vector2 trusted in-process TAP. Current status: PASS through
  `vector2-inproc-tap-smoke`.
- Vector2 parser-only transport boundary. Current status: TAP/fd are the only
  runtime netdev transports; raw/GRE/L2TPv3/hybrid/BESS/VDE/proxy are guarded
  as unsupported by the current netdev path.
- Vector2 failed-open open-unwind. Current status: PASS through
  `vector2-failed-open`.
- Syzkaller shim smoke.

Longer gates:

- KVM v2 24 hour soak on final cleaned tree.
- Vector2 7200 second seccomp soak.
- Tier 3 networking on seccomp/vector2.
- Tier 3 networking on KVM v2/vector2.
- CPython/Tier 3 workload with gadget state documented.
- Record/replay deterministic workload if counted complete.

## Completion Dashboard

Use this table in the tracking doc and update it after every integration
branch lands.

| Area | Current state | Required final state | Status |
| ---- | ------------- | -------------------- | ------ |
| KVM v2 core | Present on `next` | Hardened, validated | In progress |
| KVM v2 restore error handling | Fixed in current series | Checked/fatal policy | Closed for known issue |
| KVM snapshot | Present with KUnit, live export, restore smoke, and SMP gate | Present, validated, SMP policy defined | Closed for current scope |
| Snapshot ELF export | Present with live export pass | Working and documented on `next` | Closed for live export |
| Record/replay | Experimental syscall hook, snapshot-backed debugfs control, and live record smoke present; deterministic replay incomplete | Complete deterministic tier or explicitly experimental | Partially closed; replay runtime open |
| State trace | Historical/prototype | Clean optional debug infra | Open |
| Template pause | Single-shot and pivot/member paths validated; vector2 leg skips without guest `vec0` | Validated and documented | Mostly closed; vector2 leg pending |
| Fork server | Fork-on-resume smoke and default stress pass | Complete multi-iteration fork workflow plus stress | Closed for current fork-on-resume scope |
| Pool exec | Successful command, stdout/stderr capture, exit-status preservation, timeout reporting, and helper cleanup validated | Public `exec/1` ABI documented; future stricter kernel argv transport assigned to `exec/2` | Closed for current completion claim |
| Pool port-forward | Typed result/error handling validated | Validated against final networking mode | Mostly closed |
| Vector2 | Present, experimental | Replacement-ready or claims reduced | Open |
| Syzkaller shim | Present | End-to-end smoke | Open |
| Profiles | Present | Build/test matrix | Open |
| Selftests | Broad, noisy | Curated suites | Open |
| Docs/reports | Mixed current/stale | Truthful final status | Open |
| Upstream queue | Drafted/stale | Refreshed from final `next` | Open |

## Pool/Fork Execution Snapshot: 2026-06-10

This snapshot records the first live pass over the current pool and
template-pause surfaces after the historical branch comparison and snapshot
selftest import. It is the current truth for the next engineering increment.

Commands that pass:

```sh
bash -n tools/testing/selftests/um/template-pause-smoke/run-template-pause-smoke.sh
```

```sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/template-pause-smoke/run-template-pause-smoke.sh
```

Result:

- case 1 PASS;
- case 2 PASS;
- case 3 PASS;
- case 4 SKIP because `vec0` was not visible in the guest;
- verdict: the template-pause primitive and identity-apply path work.

```sh
timeout --kill-after=5 90 env UML_BINARY=$PWD/linux \
	tools/testing/selftests/um/template-pause-pivot-smoke/run-template-pause-pivot-smoke.sh
```

Result:

- PASS with 20 `PIVOT_OK` observations;
- no v1 ceiling panic;
- 20 `SIGCONT` events sent.

```sh
timeout --kill-after=5 120 env UML_BINARY=$PWD/linux \
	tools/testing/selftests/um/template-pause-pool-member-smoke/run-template-pause-pool-member-smoke.sh
```

Result:

- PASS;
- `POOL_ENTER`, `TPPM_POST_PAUSE`, `TPPM_MEMBER_ALIVE_1`, and
  `TPPM_MEMBER_DONE` were observed;
- identity fd and identity parsing were observed;
- no kernel panic and no v1 ceiling IP.

```sh
timeout --kill-after=5 120 env UML_BINARY=$PWD/linux \
	tools/testing/selftests/um/pool-spawn-smoke/run-pool-spawn-smoke.sh
```

Result:

- PASS for spawn, list, and destroy lifecycle.

```sh
timeout --kill-after=5 150 env UM_FORK_KERNEL=$PWD/linux \
	tools/testing/selftests/um/pool-serve-smoke/run-pool-serve-smoke.sh
```

Result:

- PASS for daemon socket readiness, status, valid take PID, destroy, clean
  shutdown, and master cleanup.

```sh
timeout --kill-after=5 150 env UM_FORK_KERNEL=$PWD/linux \
	tools/testing/selftests/um/pool-exec-smoke/run-pool-exec-smoke.sh
```

Result:

- PASS for successful typed NDJSON start and exit frames from `/bin/true`;
- PASS for captured stdout, captured stderr, and preserved guest exit code 7
  from a shell command;
- PASS for timeout behavior: a one-second timeout returns exit code 124 with
  `timed_out=true`, suppresses late stdout, does not emit a daemon error, and
  does not leak an extra guest `sleep` helper;
- stale daemon error boundaries such as missing `uml_mconsole(1)`, missing
  member socket, or kernel `Unknown command` are rejected by the selftest;
- `exec/1` is the current public ABI; the bounded shell-backed command string
  and guest `timeout(1)` helper dependency are documented implementation
  details, and stricter kernel argv/env/cwd transport is future `exec/2` work.

```sh
timeout --kill-after=5 150 env UM_FORK_KERNEL=$PWD/linux \
	tools/testing/selftests/um/pool-port-forward-smoke/run-pool-port-forward-smoke.sh
```

Result:

- PASS for typed JSON result shape;
- split host/guest port handling works;
- bogus PID fails cleanly;
- missing `--host-port` exits through clap with status 2.

```sh
timeout --kill-after=5 90 env UML_BINARY=$PWD/linux \
	tools/testing/selftests/um/template-pause-fork-smoke/run-template-pause-fork-smoke.sh
```

Result:

- PASS;
- two distinct child PIDs were reported through the identity memfd;
- identity blob parsing was observed twice;
- pre-fork teardown was observed twice;
- the master logged two resume cycles, proving that the harness drove the
  second take instead of killing the master at the second stop.

```sh
timeout --kill-after=5 150 env UML_BINARY=$PWD/linux \
	tools/testing/selftests/um/template-pause-fork-stress/run-template-pause-fork-stress.sh
```

Result:

- PASS;
- 548 kernel-log iterations over a 10 second observation window;
- 435 distinct child PIDs observed by the harness;
- median iteration time 18.2 ms, under the 50 ms budget;
- 548/548 clean identity round-trips across 20 distinct names;
- no kernel panics;
- no live orphans after teardown;
- `/proc` side-channel captured 98.2% of kernel-log iterations.

Commands that still fail or remain expected failures:

```sh
timeout --kill-after=5 150 env UML_BINARY=$PWD/linux \
	tools/testing/selftests/um/template-pause-pool-sustained-smoke/run-template-pause-pool-sustained-smoke.sh
```

Result:

- XFAIL;
- iteration 1 reached `MEMBER_DONE` and remains alive rather than exiting PID
  1;
- iteration 2 timed out before the second `POOL_ENTER`;
- iteration 2 left the child-pid write-back slot at `0`, so the master did
  not reach the supervisor-visible pid report;
- no kernel panic was observed;
- no v1 ceiling regression was observed;
- no live UML process remained after process-group teardown;
- the harness now stamps the current member identity before each take and keeps
  the accepted member alive so the test exercises sustained member ownership,
  not the expected panic path from exiting PID 1;
- the run still exposes the known MAP_SHARED physmem/member-ownership limit;
- the fix is a real per-member physmem file-descriptor design, not a test-only
  workaround.

Additional local boundary:

- temporary, uncommitted markers around fork preparation showed iteration 2
  reaches `sched_worker_detach_other_tasks()`, takes the runqueue lock, and
  then stalls before returning from the `rq->cfs_tasks` walk;
- this is consistent with the first live pool member mutating scheduler/kernel
  state that is still backed by the master's shared physmem;
- the next production fix should therefore isolate member kernel memory before
  the child re-enters userspace, rather than adding more loop-level guards.

Runtime replication recheck:

- `um_template_pause_pool_replicate=1` gates the replication path explicitly;
- the immediate iteration-1 regression was caused by copying/remapping only the
  setup-time registered physmem range, even though `arch_mm_preinit()` later
  lowers `uml_reserved` and exposes a larger runtime kernel physmem window;
- `um_pool_replicate_physmem()` now copies and remaps
  `uml_reserved..high_physmem`, and refreshes the registered region to the same
  range after swapping the backing fd;
- replication now runs before the child mutates task, timer, or saved-register
  state inherited from the master;
- the pool-member child resets inherited timer-wheel and hrtimer queues before
  arming its own fresh tick source;
- with `UML_POOL_REPLICATE=1`, the sustained harness reaches three
  `MEMBER_DONE` markers, records three nonzero child-pid slots, sees
  `POOL_REPLICATE_OK` three times, and observes no kernel panic or v1 ceiling
  regression;
- `template-pause-pool-member-smoke` also passes with
  `UML_POOL_REPLICATE=1`, including five timer ticks.

Detailed implementation plan:

- `2026-06-10-sustained-pool-physmem-isolation-plan.md` tracks the gated
  replication path, bounded failure handling, post-replication userspace/stub
  fixes, sustained-smoke PASS gate, daemon live-member conversion, and the
  final pool/vector2/syzkaller acceptance gates.

```sh
timeout --kill-after=5 180 env UM_FORK_KERNEL=$PWD/linux \
	UMLCTL=$PWD/tools/uml/uml-launcher/target/debug/umlctl \
	POOL_BENCH_TAKES=5 POOL_BENCH_FORKS=3 \
	POOL_BENCH_LIFECYCLE_N=20 POOL_BENCH_THROUGHPUT_S=3 \
	POOL_BENCH_THROUGHPUT_R=50 \
	tools/testing/selftests/um/pool-bench/run-pool-bench.sh
```

Result:

- PASS in the reduced validation gate;
- take p50 was 0.5 ms and p99 was 0.9 ms over five measured takes;
- RSS sampled 3/3 live sparse-copied replicated children and total RSS was
  148.6 MiB;
- smaps rollup for the reduced gate reported 133.6 MiB PSS, 16.5 MiB private
  dirty, and 6.7 MiB shared dirty;
- lifecycle RSS drift was 0.00% over 20 take/destroy cycles;
- throughput completed 150/150 takes in the 3-second reduced gate;
- this proves the benchmark now measures live children, but it is not a
  substitute for the full default-scale benchmark.

Full default-scale validation:

```sh
timeout --kill-after=10 900 env KEEP_OUT=1 UM_FORK_KERNEL=$PWD/linux \
	UMLCTL=$PWD/tools/uml/uml-launcher/target/debug/umlctl \
	tools/testing/selftests/um/pool-bench/run-pool-bench.sh
```

Result:

- PASS overall, 5/5 gates passed;
- take p50 was 1.8 ms and p99 was 2.5 ms over 1000 measured takes;
- memory sampled 100/100 live quiesced replicated members at 139.0 MiB PSS,
  499.3 MiB summed RSS, and 17.6 MiB private dirty; the benchmark keeps summed
  RSS as context but gates on PSS because shared executable/libc/tmpfs mappings
  are intentionally shared across stopped members;
- lifecycle RSS drift was 0.05% over 10,000 take/destroy cycles;
- throughput completed 3000/3000 target takes in the 60-second gate, above the
  2700 pass threshold;
- artifacts from the full run were kept at `/tmp/pool-bench.7yKXDC` and
  `/tmp/pool-bench-rt.hSsofa`;
- this closes the pool benchmark blocker for the current completion claim.

Immediate engineering conclusion:

- current `next` already has the broad pool/fork command surface;
- single template pause, fork-on-resume smoke/stress, identity apply, pivot,
  one-shot pool member, spawn, serve, typed exec error handling, and
  port-forward result handling are real;
- replicated sustained pool-member lifetime now passes in the direct harness;
- daemon pool take/serve now routes through the replicated live-member path and
  returns members quiesced until daemon-routed `exec` resumes them;
- daemon `min_warm` now prefills, consumes, replenishes, reports, and cleans up
  pre-identified ready members through `pool take --ready`;
- `syzkaller-shim-smoke` now validates the shim source contract and the
  syzkaller-style take/exec/port-forward/status/destroy wire path through
  `umlctl`;
- final completion requires finishing the remaining vector2 networking gates
  plus the open record/replay, state trace, profile, selftest, and upstream
  readiness items tracked above.

## Immediate Next Actions

1. Keep the current warm scheduling contract covered by `pool-serve-smoke` and
   `syzkaller-shim-smoke`: request-specific takes stay lazy, while
   `pool take --ready` is daemon-assigned identity only.
2. Keep the daemon-routed `exec/1` ABI covered by `pool-exec-smoke` and the
   syzkaller shim smoke. Stricter kernel argv/env/cwd transport is future
   `exec/2` work, not a current completion blocker.
3. Keep the retired per-take pool fd handoff boundary covered in status docs;
   launcher-owned vector2 fd handoff is now covered by `vector2-fd-handoff-smoke`.
4. Complete record/replay beyond the explicit experimental syscall hook and
   snapshot-backed debugfs record control, including time, signal, device, and
   workload-level replay gates, before counting the original mission complete.
5. Decide whether private state trace is worth importing as clean optional
   diagnostics.
6. Re-audit vector2 transport claims, Kconfig wording, and replacement
   readiness against actual validation.
7. Curate selftests and source comments for upstream style: no internal issue
   numbers, diary prose, branch-specific commit IDs, or stale phase notes on
   upstream-facing paths.
8. Refresh reports/presentations from normalized status and evidence tables
   once functionality and validation are final.
9. Run the final validation matrix, update `STATUS.md` and the inventory,
   commit, and push `next`.

## Policy For Retiring Functionality

Because the goal is "import or complete all functionality", retirement is not
the default. A feature can be retired only if all of the following are true:

- It is explicitly listed in the inventory.
- The reason is technical, architectural, or scope-based, not convenience.
- Any user-facing docs/tools/tests that reference it are removed or marked
  historical.
- The status file says it is retired and why.
- The user agrees to the retirement.

Until then, missing historical functionality remains open work.
