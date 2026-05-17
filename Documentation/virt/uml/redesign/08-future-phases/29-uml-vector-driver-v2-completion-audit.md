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
| `ip link set up/down` reaches modeled lifecycle | KUnit lifecycle tests; fd/TAP netdev open/stop tests; manual TAP/fd smokes | Partial: not 10,000-cycle failure-injection proof |
| Single-queue trusted TAP packet path | R5 TAP datapath doc; ping smokes; Tier 3 seccomp 30/30 | Done for trusted TAP/seccomp |
| Single-queue and multiqueue fd transport with launcher-supplied fds | fd datapath exists over inherited fds; sandbox accepts inherited `fd=`; manual no-root fd ping smokes; `umlctl` records manifest labels and passes vector2 TAP fds as inherited fd ranges starting at 200; live single-queue and 4-queue `umlctl up` fd-handoff smokes passed; `queues = "auto"` / `--network-queues auto` resolve to numeric vector2 fd ranges from `[runtime].ncpus` | Done for launch path; deeper SMP/perf still open |
| TX/RX move through v2 queues, not legacy queues | `vector2_queue` rings/batches used by fd and TAP; KUnit TX/RX tests | Done for implemented fd/TAP paths |
| Tier 3 Django/FastAPI on seccomp and kvm-v2 | Django stdlib shim passes seccomp 30/30; real FastAPI + uvicorn vector2 fd smoke passes seccomp 1/1 with `FASTAPI_HTTP ok=51 fail=0`; kvm-v2 no-network readiness fails before vector2 validation | Partial: kvm-v2 and longer FastAPI repetition remain open |
| KUnit coverage for config, lifecycle, queue, fake host, transport, host-open failure, unwind, queue policy | `um_vector2_*` KUnit passes 72/72 after fd wrong-type, fd multiqueue unwind, and queue-to-CPU policy coverage | Partial: coverage exists, but more failure injection remains |
| ethtool stats and ring queries stopped/running | R6/R8b docs; KUnit ethtool tests; live queue stats | Done for current surfaces |
| Sandbox blocks host helper/TAP/raw/BPF creation | parser rejects trusted host options without `INPROC`; TAP sandbox KUnit; inherited fd allowed by policy; `umlctl` fd handoff keeps TAP opening in the launcher | Partial: needs strace/audit gate |
| Multiqueue TAP/fd KCSAN and distribution | TAP multiqueue works and queue counters move; fd multiqueue core opens contiguous inherited fd ranges under KUnit; `umlctl` fd multiqueue passes live 4-queue smoke and 3/3 gate loop; vector2 has explicit `ndo_select_queue` plus XPS queue-to-CPU policy; short KCSAN auto-queue fd multiqueue smoke passed with no data-race signatures; broader KCSAN and fairness profiles absent | Partial |
| Performance parity or accepted regression | No current v2 vs legacy perf baseline in this checkpoint set | Open |
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

Validation evidence recorded in the checkpoint docs includes:

- targeted vector2 object builds;
- full trusted runtime UML build;
- full sandbox runtime UML build;
- `um_vector2_*` KUnit: 72/72 passed;
- no-root fd ping over inherited UNIX datagram fd;
- sandbox-only fd ping over inherited UNIX datagram fd;
- fd open diagnostics for missing and wrong-type inherited fds;
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
- short `umlctl gate loop` vector2 fd handoff repetition:
  `PASS=3/3 FAIL=0 TIMEOUT=0` and no lingering `v2fd0`;
- short `umlctl gate loop` vector2 fd multiqueue repetition:
  `PASS=3/3 FAIL=0 TIMEOUT=0`, no lingering `v2fdmq0`, and no
  cleanup failure logs;
- `umlctl gate loop` TAP cleanup audit marks an iteration failed if
  the declared TAP remains under `/sys/class/net` after teardown;
- vector2 TAP seccomp Tier 3 Django stdlib shim: 30/30 passed;
- vector2 fd seccomp FastAPI + uvicorn one-shot: `PASS=1/1`,
  gateway ping over `vec2.0`, `SERVER_READY`,
  `FASTAPI_HTTP ok=51 fail=0`, `VECTOR2_FASTAPI_OK`,
  `REPRO_DONE rc=0`, and no lingering `v2fastapi0`;
- vector2 TAP `queues=2` seccomp smoke and queue distribution evidence;
- vector2 `ndo_select_queue` and XPS queue-to-CPU policy KUnit
  evidence;
- TAP teardown checks showing no lingering `soak-tap0`.

## Remaining Work

Vector v2 is not replacement-ready.  The shortest honest remaining
list is:

- fix the separate kvm-v2 baseline readiness blocker;
- rerun vector2 Tier 3 Django 30/30 on kvm-v2 after that fix;
- repeat the FastAPI/uvicorn vector2 seccomp smoke for a larger count;
- validate queue-to-CPU policy under longer SMP traffic;
- run longer KCSAN on multiqueue traffic;
- collect legacy-vs-v2 performance baselines;
- run a repeated long soak with vector2 workloads;
- decide and implement the legacy `vecN:` transition.

Until those are done, vector v2 must remain an experimental parallel
driver selected explicitly through `vec2.*` or `umlctl`
`network.driver = "vector2"`.
