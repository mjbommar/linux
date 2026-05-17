# UML vector driver v2 completion audit

**Status:** audit - not complete.
**Date:** 2026-05-17.

This audit maps the vector v2 buildout objective to concrete evidence
in the tree.  It is intentionally conservative: uncertainty is treated
as incomplete.

## Objective

Complete building, testing, and documenting the UML vector networking
v2 driver described by `15-uml-vector-driver-v2-buildout.md`.

Completion means vector v2 is eligible to replace the legacy
`CONFIG_UML_NET_VECTOR` implementation, not merely that an
experimental netdev exists.

## Prompt-To-Artifact Checklist

| Requirement | Evidence | Status |
| --- | --- | --- |
| Build live netdev driver under `CONFIG_UML_NET_VECTOR_V2=y` | `vector2_cmdline.c`, `vector2_core.c`, `vector2_netdev.c`, `vector2_host_fd.c`, `vector2_host_tap.c`, `vector2_runtime.c`; runtime UML builds passed for trusted and sandbox configs | Partial: experimental only |
| Register stable v2 netdev names | `vec2.<unit>` registration through `register_netdevice()`; runtime logs show `registered netdev vec2.0` | Done for v2 syntax |
| `ip link set up/down` reaches modeled lifecycle | KUnit lifecycle tests; fd/TAP netdev open/stop tests; fd KUnit repeats netdev open/stop 1000 times; manual TAP/fd smokes; live vector2 lifecycle gate repeats `ip link down/up` 10,000 times and verifies ethtool open/close deltas; live `fail_open_after=2` gate drives a failed second `ip link set up` through `ndo_open()` and verifies closed registered state | Done for current fd lifecycle/failure path; longer soaks still tracked separately |
| Single-queue trusted TAP packet path | R5 TAP datapath doc; ping smokes; Tier 3 seccomp 30/30 | Done for trusted TAP/seccomp |
| Single-queue and multiqueue fd transport with launcher-supplied fds | fd datapath exists over inherited fds; sandbox accepts inherited `fd=`; manual no-root fd ping smokes; `umlctl` records manifest labels and passes vector2 TAP fds as inherited fd ranges starting at 200; live single-queue and 4-queue `umlctl up` fd-handoff smokes passed; `queues = "auto"` / `--network-queues auto` resolve to numeric vector2 fd ranges from `[runtime].ncpus` | Done for launch path; deeper SMP/perf still open |
| TX/RX move through v2 queues, not legacy queues | `vector2_queue` rings/batches used by fd and TAP; KUnit TX/RX tests | Done for implemented fd/TAP paths |
| Tier 3 Django/FastAPI on seccomp and kvm-v2 | Django stdlib shim passes seccomp 30/30; real FastAPI + uvicorn vector2 fd smoke passes seccomp 30/30 with `FASTAPI_HTTP ok=51 fail=0` each run; a correctly configured KVM-v2 vector2 runtime now passes no-network readiness, vector2 fd-handoff, and a one-shot Django smoke; one Django KVM-v2 30-run attempt was `PASS=29/30 FAIL=1`, a diagnostic rerun passed `PASS=30/30 FAIL=0`, a longer diagnostic sample reached only `PASS=57/60 FAIL=2 TIMEOUT=1`, a trace-enabled rerun reached `PASS=59/60 FAIL=1 TIMEOUT=0`, and a post-syscall shared-run hardening rerun still reached only `PASS=29/30 FAIL=1 TIMEOUT=0` while proving the original post-syscall mismatch is gone | Partial: kvm-v2 Tier 3 remains unstable; hours-long workload soak remains open |
| KUnit coverage for config, lifecycle, queue, fake host, transport, host-open failure, unwind, queue policy | `um_vector2_*` KUnit passes 76/76 after fd wrong-type, fd multiqueue unwind, queue-to-CPU policy coverage, fd open/stop repeat stress, bad-fd unwind, missing-config failure stress, and injected failed-open coverage | Done for current implemented surfaces; more tests needed as new transports/features land |
| ethtool stats and ring queries stopped/running | R6/R8b docs; KUnit ethtool tests; live queue stats | Done for current surfaces |
| Sandbox blocks host helper/TAP/raw/BPF creation | parser rejects trusted host options without `INPROC`; TAP sandbox KUnit; inherited fd allowed by policy; `umlctl` fd handoff keeps TAP opening in the launcher; a traced vector2 auto-queue fd boot found no actual `/dev/net/tun` open, `TUNSETIFF`, `AF_PACKET`, `bpf()`, or UML network-helper exec in the vector host path; `umlctl gate loop --audit-vector-sandbox` now preserves per-iteration strace/audit logs and fails on those forbidden vector host operations | Partial: local gate exists and one audited boot passed, but CI/long workload coverage and guest userspace raw/netlink socket policy remain open |
| Multiqueue TAP/fd KCSAN and distribution | TAP multiqueue works and queue counters move; fd multiqueue core opens contiguous inherited fd ranges under KUnit; `umlctl` fd multiqueue passes live 4-queue smoke and 3/3 gate loop; vector2 has explicit `ndo_select_queue` plus XPS queue-to-CPU policy; repeated KCSAN auto-queue fd multiqueue smoke found and fixed a queue-lock bottom-half lockdep warning, then passed `PASS=10/10` with no warning, KCSAN, or data-race signatures; one KCSAN FastAPI vector2 fd workload passed; concurrent TCP/UDP KCSAN traffic has an initial pass plus three default repeats with all four TX and RX queues moving; varied KCSAN profiles now include fixed two-queue/two-vCPU/six-flow evidence and a paced eight-flow/four-queue larger-volume pass | Partial: longer fairness profiles, additional host/kernel coverage, and kvm-v2 reruns still open |
| Performance parity or accepted regression | Initial `umlctl` bidirectional TCP baseline exists; repeated legacy-vs-vector2 TCP sweep now covers both directions, 1 MiB/8 MiB/32 MiB, two repeats per cell; vector2 remains slower guest-to-host and much faster host-to-guest on this host; UDP, syscall, CPU, and broader host/kernel profiles remain absent | Partial: mixed results measured, not accepted |
| Legacy `vecN:` compatibility transition | Legacy remains production path; no v2 compatibility switch | Open |
| Reviewable, bisectable patch series | Work is split across pushed commits and checkpoint docs | Ongoing |

## Evidence Snapshot

Recent pushed checkpoints:

- `9f68307c28e4` - `umlctl` vector2 usability and diagnostics.
- `9b5d0dcdafc0` - kvm-v2 readiness isolation.
- `844cd050930e` - trusted TAP multiqueue shape.
- `a8666760f21c` - per-queue ethtool stats.
- `1e3e23f63e13` - TAP queue distribution smoke evidence.
- `63c5ddecf0d3` - direct-fd packet datapath.
- `f7d3c9fcf2eb` - inherited fd allowed in sandbox builds.
- `39ff4cc55aae` - seccomp Tier 3 Django 30/30 evidence.
- post-audit follow-up - `umlctl` vector2 single-queue TAP fd handoff.
- post-audit follow-up - vector2 fd multiqueue and queue-to-CPU
  policy.
- post-audit follow-up - fd failure-stress KUnit.
- post-audit follow-up - live vector2 lifecycle stress harness.
- post-audit follow-up - KCSAN concurrent TCP/UDP traffic harness.
- post-audit follow-up - FastAPI/uvicorn vector2 fd 30/30 repetition.
- post-audit follow-up - live vector2 failed-open injection proof.
- post-audit follow-up - KVM-v2 vector2 readiness narrowed to Django
  workload instability.
- post-audit follow-up - backend-filtered vector2 seccomp Tier 3
  soak pilot for Django and FastAPI.
- post-audit follow-up - requested-stop vector2 seccomp Tier 3
  long-soak evidence.

Current status update against the short remaining list:

| Item | Current Status | Notes |
| --- | --- | --- |
| kvm-v2 baseline readiness blocker | Partial / narrowed | No-network readiness, vector2 fd-handoff, one-shot Django vector2 smoke, and a no-network Python import control now pass on a correctly configured KVM-v2 runtime, but a no-network Django-loopback control fails. The remaining KVM-v2 issue is a guest userspace execution flake under the Django/server/socket workload shape, not vector2 fd/TAP setup or generic boot readiness. |
| kvm-v2 + vector2 Tier 3 30/30 | Partial / not accepted | One diagnostic Django vector2 KVM-v2 run reached `PASS=30/30 FAIL=0 TIMEOUT=0`, but longer or trace-enabled samples still failed at `PASS=57/60 FAIL=2 TIMEOUT=1`, `PASS=59/60 FAIL=1 TIMEOUT=0`, and later `PASS=29/30 FAIL=1 TIMEOUT=0`. This remains open until the backend flake is fixed or bounded with clean repeat evidence. |
| FastAPI variant | Done for seccomp vector2 fd | Real FastAPI + uvicorn vector2 fd handoff passed smoke and `PASS=30/30 FAIL=0 TIMEOUT=0`, with every run reaching `FASTAPI_HTTP ok=51 fail=0`. |
| fd multiqueue | Done for current launch/core path | Core KUnit, live `umlctl` fd multiqueue, auto queue sizing, per-queue stats, and clean TAP teardown evidence are recorded. |
| queue-to-CPU policy | Done for implementation and unit coverage | vector2 has explicit `ndo_select_queue`, XPS setup, and KUnit coverage. Longer SMP fairness validation remains part of the broader performance/KCSAN work. |
| KCSAN on multiqueue | Partial / materially progressed | No longer a plain open item: auto-queue fd multiqueue passed `PASS=10/10`, FastAPI passed once under KCSAN, concurrent TCP/UDP traffic passed with all four TX/RX queues moving, and varied KCSAN profiles include two-queue/six-flow and paced eight-flow/four-queue evidence. Longer fairness matrices, additional host/kernel coverage, and kvm-v2 reruns remain open. |
| legacy-vs-v2 perf baseline | Partial / measured, not accepted | Repeated TCP sweeps now cover both directions, 1 MiB/8 MiB/32 MiB, and two repeats per cell. Results are mixed: vector2 is slower guest-to-host and much faster host-to-guest on this host. UDP, syscall, CPU, and acceptance analysis remain open. |
| long-soak proof | Partial / stopped-clean seccomp run | Backend-filtered vector2 seccomp Tier 3 evidence now includes the 1266-second 200/200 pilot and a requested-stop 6142-second run with 970/970 passes: Django-v2 490/490, FastAPI-v2 480/480, all rows using vector2 `vec2.0` TAP/inproc with one queue, all per-run logs reaching `SERVER_READY`, `GUEST_CURL ok=100 fail=0`, and `TIER3_OK`, no hidden fatal/BUG/KCSAN signatures, and clean TAP/process teardown. This is strong long-run evidence, but it does not close the accepted 7200-second gate because the run stopped at 85.3% of budget. |
| sandbox strace/audit gate | Done locally / broader rollout open | `umlctl gate loop --audit-vector-sandbox` preserves per-iteration strace/audit logs and fails on forbidden vector host operations; one audited vector2 auto-queue fd boot passed. CI/preflight wiring, longer workload coverage, and guest raw/netlink policy remain open. |

Validation evidence recorded in the checkpoint docs includes:

- targeted vector2 object builds;
- full trusted runtime UML build;
- full sandbox runtime UML build;
- `um_vector2_*` KUnit: 75/75 passed after fd failure-stress coverage,
  then 76/76 passed after the live failed-open injection work;
- no-root fd ping over inherited UNIX datagram fd;
- sandbox-only fd ping over inherited UNIX datagram fd;
- fd open diagnostics for missing and wrong-type inherited fds;
- fd failure-stress KUnit: 1000 repeated netdev open/stop cycles,
  bad-fd `ndo_open()` unwind to closed state, 10,000 direct missing-fd
  backend failures, `um_vector2_host_fd` 11/11 at that checkpoint, and
  no KUnit log warning/error signatures;
- vector2 lifecycle stress harness:
  `tools/uml/uml-launcher/examples/vector2-lifecycle-stress.toml`
  renders vector2 fd handoff over fd 200; a 25-cycle live smoke passed,
  then the default 10,000-cycle run passed with
  `open_delta=10000 close_delta=10000`, post-loop gateway ping 3/3,
  `VECTOR2_LIFECYCLE_STRESS_OK`, no warning/BUG/KCSAN signatures in the
  copied run log, no lingering `v2life0`, and no UML process leak;
- vector2 failed-open injection proof:
  `tools/uml/uml-launcher/examples/vector2-failed-open.toml` renders
  vector2 fd handoff with `fail_open_after=2`; `um_vector2_*` KUnit
  passes 76/76; the live gate passes `PASS=1/1 FAIL=0 TIMEOUT=0` after
  the second `ip link set up` fails through `ndo_open()` with
  `open_delta=1 fail_delta=1 close_delta=1`, state `RUNNING` to
  `REGISTERED`, `VECTOR2_FAILED_OPEN_OK`, no warning/BUG/KCSAN
  signatures in the runtime log, no lingering `v2failopen0`, and no UML
  process leak;
- fd multiqueue core KUnit for contiguous inherited fd ranges and
  missing-later-fd unwind;
- live `umlctl up` vector2 fd handoff over TAP fd 200, with
  `FD_HANDOFF_OK` and clean TAP teardown;
- live `umlctl up` vector2 fd multiqueue handoff over TAP fds
  200..203, with `requested_queues=4 runtime_queues=4`,
  `numtxqueues 4`, 3/3 ping, per-queue ethtool counters, and clean
  TAP teardown;
- `umlctl` auto queue sizing dry-run evidence: `queues = "auto"`
  with `ncpus = 4` resolves to `queues=4 queue_spec=auto`,
  `vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4`, and
  inherited fds `200..203`;
- short KCSAN-instrumented vector2 auto-queue fd multiqueue gate:
  `PASS=1/1 FAIL=0 TIMEOUT=0`, no lingering `v2autoq0`,
  `requested_queues=4 runtime_queues=4`, `UMLCTL_NETWORK_QUEUE_SPEC=auto`,
  and no `BUG: KCSAN` / `data-race` signatures in the run log;
- repeated KCSAN vector2 auto-queue fd multiqueue gate after the
  queue-lock bottom-half fix: `PASS=10/10 FAIL=0 TIMEOUT=0`, no
  lingering `v2autoq0`, `um_vector2_*` KUnit 72/72, and no
  `WARNING`, `BUG`, `KCSAN`, `data-race`, panic, or lockdep signatures
  in the captured run logs;
- KCSAN FastAPI vector2 fd workload: `PASS=1/1 FAIL=0 TIMEOUT=0`,
  `SERVER_READY`, `FASTAPI_HTTP ok=51 fail=0`,
  `VECTOR2_FASTAPI_OK`, `REPRO_DONE rc=0`, no warning/BUG/KCSAN
  signatures in the copied run log, no lingering `v2fastapi0`, and no
  UML process leak;
- KCSAN concurrent vector2 fd multiqueue traffic:
  `tools/uml/uml-launcher/scripts/vector2-kcsan-concurrent-traffic.sh`
  over KCSAN kernel
  `/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux`;
  guest-to-host TCP received 4,194,304 bytes, guest-to-host UDP received
  1024 unique packets / 524,288 bytes, host-to-guest TCP delivered
  4,194,304 bytes, host-to-guest UDP delivered 1024 packets /
  524,288 bytes, all four TX queues moved with values
  `1815,465,1799,1589`, all four RX queues moved with values
  `3040,726,1183,1788`, `VECTOR2_KCSAN_TRAFFIC_OK`, no
  warning/BUG/KCSAN signatures in the captured logs, no lingering
  `v2kcstraffic0`, and no UML process leak; three additional default
  repeats passed with all four TX/RX queues moving, no warning/BUG/KCSAN
  signatures, no lingering TAP, and no UML process leak;
- varied KCSAN concurrent traffic profiles: the helper now accepts
  `UML_VECTOR2_KCSAN_NCPUS` and `UML_VECTOR2_KCSAN_QUEUES=N|auto`;
  a fixed two-vCPU/two-queue/six-flow run passed with exact
  bidirectional TCP/UDP accounting, TX queue values `1792,1530`, RX
  queue values `3170,1562`, clean teardown, and no warning/BUG/KCSAN
  signatures; a paced four-vCPU/auto-queue/eight-flow heavier run
  passed with 16,777,216 TCP bytes and 2048 UDP packets in each
  direction, all four TX/RX queues moving, clean teardown, and no
  warning/BUG/KCSAN signatures; an unpaced eight-flow/4096-UDP-packet
  attempt failed on host-to-guest UDP receive accounting without KCSAN
  or kernel warning signatures, defining a KCSAN workload pacing bound;
- short `umlctl gate loop` vector2 fd handoff repetition:
  `PASS=3/3 FAIL=0 TIMEOUT=0` and no lingering `v2fd0`;
- short `umlctl gate loop` vector2 fd multiqueue repetition:
  `PASS=3/3 FAIL=0 TIMEOUT=0`, no lingering `v2fdmq0`, and no
  cleanup failure logs;
- `umlctl gate loop` TAP cleanup audit marks an iteration failed if
  the declared TAP remains under `/sys/class/net` after teardown;
- vector2 TAP seccomp Tier 3 Django stdlib shim: 30/30 passed;
- vector2 fd seccomp FastAPI + uvicorn: one-shot `PASS=1/1`, short
  repetition `PASS=10/10 FAIL=0 TIMEOUT=0`, and longer repetition
  `PASS=30/30 FAIL=0 TIMEOUT=0`; every run had
  gateway ping over `vec2.0`, `SERVER_READY`,
  `FASTAPI_HTTP ok=51 fail=0`, `VECTOR2_FASTAPI_OK`,
  `REPRO_DONE rc=0`, and no lingering `v2fastapi0`; the 30/30 copied
  run logs had no warning/BUG/panic/KCSAN/data-race/failure signatures
  and no running UML instance after teardown;
- vector2 fd seccomp FastAPI + uvicorn with sandbox audit:
  `PASS=1/1 FAIL=0 TIMEOUT=0`, `SERVER_READY`,
  `FASTAPI_HTTP ok=51 fail=0`, `VECTOR2_FASTAPI_OK`,
  `REPRO_DONE rc=0`, no lingering `v2fastapi0`, no UML/strace process
  leak, and a 2068786-line strace with no host TAP open, `TUNSETIFF`,
  `AF_PACKET`, `bpf()`, or UML network-helper exec;
- vector2 TAP seccomp Tier 3 soak pilot:
  `tier3-django-v2` and `tier3-fastapi-v2`, backend-filtered to
  `seccomp`, ran for 1266 seconds across 10 rotations with 200/200
  passes; every scoreboard row recorded vector2 `vec2.0`, TAP,
  `host_mode=inproc`, and `queue_count=1`; all 20 loop logs reported
  `PASS=10/10 FAIL=0 TIMEOUT=0`; all 200 run logs contained
  `SERVER_READY`, `GUEST_CURL ok=100 fail=0`, and `TIER3_OK`; the
  captured logs had no hidden fatal/BUG/KCSAN signatures and teardown
  left no `soak-tap0` or UML soak process;
- vector2 TAP seccomp Tier 3 requested-stop long run:
  `45-uml-vector-driver-v2-seccomp-soak-status.md` records a
  6142-second run against a 7200-second budget, stopped on request at
  85.3% consumed; it completed 970/970 passes, with Django-v2 at
  490/490 and FastAPI-v2 at 480/480; all rows recorded vector2
  `vec2.0`, TAP, `host_mode=inproc`, and `queue_count=1`; all 97
  loop logs reported `PASS=10/10 FAIL=0 TIMEOUT=0`; all 970 run logs
  contained `SERVER_READY`, `GUEST_CURL ok=100 fail=0`, and
  `TIER3_OK`; the captured logs had no hidden fatal/BUG/KCSAN
  signatures and teardown left no `soak-tap0` or matching soak/UML
  process;
- both-drivers kernel compatibility for explicit vector2 syntax:
  legacy vector leaves `vec2.` and `vec2=` for vector2 while preserving
  legacy `vec2:` as old-driver unit 2; the short perf baseline showed
  `vec2.0` registering successfully in a both-drivers kernel;
- initial legacy-vs-vector2 performance baseline helper:
  `tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh`,
  producing `/tmp/um-vector-perf-baseline/summary.tsv` with
  guest-to-host legacy vector TAP at 662 MiB/s guest-side and vector2
  fd multiqueue at 220 MiB/s guest-side for 32 MiB, plus
  `/tmp/um-vector-perf-h2g/summary.tsv` with host-to-guest legacy
  vector TAP at 3.2 MiB/s guest-side and vector2 fd multiqueue at
  456 MiB/s guest-side for 32 MiB;
- repeated-size perf helper smoke: vector2-only, both directions,
  1 MiB and 2 MiB, one repeat each, `summary.tsv` with `repeat` column,
  no lingering `vperf-v2-g2h` or `vperf-v2-h2g`;
- repeated legacy-vs-vector2 performance sweep: both directions,
  1 MiB/8 MiB/32 MiB, two repeats per cell; vector2 averaged
  45/159/224 MiB/s guest-to-host versus legacy vector 70/358/625 MiB/s,
  and vector2 averaged 231/357/453 MiB/s host-to-guest versus legacy
  vector 0.5/2.2/2.4 MiB/s; no lingering perf TAPs or UML processes;
- vector2 TAP `queues=2` seccomp smoke and queue distribution evidence;
- vector2 `ndo_select_queue` and XPS queue-to-CPU policy KUnit
  evidence;
- `umlctl up --strace` vector2 auto-queue fd audit:
  `started vector2-auto-queues pid=3807670 run_id=01KRV3S353GXS9FQZA082DSADE`,
  `VECTOR2_AUTO_QUEUES_OK`, `TAP_ABSENT`, `UML_PROCESS_ABSENT`, and a
  narrowed syscall scan with no actual `/dev/net/tun` open,
  `TUNSETIFF`, `AF_PACKET`, `bpf()`, or UML network-helper exec; broad
  raw-socket hits were traced to guest `ip` and `ping` userspace;
- `umlctl gate loop --audit-vector-sandbox` vector2 auto-queue fd
  smoke: `PASS=1/1 FAIL=0 TIMEOUT=0`, saved `strace-1.log`,
  `strace-audit-1.log`, `TAP_ABSENT`, `UML_PROCESS_ABSENT`, and audit
  pass text reporting no host TAP open, `TUNSETIFF`, `AF_PACKET`,
  `bpf()`, or UML network-helper exec;
- KVM-v2 vector2 readiness checkpoint:
  `/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux`
  was built with `CONFIG_UM_BACKEND_KVM_V2=y`,
  `CONFIG_UM_BACKEND_DYNAMIC=y`, `CONFIG_UML_NET_VECTOR_V2=y`, and
  `CONFIG_UML_NET_VECTOR_V2_SANDBOX=y`; no-network KVM-v2 readiness
  passed `PASS=1/1`, vector2 fd-handoff on KVM-v2 passed `PASS=1/1`,
  Django stdlib-shim vector2 fd smoke on KVM-v2 passed `PASS=1/1`, but
  the 30-run Django KVM-v2 attempt finished `PASS=29/30 FAIL=1` after
  guest `python3` aborted during server startup on iteration 25; the
  same Django/vector2 shape passed a seccomp control `PASS=3/3`; after
  adding server-log dump markers, a second KVM-v2 Django 30-run passed
  `PASS=30/30 FAIL=0`, with all 30 copied logs reaching
  `SERVER_READY`, `GUEST_CURL ok=100 fail=0`, and `TIER3_OK`, no fail
  signatures, and max `KVM_V2_TLB_LAG=2117`; a longer diagnostic
  sample reached `PASS=57/60 FAIL=2 TIMEOUT=1`, with 57
  `SERVER_READY` / `GUEST_CURL ok=100 fail=0` / `TIER3_OK` runs, two
  guest `python3` server failures before `SERVER_READY`
  (`Segmentation fault` and `Fatal Python error:
  _PyEval_EvalFrameDefault: Executing a cache.`), one `django-up`
  timeout, no lingering `v2djkv2`, and max `KVM_V2_TLB_LAG=2172`;
- KVM-v2 state-trace follow-up:
  `/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux`
  was built with `CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE=y` and
  `CONFIG_DEBUG_FS=y`; Tier 3 templates now dump
  `/sys/kernel/debug/um_kvm_v2_trace/dump` on `SERVER_FAIL` when
  present; a trace-enabled Django/vector2 KVM-v2 smoke passed
  `PASS=1/1 FAIL=0 TIMEOUT=0`; a trace-enabled 60-run reached
  `PASS=59/60 FAIL=1 TIMEOUT=0`, with iteration 21 aborting guest
  `python3` before `SERVER_READY`, dumping
  `Fatal Python error: _PyEval_EvalFrameDefault: Executing a cache.`
  while importing `re` / `email.utils` / `http.server`, and then
  emitting `KVMV2T_DUMP_BEGIN reason=debugfs entries=5140`; the failed
  iteration max `KVM_V2_TLB_LAG` was 943, while the run max was 2410
  and 59/60 logs had max lag above 1000; the
  `tools/testing/selftests/um/soak/kvmv2-trace-summary.py` helper
  reassembled the failure dump into 5140 parsed / 5140 complete entries,
  reported the pid 161/1 trace distribution, two dispatch pid/tmm
  switches, two post-syscall run/task mismatches, and showed neighboring
  pass logs had higher TLB-lag maxima without a trace dump; the older
  `tools/testing/selftests/um/state-trace/parse-trace.py invariants`
  pass reported 4 critical pid/tmm stability violations around the
  pid 161/1 transition, while `mmap-zero` reported no mmap-returned-zero
  event;
- KVM-v2 post-syscall shared-run hardening:
  `kvm_v2_handle_io_trap()` no longer reads from or writes to the shared
  `kvm_run` mmap after `handle_syscall()` returns, and the trace helper
  skips fixed-kernel `HANDLE_SYSCALL_POST` rows without the syscall IO
  port when reporting post-syscall mismatches.  The old failing trace
  still reports the original two mismatches; a rebuilt trace runtime
  passed a one-shot Django/vector2 KVM-v2 smoke, but the follow-up
  30-run still failed `PASS=29/30 FAIL=1 TIMEOUT=0` with a guest Python
  abort, a complete 5140-entry trace dump, two dispatch pid/tmm switches,
  two syscall task/mm switches, three mm-generation backsteps, no
  post-syscall mismatches, and the same no-mmap-zero result.  This is
  progress on the ownership model, not a KVM-v2 Tier 3 closeout;
- KVM-v2 `regs` owner trace follow-up: the state trace now records
  whether the sampled `uml_pt_regs` pointer belongs to `current`, and
  the summary helper reports `regs_owner_mismatches`; the rebuilt trace
  runtime passed a one-shot Django/vector2 KVM-v2 smoke, then reproduced
  the failure at `PASS=29/30 FAIL=1 TIMEOUT=0` with a complete trace
  reporting `regs_owner_mismatches: count=0`, which rules out stale
  `uml_pt_regs` ownership as the direct reason for the pid/tmm invariant
  hits;
- KVM-v2 no-network Python import control:
  the same trace runtime passed `PASS=30/30 FAIL=0 TIMEOUT=0` with
  `network.mode = "none"` and 100 fresh `python3` import startups per
  boot for the Django-failure stdlib modules.  All 30 run logs contained
  exactly one `PY_IMPORT_COUNT ok=100` marker and no fatal Python, abort,
  BUG, panic, KCSAN, or trace-dump markers; the passing logs still
  contained 947 `KVM_V2_TLB_LAG` diagnostics with max lag 2685, so
  simple no-network Python import startup did not reproduce the abort
  class and TLB lag alone is not a failure classifier;
- KVM-v2 no-network Django-loopback control:
  the same trace runtime reproduced the workload flake without vector2
  at `PASS=27/30 FAIL=2 TIMEOUT=1`; the reusable template is
  `tools/testing/selftests/um/soak/django-loopback-none.toml.template`.
  The 27 passing logs reached `SERVER_READY`, `GUEST_CURL ok=100
  fail=0`, and `TIER3_OK`, while the non-passing logs included a fatal
  Python `Executing a cache` abort in the readiness-probe helper, an
  abort of the background stdlib HTTP server before readiness, and a
  timeout during `django-up`.  This makes the KVM-v2 backend the blocker
  for vector2 KVM-v2 Tier 3 acceptance;
- KVM-v2 no-network Django-loopback delayed trace capture:
  after the reusable control was changed to dump all `KVMV2T` dmesg
  lines, a rendered-template smoke passed `PASS=1/1 FAIL=0 TIMEOUT=0`
  and a delayed-classification 30-run reproduced the flake at
  `PASS=27/30 FAIL=3 TIMEOUT=0`; one failure produced a partial
  state-trace dump with 2956 parsed / 2955 complete entries out of 4854
  declared entries, one dispatch switch, one syscall switch, three
  mm-generation backsteps, `regs_owner_mismatches: count=0`, and one
  critical `tmm changed mid-dispatch` invariant hit;
- TAP teardown checks showing no lingering `soak-tap0`.

## Remaining Work

Vector v2 is not replacement-ready.  The shortest honest remaining
list is:

- fix or explain the remaining KVM-v2 Django workload instability: the
  correctly configured KVM-v2 runtime now boots, runs vector2 fd smoke,
  has one clean Django `PASS=30/30` rerun, and now captures and parses a
  trace-ring dump on failure, but longer samples still fail at
  `PASS=57/60 FAIL=2 TIMEOUT=1`, `PASS=59/60 FAIL=1 TIMEOUT=0`, and
  post-hardened `PASS=29/30 FAIL=1 TIMEOUT=0` with guest Python
  failures and one startup timeout; the direct post-syscall stale
  `kvm_run` consumption path has been removed and stale `uml_pt_regs`
  ownership has been ruled out; a no-network Python import control did
  not reproduce the abort across 3000 fresh import startups, but a
  no-network Django-loopback control reproduced the flake without
  vector2, so the next backend audit must resolve guest memory/TLB
  state, Django/server/socket workload state, or another KVM-v2
  userspace-corruption path;
- repeat vector2 Tier 3 Django on kvm-v2 after the backend
  investigation until the flake rate is acceptably bounded;
- capture a complete no-network Django-loopback state trace; the latest
  delayed capture recovered useful task/mm evidence but missed the dump
  end marker and did not include all declared entries;
- let the vector2 seccomp Tier 3 soak finish the accepted
  7200-second/CI window naturally; the latest stopped-clean run reached
  6142 seconds and 970/970 passes, but ended by operator request;
- validate queue-to-CPU policy under longer SMP traffic;
- extend KCSAN concurrency/fairness profiles under longer SMP traffic,
  broader queue/flow matrices, and kvm-v2 after its baseline is fixed;
- expand performance profiling beyond TCP throughput and explain or
  accept the measured guest-to-host regression and mixed bidirectional
  results;
- wire the vector sandbox audit gate into CI/preflight, run it on longer
  workload repetitions, cover Django, and decide the guest userspace
  raw/netlink socket policy for secure profiles;
- run a repeated long soak with vector2 workloads;
- decide and implement the legacy `vecN:` transition.

Until those are done, vector v2 must remain an experimental parallel
driver selected explicitly through `vec2.*` or `umlctl`
`network.driver = "vector2"`.
