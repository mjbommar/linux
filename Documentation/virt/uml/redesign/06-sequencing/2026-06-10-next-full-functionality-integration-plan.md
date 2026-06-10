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
  runnable members.
- `pool serve --min-warm` now keeps daemon-assigned ready members, exposes
  ready/taken/failed/min-warm status, supports `pool take --ready`, and
  cleans up ready members during destroy/shutdown.
- Reduced raw `pool-bench` passes all five gates with live sparse-copied
  replicated children: 5/5 latency takes, 3/3 live RSS children at
  145.9 MiB, 0.00% lifecycle RSS drift, and 4/4 throughput takes in the
  reduced two-second gate.
- Full default `pool-bench` now runs to completion but still fails 2/5 gates:
  RSS is 7,409.4 MiB for 100/100 live children against the 200 MiB target, and
  throughput reaches 2250/3000 takes against the 2700 target.

The active blockers are now:

1. Fix or intentionally revise the full-scale pool memory/throughput targets.
   The current implementation is stable, live, and sparse-copied, but still
   far above the original 100-member RSS target and below the throughput gate.
2. Decide the final daemon-routed guest exec ABI. The current smoke now proves
   successful commands through the final member mconsole path: `/bin/true`
   exits 0, captured stdout/stderr round-trip through NDJSON, guest exit code
   7 is preserved as a normal exec result rather than a daemon error, and a
   one-second timeout returns exit code 124 with `timed_out=true`, no late
   stdout, and no leaked guest `sleep` helper. The kernel command is bounded,
   shell-backed, and currently uses a guest `timeout(1)` helper for
   cancellation. The open decision is whether that command string/helper model
   is the final ABI or whether completion requires stricter argv/env/cwd
   encoding. A naive child-side mconsole rebind was tested locally and
   rejected because it panicked before the member reached `MEMBER_DONE`; see
   `2026-06-10-pool-mconsole-exec-investigation.md`. The checked-in
   `pool-mconsole-path-probe` now passes as a focused socket-addressability
   gate.
3. Decide the final request-specific warm scheduling contract. Either add a
   predeclared slot/identity API before warm fork, or route syzkaller and other
   fast consumers through daemon-assigned ready identities with
   `pool take --ready`.
4. Validate vector2 TAP/fd handoff through live pool members, then run the
   relevant vector2 networking gates against seccomp and KVM v2.
5. Validate the syzkaller UML shim against the final take/exec/destroy path,
   including stdout/stderr/status/timeout and cleanup behavior.
6. Import or complete record/replay, or land it behind an explicit
   experimental Kconfig and keep it out of the completion claim.
7. Decide whether the historical KVM v2 private state trace should be imported
   as clean optional diagnostics.
8. Curate source comments, selftests, reports, and status docs so upstream-
   facing code is free of internal issue numbers, phase diaries, random
   history, and stale claims.

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
- Baseline update: pool daemon live-member routing, warm-ready members, full
  pool benchmark documentation, and sparse physmem copying are landed and
  pushed; full-scale pool RSS and throughput remain open.

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
- Does not yet contain record/replay or private state trace source.
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
- Record/replay and private state trace functionality remain historical-only.

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

Acceptance gates:

- Record KUnit tests.
- Record smoke test.
- Replay smoke test.
- Buffer overflow behavior test.
- Gadget-on and gadget-off comparison.
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
- TAP/fd handoff works for vector2 pool members.
- Identity blob layout is documented and tested.
- Per-member mconsole path synthesis is reliable.
- Warm pool support is implemented for daemon-assigned ready members, with a
  final contract decision for request-specific warm identity scheduling.
- The full-scale pool benchmark either meets the original RSS/throughput
  targets or the targets are consciously revised with rationale.

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
- Current template-pause, fork, spawn, serve, port-forward, and reduced
  benchmark smokes pass on the live replicated-member path.
- The remaining pool/fork gaps are full default `pool-bench` RSS/throughput,
  successful daemon-routed guest exec, vector2 TAP/fd handoff through pool
  members, and syzkaller-style take/exec/destroy.
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

- Confirm all intended transports and modes from historical vector work:
  fd, tap, proxy, raw, gre, l2tpv3, vde, bess, hybrid.
- If some transports are parser-only or not implemented, either implement them
  or remove/mark them as unsupported.
- Complete multiqueue behavior and fairness validation.
- Complete sandbox validation for untrusted mode.
- Complete in-process trusted host validation.
- Confirm fd handoff from launcher/pool.
- Confirm failure injection is test-only or clearly documented.
- Align Kconfig wording with actual readiness.

Publication rule:

- `CONFIG_UML_NET_VECTOR_V2` can stay `default n`, but documentation must not
  say legacy vector is superseded until v2 passes replacement gates.

Acceptance gates:

- vector2 KUnit suites.
- vector2 sandbox audit.
- fd handoff smoke.
- tap smoke.
- multiqueue smoke and fairness/perf test.
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
- launcher smoke
- deploy smoke
- gate dry run
- `umlbuild` MVP smoke
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
- Unit or smoke test for take/exec/destroy.
- End-to-end syzkaller-style command execution smoke.
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

- Profile config build matrix.
- Instrumentation smoke tests.
- Documentation table with pass/fail/skip and rationale.

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
- Gadget status still needs final validation on the cleaned tree.

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
  warm-ready members, port-forward result handling, and reduced pool-bench
  pass on the current path.
- Full default `pool-bench` still fails RSS and throughput.
- Daemon-routed guest exec passes for command success, stdout/stderr capture,
  exit-status preservation, timeout reporting, and helper cleanup; the final
  ABI/helper dependency decision remains open.

Exit criteria:

- Pool serve/take/port-forward continues to pass on live members.
- Successful daemon-routed guest exec works through the final published ABI,
  or any experimental shell/helper dependency is explicitly documented outside
  the completion claim.
- Full pool benchmark passes, or the original RSS/throughput targets are
  revised with evidence and approval.
- Vector2 TAP/fd handoff works through live pool members.
- Syzkaller-style take/exec/destroy works through the final path.
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
- Vector2 sandbox audit.
- Vector2 fd handoff.
- Vector2 tap/multiqueue.
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
| Record/replay | Historical/prototype | Complete or experimental | Open |
| State trace | Historical/prototype | Clean optional debug infra | Open |
| Template pause | Single-shot and pivot/member paths validated; vector2 leg skips without guest `vec0` | Validated and documented | Mostly closed; vector2 leg pending |
| Fork server | Fork-on-resume smoke and default stress pass | Complete multi-iteration fork workflow plus stress | Closed for current fork-on-resume scope |
| Pool exec | Successful command, stdout/stderr capture, exit-status preservation, timeout reporting, and helper cleanup validated | Final ABI/helper dependency decision made | Validated path; ABI decision open |
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
- the remaining exec work is the final ABI decision: keep the current bounded
  shell-backed command string plus guest `timeout(1)` helper dependency, or
  replace it with stricter argv/env/cwd encoding before claiming completion.

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
	POOL_BENCH_TAKES=5 POOL_BENCH_FORKS=3 \
	POOL_BENCH_LIFECYCLE_N=5 POOL_BENCH_THROUGHPUT_S=2 \
	POOL_BENCH_THROUGHPUT_R=2 \
	tools/testing/selftests/um/pool-bench/run-pool-bench.sh
```

Result:

- PASS in the reduced validation gate;
- take p50 was 1.4 ms and p99 was 1.5 ms over five measured takes;
- RSS sampled 3/3 live sparse-copied replicated children and total RSS was
  145.9 MiB;
- lifecycle RSS drift was 0.00% over five take/destroy cycles;
- throughput completed 4/4 takes in the 2-second reduced gate;
- this proves the benchmark now measures live children, but it is not a
  substitute for the full default-scale benchmark.

Full default-scale validation:

```sh
timeout --kill-after=10 900 env UM_FORK_KERNEL=$PWD/linux \
	UMLCTL=$PWD/tools/uml/uml-launcher/target/debug/umlctl \
	tools/testing/selftests/um/pool-bench/run-pool-bench.sh
```

Result:

- FAIL overall, 3/5 gates passed;
- take p50 was 1.3 ms and p99 was 1.6 ms over 1000 measured takes;
- RSS sampled 100/100 live replicated children and total RSS improved from
  17,262.7 MiB with full physmem copy to 7,409.4 MiB with sparse extent copy,
  still failing the 200 MiB gate;
- lifecycle RSS drift was 0.09% over 10,000 take/destroy cycles;
- throughput completed 2250/3000 target takes in the 60-second gate, below the
  2700 pass threshold;
- this makes memory amplification from per-member private copies of populated
  physmem extents the next pool correctness/performance blocker.

Immediate engineering conclusion:

- current `next` already has the broad pool/fork command surface;
- single template pause, fork-on-resume smoke/stress, identity apply, pivot,
  one-shot pool member, spawn, serve, typed exec error handling, and
  port-forward result handling are real;
- replicated sustained pool-member lifetime now passes in the direct harness;
- daemon pool take/serve now routes through the replicated live-member path;
- daemon `min_warm` now prefills, consumes, replenishes, reports, and cleans up
  pre-identified ready members through `pool take --ready`;
- final completion requires fixing the full-scale pool benchmark RSS and
  throughput failures, resolving the request-specific warm scheduling decision
  or API, vector2 TAP/fd pool networking, the final daemon-routed exec ABI
  decision, and the syzkaller take/exec/destroy path.

## Immediate Next Actions

1. Fix the full-scale pool benchmark failures: 100 live replicated members
   currently consume 7,409.4 MiB RSS against the 200 MiB target after sparse
   extent copying, and 60-second throughput reaches 2250/3000 takes against the
   2700 pass threshold.
2. Decide whether request-specific warm scheduling needs a predeclared slot API
   or whether syzkaller should consume daemon-assigned ready identities through
   `pool take --ready`.
3. Decide whether the bounded shell-backed `exec` command string and guest
   `timeout(1)` helper dependency are the final daemon-routed exec ABI, or
   replace them with stricter argv/env/cwd encoding before claiming
   completion. The current `pool-exec-smoke` already validates command
   success, stdout/stderr/status, timeout reporting, late-output suppression,
   and helper cleanup.
4. Validate vector2 TAP/fd handoff through pool members and the syzkaller
   take/exec/destroy path.
5. Import or complete record/replay, or land it behind an explicit
   experimental Kconfig with docs that do not count it as mission-complete.
6. Decide whether private state trace is worth importing as clean optional
   diagnostics.
7. Re-audit vector2 transport claims, Kconfig wording, and replacement
   readiness against actual validation.
8. Curate selftests and source comments for upstream style: no internal issue
   numbers, diary prose, branch-specific commit IDs, or stale phase notes on
   upstream-facing paths.
9. Refresh reports/presentations from normalized status and evidence tables
   once functionality and validation are final.
10. Run the final validation matrix, update `STATUS.md` and the inventory,
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
