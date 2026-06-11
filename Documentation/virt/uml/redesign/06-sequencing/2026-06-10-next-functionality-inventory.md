# UML v2 next Functionality Inventory

Date: 2026-06-10

Purpose: track every known UML v2 feature area that must either be present on
`next`, imported into `next`, completed on `next`, or explicitly retired before
the UML v2 effort can be called complete.

This file is the live execution tracker for
`2026-06-10-next-full-functionality-integration-plan.md`.

## Branch Baseline

| Ref | Commit | Date | Role |
| --- | ------ | ---- | ---- |
| `next` | `d0fecac57c26` | 2026-06-10 | Authoritative integration target. |
| `origin/next` | `d0fecac57c26` | 2026-06-10 | Remote tracking ref for `next`. |
| `kvm-v2-snapshot-elf64` | `fe9616e221c7` | 2026-05-21 | KVM v2 snapshot, ELF export, record, and state trace source branch. |
| `fork-server-phase1c` | `df19046e0b49` | 2026-05-21 | Historical fork-server phase branch. |
| `memo09-phase2` | `d933f95f06c2` | 2026-05-21 | Historical identity apply phase branch. |
| `memo09-phase3-pool-bench` | `64d5eef34ca1` | 2026-05-21 | Historical pool benchmark and acceptance branch. |
| `memo09-phase4` | `83ab00dc2f33` | 2026-05-21 | Historical pool/fork-server landed branch. |
| `experiment-path-c` | `73c98eabd31b` | 2026-05-21 | Historical path-C experiment branch. |
| `umlctl-deploy` | `ea849a35e20c` | 2026-05-25 | Historical launcher/deploy/gate branch. |
| `torvalds/master` | `acb7500801e9` | 2026-06-10 | Linus baseline used for current up-to-date check. |

## Status Labels

- `Present`: code exists on `next` and has not been found broken by this
  inventory.
- `Present-KUnit-pass`: code exists on `next` and the named KUnit gate has
  passed, but broader runtime/tool validation may still be open.
- `Present-validated`: code exists on `next` and the named runtime/tool gate
  has passed.
- `Present-validated-needs-decision`: code exists on `next` and the named
  runtime/tool gate has passed, but a final ABI, support, or publication
  decision remains open.
- `Present-needs-fix`: code exists on `next`, but a correctness, validation,
  documentation, or user-surface issue is known.
- `Present-needs-runtime-builds`: profile/config source exists and configures,
  but full kernel builds and runtime probes for those profiles remain open.
- `Present-needs-profile-binary`: code exists, but the live smoke requires a
  profile-specific binary or module that was not available in the current
  checkout.
- `Historical-only`: functionality exists only on one or more historical
  branches.
- `Partial`: some of the feature exists on `next`, but required behavior is
  missing.
- `Partial-needs-fix`: some of the feature exists on `next`, and a known
  implementation fix is required before it can count as complete.
- `Stale-surface`: docs, tools, or tests on `next` refer to functionality that
  is not implemented on `next`.
- `Needs-decision`: the plan must decide whether to complete, keep
  experimental, or retire the feature.
- `Retired`: intentionally not part of the final branch. Retirement requires
  explicit user approval and a historical note.

## Feature Inventory

| Area | Feature | Current `next` status | Historical source | Required disposition | Validation gate |
| ---- | ------- | --------------------- | ----------------- | -------------------- | --------------- |
| KVM v2 core | Backend selection and VM/vCPU lifecycle | Present | `next` | Keep and harden. | KVM smoke, CPython parity. |
| KVM v2 core | Checked restore of saved XSAVE and VCPU events | Fixed-in-current-update | `next` | Keep checked fail-fast restore handling. | Build plus KVM v2 smoke. |
| KVM v2 core | LSTAR gadget and fallback syscall path | Present-validated | `next`, `kvm-v2-snapshot-elf64` | Keep the static gadget/fallback coverage and the dynamic-loader regression gate. The forced-KVM `ld-linux` fault at address `0x10` is fixed by preserving FS/GS bases after user segment refresh. Broader Tier 3 KVM workloads still run under the vector2 completion gates. | `perf-getpid` PASS `cyc_per_call=89`, `perf-pidfam` PASS `cyc_per_call=94`, `kvm-bounds` PASS 9/9, fallback static loop PASS; forced-KVM `/bin/true` clean init exit `exitcode=0`; `dyn-loader` backend=kvm PASS; `kvm_v2_marshal` 9/9 includes FS/GS-base ordering regression, 2026-06-10. |
| KVM v2 core | XSAVE/YMM preservation after AVX exposure | Present | `next` | Keep; ensure no legacy FPU restore remains in active paths. | CPython/Tier 3 flake soak. |
| KVM v2 core | APERF/MPERF passthrough | Present | `next`, `umlctl-deploy` | Keep optional. | `aperf-mperf-smoke` if config enabled. |
| KVM v2 core | RDPMC userspace support | Present | `next`, `umlctl-deploy` | Keep optional and document sandbox tradeoff. | `rdpmc-smoke` if config enabled. |
| KVM v2 core | ITIMER_VIRTUAL accounting | Present | `next` | Keep; include in KVM smoke. | timer/CPython signal smoke. |
| KVM snapshot | Register-only snapshot | Present-KUnit-pass | `kvm-v2-snapshot-elf64` | Kernel code imported and cleaned; runtime smoke still pending. | `um_kvm_v2_snapshot` KUnit PASS 4/4, 2026-06-10. |
| KVM snapshot | Full memslot snapshot capture | Present-validated | `kvm-v2-snapshot-elf64` | Kernel code imported and cleaned; live export smoke covers metadata-only large-slot behavior; SMP is gated to one online CPU. | `um_kvm_v2_snapshot` KUnit PASS 4/4 plus live export smoke PASS, 2026-06-10. |
| KVM snapshot | Snapshot restore | Present-validated | `kvm-v2-snapshot-elf64` | Kernel code imported and cleaned; SMP is gated to one online CPU. | `um_kvm_v2_snapshot` KUnit PASS 4/4 plus `kvm-snapshot-restore-smoke` PASS, 2026-06-10. |
| KVM snapshot | Snapshot ELF64 export | Present-validated | `kvm-v2-snapshot-elf64` | Kernel exporter, debugfs trigger, mconsole trigger, boot-time export trigger, and `umlctl` export path are present; SMP is gated to one online CPU. | `umlctl snapshot export` PASS with `readelf -h/-l/-n`, `gdb -c`, and helper load; `snapshot-elf-roundtrip` PASS, 2026-06-10. |
| KVM snapshot | GDB snapshot helper | Present-validated | `kvm-v2-snapshot-elf64`, `next` | Keep helper matched to UML private note layout. | helper loads against fresh exported core, 2026-06-10. |
| KVM snapshot | Snapshot selftests | Present-validated | `kvm-v2-snapshot-elf64`, `memo09-phase4` | Clean KUnit suite, restore smoke, benchmark wrapper, KUnit wrapper, and ELF roundtrip wrapper are present. | `um_kvm_v2_snapshot` KUnit PASS 4/4, `kvm-snapshot-restore-smoke` PASS, `kvm-snapshot-bench` PASS, `snapshot-kvm-smoke` PASS, and `snapshot-elf-roundtrip` PASS, 2026-06-10. |
| KVM snapshot | Snapshot benchmark kselftest | Present-validated | `memo09-phase4` | Clean wrapper around current `kvm_v2_snapshot_bench=` imported. | `kvm-snapshot-bench` PASS, 2026-06-10. |
| KVM snapshot | Snapshot KUnit kselftest wrapper | Present-validated | `memo09-phase4` | Clean wrapper for the current four-case `um_kvm_v2_snapshot` suite imported. | `snapshot-kvm-smoke` PASS, 2026-06-10. |
| KVM snapshot | Snapshot ELF roundtrip kselftest | Present-validated | `memo09-phase4` | Clean wrapper imported using `kvm_v2_snapshot_elf_export=<host-path>` and host `readelf`/`gdb` validation. | `snapshot-elf-roundtrip` PASS, 2026-06-10. |
| Record/replay | Record state machine | Present-experimental-core | `next`, `kvm-v2-snapshot-elf64` | Keep behind `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL` until live runtime integration is complete. The debugfs singleton exposes `start`, `stop`, `replay`, `strict`, and `destroy` for experimental validation. | `kvm-record-smoke` PASS: `um_kvm_v2_record` KUnit 10/10 plus live debugfs record, 2026-06-10. |
| Record/replay | Syscall observe path | Present-experimental-core | `next`, `kvm-v2-snapshot-elf64` | Current core supports explicit observe into a bounded in-memory syscall log, and the live KVM syscall dispatcher now appends post-syscall return values when an active record container is recording. The current user control surface is debugfs-only and experimental. | `kvm-record-smoke` PASS recorded 3067 live syscalls, 245360 bytes used, and 0 drops through `kvm_v2_record_ctl`, 2026-06-10. |
| Record/replay | Replay consume path | Present-experimental-core | `next`, `kvm-v2-snapshot-elf64`, `experiment-path-c` | Current core supports FIFO consume, strict mismatch reporting, cursor preservation, and live dispatcher replay of recorded syscall return values. Deterministic workload replay remains open pending time, signal, and device policy. | `um_kvm_v2_record` FIFO/end-of-log/divergence cases PASS, 2026-06-10; live dispatcher hook builds on `next`. |
| Record/replay | Gadget bypass in record mode | Present-experimental-core | `next`, `kvm-v2-snapshot-elf64` | State-page bypass byte and LSTAR fallback branch are present behind `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL`; they pair with the live dispatcher hook so gadget-handled syscalls cannot disappear from the experimental syscall log. End-to-end workload replay still needs a user ABI and determinism policy. | `um_kvm_v2_record` gadget-bypass helper PASS, `kvm_v2_byteshape` entry-sequence PASS, both under seccomp and `backend=force=kvm`; KVM `perf-getpid` gadget hot path PASS with `cyc_per_call=89`, 2026-06-10. |
| Record/replay | Snapshot-backed session start | Present-experimental-core | `next`, `kvm-v2-snapshot-elf64` | Debugfs `start` captures and attaches a KVM v2 task snapshot before enabling the record static key; snapshot-backed `replay` restores it before entering the syscall replay core. Deterministic workload replay still needs time, signal, and device policy. | `kvm-record-smoke` PASS confirms `snapshot_attempted=1`, `snapshot_valid=1`, `snapshot_rc=0`, and `snapshot_task_state=1`, 2026-06-10. |
| Record/replay | Time-travel clock event replay | Present-experimental-core | `experiment-path-c`, `next` | KVM v2 record sessions now enable the `record_replay` hook, append UML time-travel clock advances with syscall-count anchors, and consume them during replay. This closes the bounded time-travel event slice only; raw time/RDTSC/vvar/SIGALRM/device policy remains open. | `kvm-record-clock-bench` PASS: `N=100`, `observed=100`, `replayed=100`, `mismatches=0`; `um_kvm_v2_record` time-travel FIFO case PASS, 2026-06-10. |
| Record/replay | Raw time, vvar, RDTSC, SIGALRM determinism | Partial/needs-decision | `kvm-v2-snapshot-elf64`, `experiment-path-c` | Define supported tier and implement before declaring complete. UML time-travel clock events are present; raw RDTSC/vvar and SIGALRM injection are still open. | deterministic workload gate. |
| Diagnostics | KVM v2 state trace ring | Deferred-historical | `kvm-v2-snapshot-elf64` | Do not import raw historical source; current supported observability is `TRACE_EVENT` coverage. Reintroduce only as clean optional debug infrastructure if needed. | build disabled/enabled plus enable/capture/dump/clear smoke if reintroduced. |
| Diagnostics | State trace parser | Deferred-historical | `umlctl-deploy`, `kvm-v2-snapshot-elf64` | Keep as reference material until a clean kernel-side trace format lands. | parser smoke if reintroduced. |
| Template pause | Kernel template pause entry | Present-validated | `next`, `memo09-phase*`, `fork-server-phase1c` | Historical comparison complete; smoke now finishes with bounded teardown. Vector2 leg skips when guest `vec0` is absent. | `template-pause-smoke` PASS cases 1-3, SKIP case 4, 2026-06-10. |
| Template pause | Identity blob layout and apply path | Present-validated | `next`, `memo09-phase2`, `memo09-phase4` | Current layout matches the final launcher contract by source comparison and live pool-member validation. | `template-pause-pool-member-smoke` PASS, 2026-06-10. |
| Template pause | Template pause pivot mode | Present-validated | `memo09-phase4`, `next` | Keep; pivot smoke passed on current `next`. | `template-pause-pivot-smoke` PASS, 2026-06-10. |
| Fork server | Fork-on-resume loop | Present-validated | `next`, `memo09-phase4` | Keep the current production path; the corrected smoke drives the second take and default stress now passes. | `template-pause-fork-smoke` PASS, child PIDs distinct and master resume cycles=2; `template-pause-fork-stress` PASS, 548 iterations, median 18.2 ms, 548/548 clean identities, 2026-06-10. |
| Fork server | Child PID reporting through identity memfd | Present-validated | `next`, `memo09-phase4` | Keep; validate in pool take and pool-member smoke. | `pool-serve-smoke` PASS and `template-pause-pool-member-smoke` PASS, 2026-06-10. |
| Fork server | Snapshot-backed fork-server path | Historical-only/needs-decision | `memo09-phase4`, `kvm-v2-snapshot-elf64` | Complete after KVM snapshot import or retire with approval. | snapshot fork smoke. |
| Pool | Direct `umlctl pool spawn` | Present-validated | `next`, `memo09-phase*` | Keep. | `pool-spawn-smoke` PASS, 2026-06-10. |
| Pool | Daemon `pool serve` | Present-validated | `next`, `memo09-phase4` | Keep and continue through warm-pool completion. | `pool-serve-smoke` PASS, 2026-06-10. |
| Pool | `pool take` | Present-validated | `next`, `memo09-phase4` | Keep and extend for warm ready members. | `pool-serve-smoke` PASS, 2026-06-10. |
| Pool | `pool list/status/destroy/shutdown` | Present-validated | `next`, `memo09-phase4` | Keep. | `pool-spawn-smoke` PASS and `pool-serve-smoke` PASS, 2026-06-10. |
| Pool | Warm member pool | Present-validated | `memo09-phase3-pool-bench`, `memo09-phase4`, `next` | Keep the two-mode contract: request-specific takes stay lazy because identity is applied before fork; returned members are live but quiesced until daemon-routed `exec` resumes them; `pool take --ready` consumes daemon-assigned pre-identified members and cannot be combined with caller-supplied identity. No predeclared slot API is required for the current completion claim. | `pool-serve-smoke` PASS with `--min-warm=1`, ready take, replenish, destroy, and shutdown; `pool-exec-smoke` proves resume-before-exec; syzkaller remains on the request-specific lazy take path because it needs deterministic TAP/IP identity, 2026-06-10. |
| Pool | Pool benchmark thresholds | Present-validated | `memo09-phase3-pool-bench`, `memo09-phase4` | Full default benchmark now gates memory amplification on PSS while still reporting summed RSS as diagnostic context; this matches the quiesced live-member model where executable/libc/tmpfs pages are intentionally shared. | Full `pool-bench` PASS 5/5: p50 1.8 ms, p99 2.5 ms, `pss.100forks` 139.0 MiB PSS with 499.3 MiB summed RSS and 17.6 MiB private dirty for 100/100 live members, lifecycle drift 0.05%, throughput 3000/3000, 2026-06-10. |
| Pool | mconsole path synthesis | Present-validated | `next`, `memo09-phase4` | Keep the master-side bind plus child-side SIGIO rearm model; avoid the rejected child-side rebind experiment that panicked before `MEMBER_DONE`. | `pool-mconsole-path-probe` PASS with member alive, per-member socket present, and `version` reply; focused investigation in `2026-06-10-pool-mconsole-exec-investigation.md`, 2026-06-10. |
| Pool | `umlctl exec` via daemon | Present-validated | `next`, `memo09-phase4` | Keep `exec/1` as the current public ABI: callers send structured argv/env/cwd/timeout to the daemon and receive NDJSON frames. The current mconsole backend deliberately lowers that request through a bounded shell command; `--timeout` requires guest `timeout(1)`. Any stricter kernel transport is future `exec/2` work, not required for the current completion claim. | `pool-exec-smoke` PASS: `/bin/true` exits 0, stdout/stderr capture round-trips, guest exit 7 is preserved without a daemon error, timeout returns code 124 with `timed_out=true`, late stdout is suppressed, no extra guest `sleep` helper leaks, and stale `Unknown command`/missing-host-tool boundaries are rejected; historical `memo09`/`umlctl-deploy` branches had the same outer RPC/NDJSON intent and no stricter kernel argv transport to import, 2026-06-10. |
| Pool | `umlctl port-forward` | Present-validated | `next`, `memo09-phase4` | Keep and later validate against final network mode. | `pool-port-forward-smoke` PASS, 2026-06-10. |
| Pool | Vector2 TAP handoff | Present-validated | `next`, `memo09-phase4`, `umlctl-deploy` | Keep the vector2 TAP reopen path and smoke gate. Per-take pool fd handoff is retired from the current completion claim; current pool takes carry string identity through the identity memfd and reopen vec2 TAP by per-member TAP name. | `vector2-pool-tap-smoke` PASS: per-member TAP/MAC/IPv4 identity visible through daemon exec and one-packet host TAP ping succeeds; source audit confirms `um_template_identity_apply()` calls `um_vec2_tap_reopen_for_pool_member()` before IPv4/route apply, 2026-06-10. |
| Vector2 | Typed parser | Present | `next` | Keep. | vector2 parser KUnit. |
| Vector2 | Queue ownership | Present | `next` | Keep. | vector2 queue KUnit. |
| Vector2 | fd backend | Present-validated-needs-long-gates | `next`, `umlctl-deploy` | Keep the launcher-owned inherited-fd path for standalone `umlctl up`; per-take pool fd handoff is not part of the current completion claim. | `vector2-fd-handoff-smoke` PASS: `umlctl up` creates the TAP, opens/inherits fd 200, guest metadata reports fd transport, `vec2.0` has the assigned IPv4 address, and one-packet host TAP ping succeeds; historical branch grep found the pool SCM_RIGHTS TAP-fd swap deferred rather than implemented, 2026-06-10. |
| Vector2 | tap backend | Present-validated-needs-long-gates | `next`, `umlctl-deploy` | Keep and run networking/Tier 3 gates. | `vector2-pool-tap-smoke` PASS on the pool path, 2026-06-10; Tier 3 still required. |
| Vector2 | multiqueue | Present-validated-needs-fairness | `next`, `umlctl-deploy` | Keep the launcher-owned multiqueue fd handoff path; finish fairness/performance gates. | `vector2-fd-multiqueue-smoke` PASS: `umlctl up` creates a multiqueue TAP, opens/inherits fds 200..203, guest metadata reports four queues/fds, `vec2.0` reports four TX queues, and one-packet host TAP ping succeeds, 2026-06-10. |
| Vector2 | raw/gre/l2tpv3/vde/bess/proxy/hybrid transports | Parser-only-retired-from-runtime-claim | `next`, historical vector branches | Do not count these as implemented runtime transports on `next`; keep GRE/L2TPv3 header helpers under KUnit and require new netdev backends plus live smokes before restoring any runtime claim. | Netdev KUnit guards parser-only transports returning `-EOPNOTSUPP`; GRE/L2TPv3 header-helper KUnit remains present, 2026-06-10. |
| Vector2 | failed-open fault injection | Present-validated-validation-only | `next`, `umlctl-deploy` | Keep as a validation-only open-unwind knob; leave unset for normal workloads. | `vector2-failed-open` PASS on the current tree: fd-handoff TAP path with `fail_open_after=2`, host gateway ping succeeds, second `ndo_open()` fails by injection, `open_delta=1`, `fail_delta=1`, `close_delta=1`, `vec2.0` is left closed/registered, and the host TAP is cleaned up, 2026-06-10. |
| Vector2 | sandbox mode | Present-validated | `next` | Keep the default-safe audit gate. | `vector2-sandbox-audit` PASS: `umlctl gate loop --audit-vector-sandbox` ran a vector2 auto-queue fd boot and reported `PASS=1/1 FAIL=0 TIMEOUT=0` with no forbidden host operations, 2026-06-10. |
| Vector2 | in-process trusted mode | Present-validated | `next` | Keep explicit and document authority. | `vector2-inproc-tap-smoke` PASS: `umlctl up` reports TAP/inproc mode without launcher fd inheritance, guest metadata reports TAP/inproc with fd count zero, `vec2.0` has the assigned IPv4 address, and one-packet host TAP ping succeeds, 2026-06-10. |
| Vector2 | replacement-readiness claim | Claim-bounded-needs-gates | `next` docs/Kconfig | Keep v2 opt-in and avoid saying legacy vector is superseded until Tier 3, fairness, and long-run gates pass. | Kconfig/help text no longer describes legacy vector as superseded or tells new deployments to prefer v2; vector2 help names TAP/fd as the current netdev runtime scope, 2026-06-10. |
| Launcher | `umlctl up/down/ps/logs` | Present-validated | `next`, `umlctl-deploy` | Keep. | `umlctl-smoke` PASS, 2026-06-10. |
| Launcher | deploy configs | Present-validated | `next`, `umlctl-deploy` | Keep current example/profile set; current `umlctl` uses `up/down` plus Umlfile parsing, not a separate deploy subcommand. No useful historical deploy TOML is missing from `next`. | File-set comparison found no example/profile files unique to `umlctl-deploy`; all 18 example Umlfiles pass `UML_KERNEL=$PWD/linux umlctl up --dry-run`; all 5 built-in profiles resolve with `umlbuild profile show`, 2026-06-10. |
| Launcher | gates and gate-loop | Present-validated | `next`, `umlctl-deploy` | Keep. | `umlctl gate run --dry-run` against `tools/testing/selftests/um/gates/launcher-cargo.toml` PASS, 2026-06-10. |
| Launcher | snapshot export CLI | Present-validated | `next`, `kvm-v2-snapshot-elf64` | Keep mconsole-driven host export path. | live `umlctl snapshot export` smoke PASS, 2026-06-10. |
| Launcher | transparency tooling | Present-validated | `next`, `umlctl-deploy` | Keep. | `run-bpftrace-validate.sh` PASS: all five scripts attached; syscalls and sched produced idle UML data, 2026-06-10. |
| Launcher | `umlbuild` | Present-validated | `next`, `umlctl-deploy` | Keep MVP smoke and the clean-source override path for developer trees with in-tree build products. | `run-umlbuild-mvp.sh` PASS with `UMLBUILD_SOURCE` set to a temporary clean worktree and prebuilt debug `umlbuild`/`umlctl`; direct boot and `umlctl up` both produced the expected guest sha256, 2026-06-10. |
| Syzkaller | UML VM shim | Present-validated | `next`, `umlctl-deploy` | Keep the shim on request-specific lazy `pool take` plus the validated `umlctl` JSON contracts, including `exec/1` NDJSON frames. Future `exec/2` changes require a schema bump and shim update. | `syzkaller-shim-smoke` PASS: source contract check plus syzkaller-style take, exec output merge, port-forward, status, destroy, 2026-06-10. |
| Profiles | profile configs | Present-needs-runtime-builds | `next`, historical docs | Keep the 10 kernel Kconfig profiles and the 5 `umlbuild` profiles; full per-profile kernel builds and runtime feature probes remain required before final completion. | Clean-worktree `make ARCH=um O=<out> uml/<profile>` config matrix PASS for all 10 kernel profiles, `research-kmsan` now documented and listed in arch help, and all 5 `umlbuild` profiles resolve with `umlbuild profile show`, 2026-06-10. |
| Instrumentation | kprobes | Present-needs-profile-binary | `next` | Keep and test with a research-profile build that includes `samples/kprobes/kretprobe_example.ko`. | Current `./linux` has `CONFIG_KPROBES=y`/`CONFIG_KRETPROBES=y`, but `kprobes-stress` SKIPed because `kretprobe_example.ko` was not built, 2026-06-10. |
| Instrumentation | ftrace | Present-validated | `next` | Keep normal dynamic function tracing; do not advertise function-graph tracing until the UML return-stack interaction is fixed. | Clean-worktree ftrace build with `FUNCTION_TRACER=y` and no `FUNCTION_GRAPH_TRACER` passed `ftrace-smoke` with 51,300 trace lines; `um/ftrace-smoke` now fails fast if a binary advertises unsupported `function_graph`, 2026-06-10. |
| Instrumentation | KMSAN | Present-needs-profile-binary | `next` | Keep `research-kmsan`; test with a clang/LLVM KMSAN profile binary. | `research-kmsan` config matrix PASS; current `./linux` `kmsan-smoke` SKIPed because the binary does not contain the KMSAN runtime, 2026-06-10. |
| Instrumentation | KASAN/KCSAN/KFENCE/KCOV | Partial/needs-validation | `next`, docs | Verify each profile and document gaps. | instrumentation matrix. |
| Instrumentation | BPF/JIT shims | Present-needs-validation | `next` | Keep if tests pass and docs are accurate. | BPF smoke if available. |
| Instrumentation | KGDB | Needs-decision | original plan/docs | Implement, document absent, or retire with approval. | KGDB smoke if kept. |
| Selftests | Upstreamable smoke/regression tests | Partial-cleaned | all branches | Continue curation; active gate metadata and tier smoke fixtures now use semantic group names and neutral test data instead of planning phase labels. | Focused scan over `arch/um`, `tools/testing/selftests/um`, and `tools/uml/uml-launcher` found no standalone workstream/decision/memo/phase labels after cleanup, excluding operational post-mortem wording and archived redesign docs, 2026-06-10. |
| Selftests | local stress/soak tests | Present | `next`, `umlctl-deploy` | Keep under clear local/soak docs. | selected soak gates. |
| Selftests | historical repros | Present/needs-cleanup | all branches | Move or mark archival. | grep for diary/internal labels. |
| Docs | `STATUS.md` | Present-current | `next` | Update after every landed workstream. | status review updated with launcher/selftest curation boundary, 2026-06-10. |
| Docs | old reports/presentations | Present-stale | `next`, historical docs | Refresh or mark historical. | docs audit. |
| Docs | snapshot ELF docs | Present-needs-validation | `next`, `kvm-v2-snapshot-elf64` | Re-audit against imported kernel code and fresh exported core. | trigger/symbol grep. |
| Docs | active UML user docs | Present-cleaned | `next` | Keep non-redesign UML docs focused on current interfaces and limitations, not workstream history. | Normalized ftrace, kprobes, KMSAN, debugfs, section-split, snapshot/forkserver, launcher, APERF/MPERF, and profile docs; focused scan over non-redesign `Documentation/virt/uml/*.rst` now has no standalone workstream/decision/memo/phase-history labels apart from an OpenWrt sample `#0` line, 2026-06-10. |
| Docs | vector2 validation docs | Present-needs-update | `next` | Align with final gates. | docs audit. |
| Upstream | patch queue | Present-stale | `next` docs | Regenerate from final `next`. | checkpatch and cover letters. |

## Closed In Current Update

- KVM v2 no longer ignores failed architectural-state restore ioctls for
  `KVM_SET_MSRS`, `KVM_SET_XSAVE`, or `KVM_SET_VCPU_EVENTS`.
- KVM v2 experimental record/replay core is present on `next` behind
  `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL`. It includes the
  single-active container, lifecycle, strict replay flag, bounded syscall
  observe, FIFO consume, divergence cursor preservation, and overflow
  accounting, plus LSTAR gadget-bypass plumbing, snapshot-backed debugfs
  session start, time-travel clock event replay, and an experimental singleton
  control/status surface for live dispatcher logging. Validation:
  `make ARCH=um -j16`, `um_kvm_v2_record` 10/10, `kvm_v2_marshal` 9/9,
  `kvm_v2_byteshape` 9/9, `kvm-record-smoke` live snapshot-backed record PASS,
  and `kvm-record-clock-bench` PASS on 2026-06-10.
- KVM v2 dynamic-loader/TLS startup now passes the focused gates: forced-KVM
  `/bin/true` reaches clean init exit with `exitcode=0`, and the dyn-loader
  kselftest reports `DYN_LOADER: backend=kvm PASS`.
- KVM v2 snapshot capture/restore and snapshot ELF64 export source is present
  on `next` and builds with `make ARCH=um -j16`.
- KVM v2 snapshot KUnit coverage is present on `next` and passes under
  `backend=force=kvm-v2` with
  `kunit.filter_glob=um_kvm_v2_snapshot kunit_shutdown=halt`:
  4 pass, 0 fail, 0 skip.
- Live `umlctl snapshot export` works against a disposable KVM v2 hostfs
  guest and the resulting ELF parses with `readelf -h/-l/-n`, `gdb -c`, and
  `tools/uml/uml-gdb/uml-snapshot.py`.
- Snapshot restore smoke works through `kvm_v2_snapshot_bench=1` and the
  `kvm-snapshot-restore-smoke` selftest, which observes a capture plus
  `restore_full` timing summary in full snapshot mode.
- Snapshot SMP semantics are explicitly gated: capture and restore return
  `-EOPNOTSUPP` when more than one CPU is online.
- Daemon-routed pool exec now validates success, stdout/stderr capture, guest
  exit status preservation, timeout reporting, and guest helper cleanup through
  `pool-exec-smoke`; `exec/1` is the current public ABI, while any stricter
  kernel argv transport is future `exec/2` work.
- The full default pool benchmark is closed for the current completion claim:
  returned members are quiesced until `exec`, the memory gate uses PSS to avoid
  summed-RSS double-counting of intentionally shared mappings, and
  `pool-bench` passes 5/5 on the current tree.
- Active launcher/selftest planning-label cleanup landed for `Cargo.toml`,
  SELinux policy headers, sandbox/research launcher examples, gate metadata,
  tier smoke fixtures, and adjacent launcher/profile docs. Validation:
  `cargo fmt --check`, `cargo test`, `python3 -m py_compile` for touched tier
  smoke scripts, `umlctl gate list --source-root .`, dry-run parse of all
  gate TOMLs, `uml-launcher run --dry-run --config` for the touched legacy
  launcher examples, and `git diff --check` all passed on 2026-06-10.
- Active non-redesign UML docs no longer use workstream, decision-log, memo,
  phase-history, or future-phase labels as current user-facing explanation.
  The focused scan exception is a real OpenWrt sample kernel version line
  containing `#0`.

## Remaining Hard Blockers

These items must be closed before the final branch can be called complete:

1. Record/replay runtime functionality must be completed beyond the current
   experimental core, snapshot-backed start, and live record smoke before the
   original record/replay mission is closed.
2. Vector2 replacement claims must match validation evidence.
3. Pool/fork-server current tests must pass, including warm-pool and
   pool-member paths. Vector2 pool-member TAP and launcher-owned fd handoff
   now have dedicated smoke gates; per-take pool fd handoff has been retired
   from the current completion claim in favor of the validated TAP reopen path.
4. Old reports, presentations, and historical repro notes still need final
   archival marking. Focused scans over active source, launcher, selftests,
   and non-redesign UML docs are clean for the targeted standalone
   planning-label patterns as of 2026-06-10.
5. The final validation matrix from the integration plan must pass.

## Next Update Rules

When a workstream lands:

1. Change affected rows from `Open`/`Partial`/`Historical-only` to the new
   status.
2. Add the landed commit hash.
3. Add the validation command and result.
4. Update `STATUS.md` if the user-visible project status changed.
5. Leave historical branch references intact until the feature is either fully
   imported or explicitly retired.
