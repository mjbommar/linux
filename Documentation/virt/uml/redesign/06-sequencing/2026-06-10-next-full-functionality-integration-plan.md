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
- Baseline before this integration update: `58e50e5dedf7`
- Relative to local `torvalds/master`: `0` behind, `41` ahead
- Current update: mconsole-backed snapshot export integration, validation, and
  documentation are being landed together.

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
  KUnit coverage. Runtime restore smoke and SMP semantics still need closure.
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

## Current `next` Blockers

These should be fixed before major imports.

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
  feature must be gated or documented as single-vCPU only.

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
- Snapshot restore runtime smoke passes. Current status: open.
- SMP behavior is either passing or explicitly gated.

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
- `umlctl exec` works through mconsole or a documented fallback.
- `port-forward` works for pool members.
- TAP/fd handoff works for vector2 pool members.
- Identity blob layout is documented and tested.
- Per-member mconsole path synthesis is reliable.
- Warm pool support is either implemented or explicitly marked not part of
  completion.

Required historical comparison:

- Diff current `next` against each `memo09-*` branch for `tools/uml`,
  `arch/um/kernel/template_pause*`, `arch/um/include/asm/um-template-pause.h`,
  and `tools/testing/selftests/um/*pool*`.
- Import only missing behavior.
- Keep current newer launcher modules where they supersede older branch code.

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

Exit criteria:

- Current `next` has no known hard correctness bug from the 2026-06-10 review.

### Phase 2: Snapshot And Snapshot ELF

Port or reimplement snapshot and snapshot ELF export.

Exit criteria:

- `umlctl snapshot export` works on `next`.
- Snapshot docs are true.
- Snapshot KUnit and live ELF export smoke pass. Current status: KUnit PASS
  4/4 and live `umlctl snapshot export` PASS on 2026-06-10.
- Snapshot restore runtime smoke passes or the unsupported state is explicitly
  gated.
- SMP snapshot semantics are validated or explicitly gated.

### Phase 3: Fork Server And Pool Completion

Consolidate pool/fork-server behavior.

Exit criteria:

- Pool serve/take/exec/port-forward works.
- Pool benchmark and smoke tests pass.
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
- Snapshot restore smoke. Current status: open.
- Snapshot ELF export roundtrip. Current status: live `umlctl snapshot export`
  plus `readelf`, `gdb`, and helper parse PASS on 2026-06-10.
- Template-pause fork smoke.
- Template-pause fork stress.
- Pool spawn smoke.
- Pool serve smoke.
- Pool exec smoke.
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
| KVM snapshot | Present with KUnit pass | Present, validated, SMP policy defined | In progress |
| Snapshot ELF export | Present with live export pass | Working and documented on `next` | Closed for live export |
| Record/replay | Historical/prototype | Complete or experimental | Open |
| State trace | Historical/prototype | Clean optional debug infra | Open |
| Template pause | Present | Validated and documented | Open |
| Fork server | Present/partial | Complete pool workflow | Open |
| Pool exec | Present | Validated with mconsole path | Open |
| Pool port-forward | Present | Validated | Open |
| Vector2 | Present, experimental | Replacement-ready or claims reduced | Open |
| Syzkaller shim | Present | End-to-end smoke | Open |
| Profiles | Present | Build/test matrix | Open |
| Selftests | Broad, noisy | Curated suites | Open |
| Docs/reports | Mixed current/stale | Truthful final status | Open |
| Upstream queue | Drafted/stale | Refreshed from final `next` | Open |

## Immediate Next Actions

1. Add or run a snapshot restore runtime smoke test.
2. Define the SMP snapshot policy: validate all-vCPU quiescence, gate the
   feature to UP/single-vCPU, or mark SMP snapshot unsupported with explicit
   checks.
3. Compare pool/fork-server behavior against `fork-server-phase1c`,
   `memo09-phase2`, `memo09-phase3-pool-bench`, and `memo09-phase4`.
4. Import or complete record/replay, or land it behind an explicit
   experimental Kconfig with docs that do not count it as mission-complete.
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
