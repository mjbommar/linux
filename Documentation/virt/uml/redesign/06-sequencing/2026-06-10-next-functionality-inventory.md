# UML v2 next Functionality Inventory

Date: 2026-06-10

Last refreshed: 2026-06-11

Purpose: track every known UML v2 feature area that must either be present on
`next`, imported into `next`, completed on `next`, or explicitly retired before
the UML v2 effort can be called complete.

This file is the live execution tracker for
`2026-06-10-next-full-functionality-integration-plan.md`.

## Branch Baseline

| Ref | Commit | Date | Role |
| --- | ------ | ---- | ---- |
| `next` | `59ad334001ea` | 2026-06-11 | Authoritative integration target. |
| `origin/next` | `59ad334001ea` | 2026-06-11 | Remote tracking ref for `next`. |
| `kvm-v2-snapshot-elf64` | `fe9616e221c7` | 2026-05-21 | KVM v2 snapshot, ELF export, record, and state trace source branch. |
| `fork-server-phase1c` | `df19046e0b49` | 2026-05-21 | Historical fork-server phase branch. |
| `memo09-phase2` | `d933f95f06c2` | 2026-05-21 | Historical identity apply phase branch. |
| `memo09-phase3-pool-bench` | `64d5eef34ca1` | 2026-05-21 | Historical pool benchmark and acceptance branch. |
| `memo09-phase4` | `83ab00dc2f33` | 2026-05-21 | Historical pool/fork-server landed branch. |
| `experiment-path-c` | `73c98eabd31b` | 2026-05-21 | Historical path-C experiment branch. |
| `umlctl-deploy` | `ea849a35e20c` | 2026-05-25 | Historical launcher/deploy/gate branch. |
| `torvalds/master` | `9716c086c8e8` | 2026-06-11 | Linus baseline used for current up-to-date check. |

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
- `Present-needs-runtime-fix`: code exists and the matching profile binary
  builds, but a runtime correctness or initialization issue still blocks the
  focused smoke gate.
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
| Record/replay | Record state machine | Present-experimental-core | `next`, `kvm-v2-snapshot-elf64` | Keep behind `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL` until live runtime integration is complete. The debugfs singleton exposes `start`, `stop`, `replay`, `strict`, and `destroy` for experimental validation, and status now reports whether recorded syscalls came from the snapshot owner or other tasks plus format metadata, payload entry/byte counters, and strict replay failure counters. | `kvm-record-smoke` PASS: `um_kvm_v2_record` KUnit 24/24 plus live debugfs record, task-owned payload record helper, and direct-syscall/UML-vDSO raw-time payload helper, 2026-06-11. |
| Record/replay | Versioned event format | Present-experimental-core | `next` | Replay entries now carry explicit format version and flags fields. Debugfs reports `format_version`, `entry_header_size`, `entry_size`, and `max_payload`, and replay rejects stale or flagged entries without advancing the cursor. This is still an in-memory experimental format, not a stable user ABI. | `um_kvm_v2_record` versioned-format contract case PASS; `kvm-record-smoke` PASS with KUnit 24/24, 2026-06-11. |
| Record/replay | Syscall observe path | Present-experimental-core | `next`, `kvm-v2-snapshot-elf64` | Current core supports explicit observe into a bounded in-memory syscall log, and the live KVM syscall dispatcher now appends post-syscall return values when an active record container is recording. The first payload-aware syscalls are `uname(2)`, which records the returned `struct new_utsname`, and `getcwd(2)`, which records the returned path bytes; the task-owned helper validates a single process can snapshot itself, record its scalar plus payload workload, and replay it from the same log. | `kvm-record-smoke` PASS recorded live syscalls through `kvm_v2_record_ctl`; task-owned leg reports `entries=386`, `syscalls=386`, `same=386`, `other=0`, `payload_entries=2`, `payload_bytes=392`, and `replayed=386`, 2026-06-11. |
| Record/replay | Replay consume path | Present-experimental-core | `next`, `kvm-v2-snapshot-elf64`, `experiment-path-c` | Current core supports FIFO consume, strict mismatch reporting, cursor preservation, live dispatcher replay of recorded syscall return values, version/flags format rejection, payload restore for `uname(2)`, `getcwd(2)`, `clock_gettime(2)`, `gettimeofday(2)`, and `time(2)`, and auto-disarm once replay consumes the full log. Strict replay now fails closed for syscalls outside the bounded subset instead of replaying arbitrary scalar-only entries. Broader payload, signal-event ordering, and replayable device policy remain open. | `um_kvm_v2_record` KUnit 24/24 covers FIFO/end-of-log/divergence plus payload restore, `getcwd(2)` payload restore, raw-time payload restore, too-small restore buffer, argument mismatch, overflow, version/flags rejection, strict syscall-policy coverage, strict raw-time syscall policy, strict external-I/O/randomness rejection, strict-on/strict-off gate behavior, strict failure accounting, and cursor preservation; `kvm-record-smoke` task-owned replay consumes 386/386 entries and raw-time replay covers direct syscall plus current UML vDSO wrapper calls, 2026-06-11. |
| Record/replay | Gadget bypass in record mode | Present-experimental-core | `next`, `kvm-v2-snapshot-elf64` | State-page bypass byte and LSTAR fallback branch are present behind `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL`; they pair with the live dispatcher hook so gadget-handled syscalls cannot disappear from the experimental syscall log. End-to-end workload replay still needs a user ABI and determinism policy. | `um_kvm_v2_record` gadget-bypass helper PASS, `kvm_v2_byteshape` entry-sequence PASS, both under seccomp and `backend=force=kvm`; KVM `perf-getpid` gadget hot path PASS with `cyc_per_call=89`, 2026-06-10. |
| Record/replay | Snapshot-backed session start | Present-experimental-core | `next`, `kvm-v2-snapshot-elf64` | Debugfs `start` captures and attaches a KVM v2 task snapshot before enabling the record static key; snapshot-backed `replay` restores it before entering the syscall replay core. The task-owned smoke now proves that a process can snapshot itself and keep the focused scalar plus `uname(2)`/`getcwd(2)` payload workload on that same task. Deterministic workload replay still needs broader payload, time, signal, and device policy. | `kvm-record-smoke` PASS confirms `snapshot_attempted=1`, `snapshot_valid=1`, `snapshot_rc=0`, `snapshot_task_state=1`, task-owned PID matching, no other-task syscalls, and two payload entries totaling 392 bytes in the task-owned helper, 2026-06-11. |
| Record/replay | Time-travel clock event replay | Present-experimental-core | `experiment-path-c`, `next` | KVM v2 record sessions now enable the `record_replay` hook, append UML time-travel clock advances with syscall-count anchors, and consume them during replay. This closes the bounded time-travel event slice only; replayable raw-time payloads and device policy remain open. Replay mode now blocks `SIGALRM` during `KVM_RUN` rather than treating timer delivery as a replayable event. | `kvm-record-clock-bench` PASS: `N=100`, `observed=100`, `replayed=100`, `mismatches=0`; `um_kvm_v2_record` time-travel FIFO case PASS, 2026-06-11. |
| Record/replay | Strict unsupported syscall policy | Present-experimental-core | `next` | Initial R/R-1 strict replay accepts only `getpid`, `getppid`, `gettid`, payload-aware `uname(2)`/`getcwd(2)`, and payload-aware `clock_gettime(2)`/`gettimeofday(2)`/`time(2)`. Other syscalls fail closed with `-EOPNOTSUPP`, SIGSEGV, and debugfs failure counters rather than falling back to live execution or scalar-only replay. Broader syscall coverage must be added deliberately with payload/time/device policy. | `um_kvm_v2_record` strict syscall-policy, strict gate, and failure-accounting cases PASS, including unsupported `getuid` and `getrandom` policy checks where those syscall numbers are available; `kvm-record-smoke` live-negative legs arm strict replay and observe `getrandom` rejected as `nr=318`, `openat` rejected as `nr=257`, `read` rejected as `nr=0`, `write` rejected as `nr=1`, and `ioctl` rejected as `nr=16`, 2026-06-11. |
| Record/replay | Raw time syscall and timestamp-instruction policy | Present-experimental-core | `next` | UML vDSO clock helpers route through syscalls. The current bounded tier replays direct syscall and current UML vDSO wrapper calls for `clock_gettime(2)`, `gettimeofday(2)`, and `time(2)` from logged payload bytes. KVM v2 sets CR4.TSD while replay is active so direct user timestamp reads fault instead of observing host time outside the log. Raw-time coverage beyond those syscall-wrapper paths, including native VVAR-style fast paths, remains future work. | `um_kvm_v2_record` strict time-policy and raw-time payload cases PASS; `kvm-record-smoke` PASS with `KVM_RECORD_TIME: PASS syscall_clock=... vdso_clock=...`, `live-time=1`, `live-rdtsc=1`, and `live-rdtscp=1`, 2026-06-11. |
| Record/replay | SIGALRM determinism | Present-experimental-blocked | `next`, `kvm-v2-snapshot-elf64`, `experiment-path-c` | R/R-1 chooses the blocked-delivery tier rather than replayable signal events: normal KVM execution keeps `SIGALRM` unblocked for timer preemption, but replay mode reprograms the per-vCPU `KVM_SET_SIGNAL_MASK` to block `SIGALRM` while inside `KVM_RUN`. Pending UML timer work is handled after VM exit, and workloads that depend on precise asynchronous signal ordering remain outside the R/R-1 contract. | `kvm-record-smoke` PASS with `KUnit=24/24`; `kvm-record-clock-bench` PASS, 2026-06-11. |
| Record/replay | Device and randomness policy | Present-experimental-fail-closed | `next` | R/R-1 does not claim replayable randomness, network, block, or hostfs mutation. Strict replay rejects randomness and representative external-I/O syscalls outside the supported subset, including `getrandom`, `openat`, `read`, `write`, and `ioctl`, instead of serving them from scalar-only replay entries. Replayable device payloads remain future work. | `um_kvm_v2_record` strict external-I/O/randomness policy case PASS; `kvm-record-smoke` PASS with `live-negative=1` for `getrandom` and `live-external-io=4/4` for `openat`/`read`/`write`/`ioctl`, 2026-06-11. |
| Record/replay | Deterministic task-owned workload replay | Present-experimental-core | `next` | The task-owned helper now suppresses the debugfs control `write(2)` from the record log, records only the bounded scalar plus `uname(2)`/`getcwd(2)` payload workload, replays it from the snapshot-backed log, and auto-disarms when the cursor reaches the end. This proves the first R/R-1 deterministic workload, not whole-system replay. | `KVM_RECORD_TASK: PASS pid=1 entries=386 syscalls=386 same=386 other=0 payload_entries=2 payload_bytes=392 replayed=386 sink=499`, 2026-06-11. |
| Diagnostics | KVM v2 state trace ring | Deferred-historical | `kvm-v2-snapshot-elf64` | Do not import raw historical source; current supported observability is `TRACE_EVENT` coverage. Reintroduce only as clean optional debug infrastructure if needed. | build disabled/enabled plus enable/capture/dump/clear smoke if reintroduced. |
| Diagnostics | State trace parser | Deferred-historical | `umlctl-deploy`, `kvm-v2-snapshot-elf64` | Keep as reference material until a clean kernel-side trace format lands. | parser smoke if reintroduced. |
| Template pause | Kernel template pause entry | Present-validated | `next`, `memo09-phase*`, `fork-server-phase1c` | Historical comparison complete; smoke now finishes with bounded teardown. Vector2 leg skips when guest `vec0` is absent. | `template-pause-smoke` PASS cases 1-3, SKIP case 4, rebuilt `59ad334001ea`, 2026-06-11. |
| Template pause | Identity blob layout and apply path | Present-validated | `next`, `memo09-phase2`, `memo09-phase4` | Current layout matches the final launcher contract by source comparison and live pool-member validation. | `template-pause-pool-member-smoke` PASS with identity fd parsed and five timer ticks, rebuilt `59ad334001ea`, 2026-06-11. |
| Template pause | Template pause pivot mode | Present-validated | `memo09-phase4`, `next` | Keep; pivot smoke passed on current `next`. | `template-pause-pivot-smoke` PASS, 20 pivots, rebuilt `59ad334001ea`, 2026-06-11. |
| Fork server | Fork-on-resume loop | Present-validated | `next`, `memo09-phase4` | Keep the current production path; the corrected smoke drives the second take and default stress now passes. | `template-pause-fork-smoke` PASS, child PIDs distinct and master resume cycles=2; `template-pause-fork-stress` PASS, 540 iterations, median 18.5 ms, 540/540 clean identities, rebuilt `59ad334001ea`, 2026-06-11. |
| Fork server | Child PID reporting through identity memfd | Present-validated | `next`, `memo09-phase4` | Keep; validate in pool take and pool-member smoke. | `pool-serve-smoke` PASS and `template-pause-pool-member-smoke` PASS, rebuilt `59ad334001ea`, 2026-06-11. |
| Fork server | Snapshot-backed fork-server path | Historical-only/needs-decision | `memo09-phase4`, `kvm-v2-snapshot-elf64` | Complete after KVM snapshot import or retire with approval. | snapshot fork smoke. |
| Pool | Direct `umlctl pool spawn` | Present-validated | `next`, `memo09-phase*` | Keep. | `pool-spawn-smoke` PASS, rebuilt `59ad334001ea`, 2026-06-11. |
| Pool | Daemon `pool serve` | Present-validated | `next`, `memo09-phase4` | Keep and continue through warm-pool completion. | `pool-serve-smoke` PASS, rebuilt `59ad334001ea`, 2026-06-11. |
| Pool | `pool take` | Present-validated | `next`, `memo09-phase4` | Keep and extend for warm ready members. | `pool-serve-smoke` PASS, rebuilt `59ad334001ea`, 2026-06-11. |
| Pool | `pool list/status/destroy/shutdown` | Present-validated | `next`, `memo09-phase4` | Keep. | `pool-spawn-smoke` PASS and `pool-serve-smoke` PASS, rebuilt `59ad334001ea`, 2026-06-11. |
| Pool | Warm member pool | Present-validated | `memo09-phase3-pool-bench`, `memo09-phase4`, `next` | Keep the two-mode contract: request-specific takes stay lazy because identity is applied before fork; returned members are live but quiesced until daemon-routed `exec` resumes them; `pool take --ready` consumes daemon-assigned pre-identified members and cannot be combined with caller-supplied identity. No predeclared slot API is required for the current completion claim. | `pool-serve-smoke` PASS with `--min-warm=1`, ready take, replenish, destroy, and shutdown; `pool-exec-smoke` proves resume-before-exec; syzkaller remains on the request-specific lazy take path because it needs deterministic TAP/IP identity, rebuilt `59ad334001ea`, 2026-06-11. |
| Pool | Pool benchmark thresholds | Present-validated | `memo09-phase3-pool-bench`, `memo09-phase4` | Full default benchmark now gates memory amplification on PSS while still reporting summed RSS as diagnostic context; this matches the quiesced live-member model where executable/libc/tmpfs pages are intentionally shared. | Full `pool-bench` PASS 5/5: p50 1.8 ms, p99 2.4 ms, `pss.100forks` 138.9 MiB PSS with 510.0 MiB summed RSS and 17.5 MiB private dirty for 100/100 live members, lifecycle drift 0.05%, throughput 3000/3000, rebuilt `59ad334001ea`, 2026-06-11. |
| Pool | mconsole path synthesis | Present-validated | `next`, `memo09-phase4` | Keep the master-side bind plus child-side SIGIO rearm model; avoid the rejected child-side rebind experiment that panicked before `MEMBER_DONE`. | `pool-mconsole-path-probe` PASS with member alive, per-member socket present, and rebuilt-kernel `version` reply; focused investigation in `2026-06-10-pool-mconsole-exec-investigation.md`, 2026-06-11. |
| Pool | `umlctl exec` via daemon | Present-validated | `next`, `memo09-phase4` | Keep `exec/1` as the current public ABI: callers send structured argv/env/cwd/timeout to the daemon and receive NDJSON frames. The current mconsole backend deliberately lowers that request through a bounded shell command; `--timeout` requires guest `timeout(1)`. Any stricter kernel transport is future `exec/2` work, not required for the current completion claim. | `pool-exec-smoke` PASS: `/bin/true` exits 0, stdout/stderr capture round-trips, guest exit 7 is preserved without a daemon error, timeout returns code 124 with `timed_out=true`, late stdout is suppressed, no extra guest `sleep` helper leaks, and stale `Unknown command`/missing-host-tool boundaries are rejected; historical `memo09`/`umlctl-deploy` branches had the same outer RPC/NDJSON intent and no stricter kernel argv transport to import, rebuilt `59ad334001ea`, 2026-06-11. |
| Pool | `umlctl port-forward` | Present-validated | `next`, `memo09-phase4` | Keep and later validate against final network mode. | `pool-port-forward-smoke` PASS, rebuilt `59ad334001ea`, 2026-06-11. |
| Pool | Vector2 TAP handoff | Present-validated | `next`, `memo09-phase4`, `umlctl-deploy` | Keep the vector2 TAP reopen path and smoke gate. Per-take pool fd handoff is retired from the current completion claim; current pool takes carry string identity through the identity memfd and reopen vec2 TAP by per-member TAP name. | `vector2-pool-tap-smoke` PASS: per-member TAP/MAC/IPv4 identity visible through daemon exec and one-packet host TAP ping succeeds; source audit confirms `um_template_identity_apply()` calls `um_vec2_tap_reopen_for_pool_member()` before IPv4/route apply; current-head rerun PASS on rebuilt `98166580dc4f`, 2026-06-11. |
| Vector2 | Typed parser | Present-validated | `next` | Keep. | `um_vector2_config` and `um_vector2_transport` KUnit PASS on rebuilt `98166580dc4f`, 2026-06-11. |
| Vector2 | Queue ownership | Present-validated | `next` | Keep. | `um_vector2_queue`, `um_vector2_netdev`, and `um_vector2_ethtool` KUnit PASS on rebuilt `98166580dc4f`, 2026-06-11; lazy-RX queue helper KUnit PASS with `um_vector2_*` 88 pass and 2 trusted-TAP skips; fd/vnet RX allocation helper KUnit PASS with `um_vector2_*` 90 pass and 2 trusted-TAP skips, 2026-06-11. |
| Vector2 | fd backend | Present-validated-needs-long-gates | `next`, `umlctl-deploy` | Keep the launcher-owned inherited-fd path for standalone `umlctl up`; per-take pool fd handoff is not part of the current completion claim. | `vector2-fd-handoff-smoke` PASS on rebuilt `98166580dc4f`; `um_vector2_host_fd` KUnit 13/13 PASS; lazy-RX rerun `vector2-fd-handoff-smoke` PASS and `um_vector2_host_fd` KUnit 14/14 PASS; fd/vnet RX allocation now follows the channel runtime `vnet_hdr` state instead of the transport enum, with `vector2-fd-handoff-smoke` PASS and `um_vector2_*` 90 pass / 2 trusted-TAP skips; historical branch grep found the pool SCM_RIGHTS TAP-fd swap deferred rather than implemented, 2026-06-11. |
| Vector2 | tap backend | Present-validated-needs-long-gates | `next`, `umlctl-deploy` | Keep and run networking/Tier 3 gates. | `vector2-inproc-tap-smoke` and `vector2-pool-tap-smoke` PASS on rebuilt `98166580dc4f`; lazy-RX reruns of both smokes PASS after rebuilding current `umlctl`; bounded KVM-v2/vector2 Tier 3 smoke PASS 2/2 for Django-v2 and FastAPI-v2, but full Tier 3 remains required, 2026-06-11. |
| Vector2 | multiqueue | Present-validated-needs-fairness | `next`, `umlctl-deploy` | Keep the launcher-owned multiqueue fd handoff path; finish fairness/performance gates. | `vector2-fd-multiqueue-smoke` PASS on rebuilt `98166580dc4f`: `umlctl up` creates a multiqueue TAP, opens/inherits fds 200..203, guest metadata reports four queues/fds, `vec2.0` reports four TX queues, and one-packet host TAP ping succeeds; lazy-RX rerun PASS, 2026-06-11. |
| Vector2 | performance benchmark harness | Present-validated-needs-matrix | `next` | Keep `net-bench` as an explicit operator-run gate; expand the remaining P4.3/P4.5 matrix before making replacement claims. | `tools/testing/selftests/um/net-bench` now has a kselftest Makefile, builds `tcp-send`, uses repo-relative defaults instead of developer-local paths, validates wrapper syntax, and captures guest `ip`/route/ethtool diagnostics. Initial TCP run: legacy vector 3/3 PASS median 39805.4 Mbps; vector2 3/3 PASS median 18949.8 Mbps; ratio 0.476 < 0.85. Scatter-gather TX improved vector2 to median 22953.7 Mbps versus legacy 40041.7 Mbps, ratio 0.573. A bounded `sendmmsg()` prototype did not improve the ratio and was not committed. TX/RX NAPI scheduling then fixed the measured guest-to-host regression: `um_vector2_*` KUnit PASS with 87 pass, 0 fail, and 2 trusted-TAP skips, fd handoff/in-process TAP/fd multiqueue smokes PASS, and normal TCP gate PASS with legacy vector median 40080.5 Mbps, vector2 median 37116.0 Mbps, ratio 0.926. Current fixed-byte bidirectional TCP refresh clears guest-to-host at 1/8/32 MiB with ratios 0.966/1.007/0.925 and host-to-guest at 8/32 MiB with ratios 2.002/0.901, but 1 MiB host-to-guest remains below legacy at 0.483 and 0.579 on rerun; a single-queue vector2 rerun reached 0.663. Lazy RX reduces representative 1 MiB host-to-guest RX-slot churn from 32448 prepared / 906 received / 31542 released slots to about 1410 prepared / 905 received / 505 released slots, keeps the normal guest-to-host gate green at ratio 0.993, and improves the focused host-to-guest best ratio only to 0.594. The fd/vnet RX allocation fix closes a framing correctness bug; focused 1 MiB host-to-guest rerun saw vector2 median 0.895 MiB/s versus legacy vector median 0.976 MiB/s, so the small-transfer blocker remains open. The fixed-byte helper now supports UDP; TCP compatibility, vector2 UDP bidirectional smoke, and a paced 1 MiB UDP matrix pass, with vector2 close to legacy on host-side MiB/s. Unpaced/larger UDP still needs publication coverage. |
| Vector2 | raw/gre/l2tpv3/vde/bess/proxy/hybrid transports | Parser-only-retired-from-runtime-claim | `next`, historical vector branches | Do not count these as implemented runtime transports on `next`; keep GRE/L2TPv3 header helpers under KUnit and require new netdev backends plus live smokes before restoring any runtime claim. | Netdev KUnit guards parser-only transports returning `-EOPNOTSUPP`; GRE/L2TPv3 header-helper KUnit remains present; `um_vector2_*` KUnit PASS on rebuilt `98166580dc4f`, 2026-06-11. |
| Vector2 | failed-open fault injection | Present-validated-validation-only | `next`, `umlctl-deploy` | Keep as a validation-only open-unwind knob; leave unset for normal workloads. | `vector2-failed-open` PASS on the current tree: fd-handoff TAP path with `fail_open_after=2`, host gateway ping succeeds, second `ndo_open()` fails by injection, `open_delta=1`, `fail_delta=1`, `close_delta=1`, `vec2.0` is left closed/registered, and the host TAP is cleaned up, 2026-06-10. |
| Vector2 | sandbox mode | Present-validated | `next` | Keep the default-safe audit gate. | `vector2-sandbox-audit` PASS on rebuilt `98166580dc4f`: `umlctl gate loop --audit-vector-sandbox` ran a vector2 auto-queue fd boot and reported `PASS=1/1 FAIL=0 TIMEOUT=0` with no forbidden host operations, 2026-06-11. |
| Vector2 | in-process trusted mode | Present-validated | `next` | Keep explicit and document authority. | `vector2-inproc-tap-smoke` PASS on rebuilt `98166580dc4f`: `umlctl up` reports TAP/inproc mode without launcher fd inheritance, guest metadata reports TAP/inproc with fd count zero, `vec2.0` has the assigned IPv4 address, and one-packet host TAP ping succeeds, 2026-06-11. |
| Vector2 | replacement-readiness claim | Claim-bounded-needs-gates | `next` docs/Kconfig | Keep v2 opt-in and avoid saying legacy vector is superseded until Tier 3, fairness, and long-run gates pass. | Kconfig/help text no longer describes legacy vector as superseded or tells new deployments to prefer v2; vector2 help names TAP/fd as the current netdev runtime scope, 2026-06-10. |
| Launcher | `umlctl up/down/ps/logs` | Present-validated | `next`, `umlctl-deploy` | Keep. | `umlctl-smoke` PASS, 2026-06-10. |
| Launcher | deploy configs | Present-validated | `next`, `umlctl-deploy` | Keep current example/profile set; current `umlctl` uses `up/down` plus Umlfile parsing, not a separate deploy subcommand. No useful historical deploy TOML is missing from `next`. | File-set comparison found no example/profile files unique to `umlctl-deploy`; all 18 example Umlfiles pass `UML_KERNEL=$PWD/linux umlctl up --dry-run`; all 5 built-in profiles resolve with `umlbuild profile show`, 2026-06-10. |
| Launcher | gates and gate-loop | Present-validated | `next`, `umlctl-deploy` | Keep. | `umlctl gate run --dry-run` against `tools/testing/selftests/um/gates/launcher-cargo.toml` PASS, 2026-06-10. |
| Launcher | snapshot export CLI | Present-validated | `next`, `kvm-v2-snapshot-elf64` | Keep mconsole-driven host export path. | live `umlctl snapshot export` smoke PASS, 2026-06-10. |
| Launcher | transparency tooling | Present-validated | `next`, `umlctl-deploy` | Keep. | `run-bpftrace-validate.sh` PASS: all five scripts attached; syscalls and sched produced idle UML data, 2026-06-10. |
| Launcher | `umlbuild` | Present-validated | `next`, `umlctl-deploy` | Keep MVP smoke and the clean-source override path for developer trees with in-tree build products. | `run-umlbuild-mvp.sh` PASS with `UMLBUILD_SOURCE` set to a temporary clean worktree and prebuilt debug `umlbuild`/`umlctl`; direct boot and `umlctl up` both produced the expected guest sha256, 2026-06-10. |
| Syzkaller | UML VM shim | Present-validated | `next`, `umlctl-deploy` | Keep the shim on request-specific lazy `pool take` plus the validated `umlctl` JSON contracts, including `exec/1` NDJSON frames. Future `exec/2` changes require a schema bump and shim update. | `syzkaller-shim-smoke` PASS: source contract check plus syzkaller-style take, exec output merge, port-forward, status, destroy, rebuilt `59ad334001ea`, 2026-06-11. |
| Profiles | profile configs | Present-needs-runtime-builds | `next`, historical docs | Keep the 10 kernel Kconfig profiles and the 5 `umlbuild` profiles; full per-profile kernel builds and runtime feature probes remain required before final completion. | Clean-worktree `make ARCH=um O=<out> uml/<profile>` config matrix PASS for all 10 kernel profiles, `research-kmsan` now documented, listed in arch help, and covered by the runtime profile harness when its LLVM-built binary is present; all 5 `umlbuild` profiles resolve with `umlbuild profile show`, 2026-06-10. |
| Instrumentation | kprobes | Present-validated | `next` | Keep kprobes/kretprobes in the research profile and the sample-module stress gate. | Clean temporary worktree `uml/research` confirms `CONFIG_KPROBES=y`, `CONFIG_KRETPROBES=y`, `CONFIG_SAMPLE_KPROBES=m`, `CONFIG_SAMPLE_KRETPROBES=m`, and `CONFIG_KPROBES_SANITY_TEST=y`; `make ARCH=um O=<out> linux samples/kprobes/kretprobe_example.ko` PASS; `kprobes-stress` PASS with `iters=1000`, `fires=1004`, `errors=0`, and `graph=on`, 2026-06-10. |
| Instrumentation | ftrace | Present-validated | `next` | Keep normal dynamic function tracing; do not advertise function-graph tracing until the UML return-stack interaction is fixed. | Clean-worktree ftrace build with `FUNCTION_TRACER=y` and no `FUNCTION_GRAPH_TRACER` passed `ftrace-smoke` with 51,300 trace lines; `um/ftrace-smoke` now fails fast if a binary advertises unsupported `function_graph`, 2026-06-10. |
| Instrumentation | KMSAN | Present-validated | `next` | Keep `research-kmsan`; rerun the KMSAN smoke in the final validation matrix. | Clean LLVM `research-kmsan` build PASS with `CONFIG_CC_IS_CLANG=y`, `CONFIG_FORTIFY_SOURCE=y`, `CONFIG_KMSAN=y`, and `CONFIG_KMSAN_CHECK_PARAM_RETVAL=y`; `kmsan-smoke` PASS with `KMSAN_SMOKE: PASS runtime=y reproducer=n`; stackless seccomp `rt_sigreturn` restorer verified by object inspection, 2026-06-11. |
| Instrumentation | KASAN | Present-validated | `next`, `research` profile | Keep the research-profile `cve-repro` KASAN gate and its loadable `kasan_test.ko` module path. | Clean temporary worktree `make ARCH=um O=<out> uml/research` plus `make ARCH=um O=<out> linux mm/kasan/kasan_test.ko` PASS; `cve-repro` PASS with `ok=10`, `not_ok=0`, `kasan_bugs=13`, `guest_wall_s=0`, and `host_wall=2.71s`, 2026-06-10. |
| Instrumentation | KFENCE | Present-validated | `next`, `research` and `fuzz-deep` profiles | Keep the profile config and the focused KFENCE report smoke. | Clean temporary research build confirms `CONFIG_KFENCE=y`, `CONFIG_KFENCE_KUNIT_TEST=m`, `CONFIG_KFENCE_SAMPLE_INTERVAL=100`, and `mm/kfence/kfence_test.ko` builds; `kfence-smoke` PASS with `bugs=1`, `stats_bugs=1`, `ok=1`, `not_ok=0`; runtime profile probe reports `PASS research (8 features match)`, 2026-06-10. |
| Instrumentation | KCSAN | Present-validated | `next`, `race` profile | Keep the race profile and the focused KCSAN debugfs/selftest smoke. | Clean temporary race build confirms `CONFIG_HAVE_ARCH_KCSAN=y`, `CONFIG_KCSAN=y`, `CONFIG_KCSAN_SELFTEST=y`, `CONFIG_SMP=y`, `CONFIG_DEBUG_FS=y`, and `CONFIG_FTRACE=y`; runtime profile probe reports `PASS race (5 features match)`; `kcsan-smoke` PASS with `selftest=1`, `initial=0`, `enabled=1`, `disabled=0`, `microbench_begin=1`, `microbench_end=1`, and `unexpected=0`, 2026-06-10. |
| Instrumentation | KCOV | Present-validated | `next`, `fuzz` and `fuzz-deep` profiles | Keep KCOV isolated to fuzzing profiles and guard it with the focused `kcov-smoke` runtime gate. | Clean temporary worktree `make ARCH=um O=<out> uml/fuzz` confirms `CONFIG_KCOV=y`, `CONFIG_KCOV_ENABLE_COMPARISONS=y`, and `CONFIG_KCOV_INSTRUMENT_ALL=y`; `tools/testing/selftests/um/kcov-smoke/` PASS against a fresh fuzz-profile UML binary with `mode=pc`, `entries=4094`, and `first_pc=0x605daf39`; runtime profile probe reports `PASS fuzz (6 features match)`, 2026-06-10. |
| Instrumentation | BPF/JIT shims | Present-validated | `next` | Keep research-profile BPF/JIT support and the focused runtime smoke. | Clean temporary worktree `make ARCH=um O=<out> uml/research` confirms `CONFIG_HAVE_EBPF_JIT=y`, `CONFIG_BPF_SYSCALL=y`, `CONFIG_BPF_JIT=y`, and `CONFIG_BPF_JIT_ALWAYS_ON=y`; `make ARCH=um O=<out> arch/x86/net/bpf_jit_comp.o` PASS; new `bpf-jit-smoke` PASS against a fresh research-profile UML binary with `bpf_jit_enable=1`, `xlated_len=16`, and `jited_len=16`, 2026-06-10. |
| Instrumentation | KGDB | Deferred-not-present | original plan/docs | Current UML does not select `HAVE_ARCH_KGDB`, the live profile fragments do not enable `CONFIG_KGDB`, and active profile docs now describe KGDB as deferred rather than present. Implementing KGDB still requires UML architecture support, backend register read/write integration, a transport choice, and a smoke test before it can re-enter the completion claim. | Source audit: no `select HAVE_ARCH_KGDB` in `arch/um/Kconfig*`, no `CONFIG_KGDB=y` in `arch/um/configs/profiles/`, 2026-06-11. |
| Selftests | Upstreamable smoke/regression tests | Partial-cleaned | all branches | Continue curation; active gate metadata and tier smoke fixtures now use semantic group names and neutral test data instead of planning phase labels. Active source and selftest comments also no longer carry the found internal audit label or temporary-policy wording. | Focused scan over `arch/um`, `tools/testing/selftests/um`, and `tools/uml/uml-launcher` found no standalone workstream/decision/memo/phase labels after cleanup; residual matches are operational init/workload phases, sample BUG log text, proc/stat field numbers, and BPF filter syntax, 2026-06-10. |
| Selftests | local stress/soak tests | Present | `next`, `umlctl-deploy` | Keep under clear local/soak docs. | selected soak gates. |
| Selftests | historical repros | Present/needs-cleanup | all branches | Move or mark archival. KVM FPU isolation reproducers are now explicitly diagnostic-only and outside the UML kselftest pass/fail gate. | Continue archival grep for diary/internal labels in broader historical repro material. |
| Docs | `STATUS.md` | Present-current | `next` | Update after every landed workstream. | status review updated with launcher/selftest curation boundary, 2026-06-10. |
| Docs | old reports/presentations | Archived-historical | `next`, historical docs | Keep the May 2026 report/deck workspace as explicitly historical unless a future report refresh freezes a new `next` cutoff and regenerates the data, TeX, and PDFs from the live inventory. | `report-presentation/README.md` and `PLAN.md` mark the workspace archival; shared TeX/report/slides/comprehensive sources carry archive notices; `status_kpis.csv` is marked historical; tracked report, slides, and comprehensive PDFs rebuild with `make all`, 2026-06-10. |
| Docs | snapshot ELF docs | Present-validated | `next`, `kvm-v2-snapshot-elf64` | Keep `snapshot-elf-format.rst` and `debugfs.rst` matched to the current boot, mconsole/`umlctl`, debugfs, and in-kernel exporter surfaces. | Trigger/symbol audit confirms `kvm_v2_snapshot_elf_export=`, mconsole `snapshot_export`, debugfs `kvm_v2_snapshot_elf_export_path`, debugfs `kvm_v2_snapshot_bench`, `kvm_v2_snapshot_elf_export_to_file()`, `kvm_v2_snapshot_elf_export_to_fd()`, and `snapshot-elf-roundtrip`; docs updated and `umlctl` help corrected, 2026-06-10. |
| Docs | active UML user docs | Present-cleaned | `next` | Keep non-redesign UML docs focused on current interfaces and limitations, not workstream history. | Normalized ftrace, kprobes, KMSAN, debugfs, section-split, snapshot/forkserver, launcher, APERF/MPERF, and profile docs; focused scan over non-redesign `Documentation/virt/uml/*.rst` now has no standalone workstream/decision/memo/phase-history labels apart from an OpenWrt sample `#0` line, 2026-06-10. |
| Docs | vector2 validation docs | Present-current | `next` | Keep the validation-gates memo as the current vector2 publication tracker and keep older sprint/default-flip notes marked historical when they conflict with current `next`. | `49-uml-vector-driver-v2-validation-gates-2026-05-17.md` now records the June 10 gate state; the post-May-19 sprint README no longer claims current default-flip completion; source comment cleanup removed stale branch-default wording, 2026-06-10. |
| Upstream | patch queue | Present-stale | `next` docs | Regenerate from final `next`. | checkpatch and cover letters. |

## Closed In Current Update

- KVM v2 no longer ignores failed architectural-state restore ioctls for
  `KVM_SET_MSRS`, `KVM_SET_XSAVE`, or `KVM_SET_VCPU_EVENTS`.
- KVM v2 experimental record/replay core is present on `next` behind
  `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL`. It includes the
  single-active container, lifecycle, strict replay flag, bounded syscall
  observe, FIFO consume, divergence cursor preservation, payload restore for
  the first supported copyout syscalls, direct-syscall and current
  UML-vDSO-wrapper raw-time payload replay, strict replay syscall-policy
  coverage, strict-on/strict-off gate behavior, strict replay failure
  accounting, and overflow accounting, plus LSTAR gadget-bypass plumbing,
  snapshot-backed debugfs session start, time-travel clock event replay, and
  an experimental versioned event format and singleton control/status surface
  for live dispatcher logging. Validation:
  `make ARCH=um -j$(nproc)`, `um_kvm_v2_record` 24/24, `kvm_v2_marshal` 9/9,
  `kvm_v2_byteshape` 9/9, `kvm-record-smoke` live snapshot-backed record PASS
  with `KVM_RECORD_TIME: PASS syscall_clock=... vdso_clock=...`, and
  `kvm-record-clock-bench` PASS on 2026-06-11.
- KVM v2 dynamic-loader/TLS startup now passes the focused gates: forced-KVM
  `/bin/true` reaches clean init exit with `exitcode=0`, and the dyn-loader
  kselftest reports `DYN_LOADER: backend=kvm PASS`.
- CPython tier-0 now passes through the current `umlctl gate run` path on the
  same `./linux` binary under both seccomp and KVM v2. Both backends report
  `pass=1 fail=0 expected_fail=0`, and both extract the same hashlib
  empty-string SHA-256 metric.
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
- The focused pool/fork/syzkaller regression set passes on the rebuilt
  current-HEAD `59ad334001ea` UML binary: template-pause, fork smoke/stress,
  pool member, replicated sustained pool, pivot, spawn, serve, exec,
  port-forward, per-member mconsole, pool benchmark, and syzkaller shim. This
  closes the immediate current-HEAD audit but must be repeated after later
  KVM v2 or vector2 changes.
- The current-head vector2 validation refresh passes on rebuilt
  `98166580dc4f`: `um_vector2_*` KUnit reports 84 pass, 0 fail, 2
  trusted-TAP skips; fd handoff, fd multiqueue, in-process TAP, sandbox audit,
  and pool TAP smokes pass; bounded KVM-v2/vector2 Tier 3 smoke passes 2/2
  for Django-v2 and FastAPI-v2. Full Tier 3, 7200-second seccomp, and
  fairness/performance gates remain open.
- Vector2 RX checksum feature reporting now matches the vnet-header receive
  path: when `csum=1`, vector2 exposes fixed RX checksum support like legacy
  vector's TAP/vnet-header path. Validation: `make ARCH=um -j$(nproc)`,
  `um_vector2_*` KUnit 89 pass, 0 fail, 2 trusted-TAP skips; fd handoff,
  fd multiqueue, and in-process TAP smokes PASS; fixed-byte diagnostics show
  vector2 `rx-checksumming: on [fixed]` with `vnet_hdr_enabled: 1`; and the
  normal guest-to-host TCP gate remains green with vector2/legacy ratio 0.990,
  2026-06-11. The 1 MiB host-to-guest fixed-byte gap remains open.
- Vector2 fd/vnet RX allocation now follows the channel runtime `vnet_hdr`
  state rather than the configured transport enum.  This keeps inherited-fd
  TAP channels with `IFF_VNET_HDR` sized for the virtio header. Validation:
  `git diff --check`, `make ARCH=um -j$(nproc)`, `um_vector2_*` KUnit 90 pass,
  0 fail, 2 trusted-TAP skips, and fd handoff, fd multiqueue, and in-process
  TAP smokes PASS, 2026-06-11. A focused 1 MiB host-to-guest diagnostic showed
  vector2 median 0.895 MiB/s versus legacy vector median 0.976 MiB/s, so this
  closes a correctness bug but not the small-transfer publication blocker.
- Vector2 UDP fixed-byte evidence is no longer missing. The performance helper
  accepts `UML_VECTOR_PERF_PROTOCOL=tcp|udp`, keeps TCP as default, records a
  `protocol` column, and supports paced UDP diagnostics. Validation:
  TCP compatibility smoke PASS, vector2 UDP bidirectional 64 KiB smoke PASS,
  and paced 1 MiB UDP matrix PASS with host-side MiB/s of vector 7.471
  guest-to-host / 8.760 host-to-guest and vector2 7.319 guest-to-host / 8.660
  host-to-guest, 2026-06-11. Unpaced legacy host-to-guest at 1 MiB did not
  reach exact-byte completion, so UDP remains initial evidence rather than a
  final publication gate.
- Vector2 fixed-byte endpoint CPU timing is now captured by the same helper.
  Guest `VECTOR_NET_PERF` lines and host `HOST_SINK`/`HOST_SEND` lines include
  `cpu_seconds=...`, and `summary.tsv` appends `guest_cpu_seconds` and
  `host_cpu_seconds`. Validation: `bash -n`, `git diff --check`, vector2 TCP
  64 KiB guest-to-host smoke PASS, and paced vector2 UDP 64 KiB bidirectional
  smoke PASS, 2026-06-11. This is helper-level per-process timing, not the
  final full-system CPU-utilisation or syscall-rate gate.
- Vector2 host-to-guest UML process metric deltas are now captured by the
  fixed-byte helper through `umlctl metrics --json` before and after the
  transfer window.  `summary.tsv` appends user/system CPU seconds, scheduler
  run/wait seconds, scheduler pcount, voluntary/involuntary context-switch
  deltas, and before/after metrics JSON paths. Validation: `bash -n`,
  `git diff --check`, vector2 TCP 64 KiB host-to-guest smoke PASS with real
  UML metric deltas, side-by-side legacy vector/vector2 TCP 64 KiB
  host-to-guest smoke PASS with 22-field summary rows for both drivers, and
  vector2 TCP 64 KiB guest-to-host shape smoke PASS with `NA` metric fields,
  2026-06-11. Local privileged syscall-rate
  collection remains blocked by `perf_event_paranoid=4`, so this is bottleneck
  diagnostic support rather than P4.3 syscall-rate closure.
- The first focused 1 MiB host-to-guest run with UML process metrics confirms
  the small-transfer cell remains open. Four repeats each for legacy vector and
  vector2 passed. Host-side MiB/s medians were legacy vector 0.9845 and vector2
  0.9005, ratio 0.9147; best observed host-side rates were legacy vector 1.630
  and vector2 1.085, ratio 0.6656. Median UML process scheduler pcount was
  26547.5 for vector2 versus 2213.0 for legacy vector, and median voluntary
  context switches were 26505.5 versus 2186.5, pointing the next bottleneck
  pass at wakeup/readiness/receive scheduling rather than raw RX-slot
  allocation alone.
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
- Snapshot ELF documentation now matches the current producer surfaces:
  `kvm_v2_snapshot_elf_export=`, mconsole `snapshot_export`, direct debugfs
  `kvm_v2_snapshot_elf_export_path`, debugfs `kvm_v2_snapshot_bench`, and the
  in-kernel ELF export helpers.
- UML KMSAN runtime closure is fixed on `next`: the page-aligned vmalloc
  metadata layout builds cleanly with LLVM, UML restores Clang's KMSAN memory
  intrinsic lowering under `CONFIG_KMSAN`, non-instrumented UML host helpers
  clear KMSAN call metadata before kernel formatting, hostfs unpoisons data
  filled by host syscalls, the KMSAN direct-map validity hook rejects host-side
  mappings, and the seccomp stub signal restorer is stackless under
  frame-pointer/KMSAN builds. `kmsan-smoke` now reaches
  `KMSAN_SMOKE: PASS runtime=y reproducer=n` on a `research-kmsan` binary.

## Remaining Hard Blockers

These items must be closed before the final branch can be called complete:

1. Record/replay runtime functionality must be completed beyond the current
   experimental core, snapshot-backed start, and live record smoke before the
   original record/replay mission is closed.
2. Vector2 replacement claims must match validation evidence. Current-head
   focused smokes, KUnit, bounded KVM-v2/vector2 Tier 3 path smoke, the
   normal guest-to-host TCP gate, lazy-RX allocation-churn cleanup, RX checksum
   feature alignment, and most fixed-byte bidirectional TCP cells pass, but
   the host-to-guest 1 MiB regression, natural 7200-second seccomp run, full
   KVM-v2 Tier 3 coverage, broader UDP plus syscall-rate/full CPU-utilisation
   performance coverage beyond the current endpoint and UML process deltas,
   and multiqueue fairness/performance gates remain open.
3. Pool/fork-server current tests pass on rebuilt current HEAD, including
   warm-pool, pool-member, replicated sustained-pool, pool-bench, and
   syzkaller paths. This remains a final-completion blocker only as a required
   rerun after later KVM v2/vector2 changes, plus the still-open
   snapshot-backed fork-server disposition. Vector2 pool-member TAP and
   launcher-owned fd handoff now have dedicated smoke gates; per-take pool fd
   handoff has been retired from the current completion claim in favor of the
   validated TAP reopen path.
4. Focused scans over active source, launcher, selftests, and non-redesign UML
   docs are clean for the targeted standalone planning-label patterns as of
   2026-06-10, with residual matches limited to operational wording and test
   fixtures. The old report/deck workspace is now explicitly archived as
   historical May 2026 material.
5. The final validation matrix from the integration plan must pass.
6. KMSAN is no longer a hard blocker after the 2026-06-11 runtime-smoke pass;
   rerun `kmsan-smoke` in the final validation matrix to protect the closure.

## Next Update Rules

When a workstream lands:

1. Change affected rows from `Open`/`Partial`/`Historical-only` to the new
   status.
2. Add the landed commit hash.
3. Add the validation command and result.
4. Update `STATUS.md` if the user-visible project status changed.
5. Leave historical branch references intact until the feature is either fully
   imported or explicitly retired.
