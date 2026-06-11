# UML Vector2 Current-HEAD Validation

Date: 2026-06-11

Branch: `next`

Evidence base: current `next` after `15af7e815d8e`, with the `umlctl`
mconsole readiness timeout alignment in this update.

UML binary after rebuild: `7.1.0-rc7-00261-gbb092119158b`

Purpose: refresh vector2 validation on the current `next` head and add a
bounded KVM-v2/vector2 Tier 3 smoke. This is publication-readiness evidence,
not replacement approval. The full 7200-second seccomp run, full KVM-v2 Tier 3
coverage, and multiqueue fairness/performance gates remain open.

## Setup

The in-tree UML binary was rebuilt from current `HEAD`:

```sh
make ARCH=um -j$(nproc)
```

Common environment:

```sh
export UML_KERNEL=$PWD/linux
export UM_FORK_KERNEL=$PWD/linux
export UMLCTL=$PWD/tools/uml/uml-launcher/target/debug/umlctl
```

The rebuilt kernel reported:

```text
7.1.0-rc7-00261-gbb092119158b
```

The launcher was rebuilt after the mconsole readiness timeout update:

```sh
cargo fmt --check
cargo test --locked
cargo build --locked --bin umlctl
```

Result: formatting passed, the locked launcher test run passed 215 tests, and
the debug `umlctl` binary rebuilt successfully.

## KVM-v2 Vector2 Tier 3 Smoke

Command:

```sh
COOLDOWN=0 THERMAL_PAUSE_C=95 THERMAL_RESUME_C=85 \
tools/testing/selftests/um/soak/run-soak-daemon.sh \
  --budget-sec 1 \
  --workers 1 \
  --iters-per-rotation 1 \
  --workloads tier3-django-v2,tier3-fastapi-v2 \
  --backends kvm-v2 \
  --out /tmp/uml-vector2-kvm-tier3-head.nvncU2 \
  --continue-on-fail-threshold
```

Result:

| Workload | Backend | Iterations | Result | Markers |
| --- | --- | ---: | --- | --- |
| `tier3-django-v2` | `kvm-v2` | 1 | PASS | `SERVER_READY`, `GUEST_CURL ok=100 fail=0`, `TIER3_OK`, `REPRO_DONE rc=0` |
| `tier3-fastapi-v2` | `kvm-v2` | 1 | PASS | `SERVER_READY`, `GUEST_CURL ok=100 fail=0`, `TIER3_OK`, `REPRO_DONE rc=0` |

Scoreboard metadata recorded `uml_network_driver=vector2`,
`uml_netdev_name=vec2.0`, `uml_vmm_backend=kvm-v2`, `uml_transport=tap`,
`uml_queue_count=1`, and `uml_host_mode=inproc` for both rows.

## Focused Vector2 Smokes

| Gate | Result | Evidence |
| --- | --- | --- |
| `vector2-fd-handoff-smoke` | PASS | Launcher-created TAP `v2fd469685`, fd 200 handoff, guest address `10.91.105.2`, guest metadata, `vec2.0` address, and host TAP ping succeeded. |
| `vector2-fd-multiqueue-smoke` | PASS | Multiqueue TAP `v2mq470024` created, fds 200..203 inherited, guest address `10.92.84.2`, guest reported four queues/fds, `vec2.0` reported four TX queues, and host TAP ping succeeded. |
| `vector2-inproc-tap-smoke` | PASS | Trusted TAP/in-process TAP `v2ip470337` reported no launcher fd inheritance, guest address `10.93.217.2`, guest metadata matched TAP/inproc mode, and host TAP ping succeeded. |
| `vector2-sandbox-audit` | PASS | `umlctl gate loop --audit-vector-sandbox` reported `PASS=1/1 FAIL=0 TIMEOUT=0` with no forbidden host operations. |
| `vector2-pool-tap-smoke` | PASS | Pool member received per-member TAP identity, daemon-routed exec validated connectivity, and destroy completed. The final post-patch run passed when run alone; concurrent pool exec pressure can still delay mconsole readiness, so the readiness wait now follows the caller timeout rather than a fixed 10 seconds. |
| `pool-exec-smoke` | PASS | Daemon-routed `/bin/true`, stdout/stderr, exit-code, timeout, and cleanup cases passed after the mconsole readiness timeout update. |
| `vector2-failed-open` | PASS | fd-handoff TAP path with `fail_open_after=2` observed `open_delta=1`, `fail_delta=1`, and `close_delta=1`, then left the device closed/registered and cleaned up the host TAP. |
| `net-bench` guest-to-host TCP | PASS | Legacy vector and vector2 each completed 3/3 iterations through `umlctl`; medians were 41865.7 Mbps and 40118.6 Mbps, for a 0.958 ratio against the 0.85 gate. |

## Vector2 KUnit

Command:

```sh
timeout 180 ./linux mem=256M \
  kunit.filter_glob='um_vector2_*' \
  kunit_shutdown=halt con=null con0=fd:0,fd:1 panic=-1
```

Result:

| Suite | Result |
| --- | --- |
| `um_vector2_config` | 12 pass, 0 fail, 0 skip |
| `um_vector2_queue` | 9 pass, 0 fail, 0 skip |
| `um_vector2_transport` | 8 pass, 0 fail, 0 skip |
| `um_vector2_fake_host` | 10 pass, 0 fail, 0 skip |
| `um_vector2_model` | 7 pass, 0 fail, 0 skip |
| `um_vector2_cmdline` | 5 pass, 0 fail, 0 skip |
| `um_vector2_netdev` | 18 pass, 0 fail, 0 skip |
| `um_vector2_ethtool` | 6 pass, 0 fail, 0 skip |
| `um_vector2_host_fd` | 14 pass, 0 fail, 0 skip |
| `um_vector2_host_tap` | 7 pass, 0 fail, 2 skip |

Total: 96 pass, 0 fail, 2 skip. The two skips are the trusted in-process TAP
host-open cases that are skipped when the build is allowed to open host TAP.

## Cleanup

Post-run checks found no leftover `v2fd*`, `v2mq*`, `v2ip*`, `v2pm*`,
`v2failopen*`, `tcpbench*`, or `soak-tap*` host links, and no leftover matching
UML/vector2 processes.

## Current Disposition

This refresh proves that the current `next` kernel and rebuilt `umlctl` still
pass the vector2 unit and focused live gates, that the daemon `exec` path is
operational through the supported `exec/1` ABI, and that a minimal
KVM-v2/vector2 Tier 3 Django and FastAPI slice can complete on the current
stack. It also records the current guest-to-host TCP gate as passing at a
0.958 vector2/vector ratio.

It does not close vector2 replacement readiness. Remaining work:

- allow the seccomp/vector2 7200-second Tier 3 soak to complete naturally;
- run full KVM-v2/vector2 Tier 3 coverage, not just the one-iteration smoke;
- add or refresh multiqueue fairness and performance gates; and
- keep v2 opt-in until the replacement gates pass.
