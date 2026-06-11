# UML Vector2 Current-HEAD Validation

Date: 2026-06-11

Branch: `next`

Commit tested: `98166580dc4f`

UML binary after rebuild: `7.1.0-rc7-00188-g98166580dc4f`

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
7.1.0-rc7-00188-g98166580dc4f
```

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
| `vector2-fd-handoff-smoke` | PASS | Launcher-created TAP, fd 200 handoff, guest metadata, `vec2.0` address, and host TAP ping succeeded. |
| `vector2-fd-multiqueue-smoke` | PASS | Multiqueue TAP created, fds 200..203 inherited, guest reported four queues/fds, `vec2.0` reported four TX queues, and host TAP ping succeeded. |
| `vector2-inproc-tap-smoke` | PASS | Trusted TAP/in-process path reported no launcher fd inheritance, guest metadata matched TAP/inproc mode, and host TAP ping succeeded. |
| `vector2-sandbox-audit` | PASS | `umlctl gate loop --audit-vector-sandbox` reported `PASS=1/1 FAIL=0 TIMEOUT=0` with no forbidden host operations. |
| `vector2-pool-tap-smoke` | PASS | Pool member received per-member TAP identity, daemon-routed exec validated connectivity, and destroy completed. |

## Vector2 KUnit

Command:

```sh
timeout 180 ./linux mem=256M \
  kunit.filter_glob='um_vector2_*' \
  kunit_shutdown=halt console=tty0
```

Result:

| Suite | Result |
| --- | --- |
| `um_vector2_config` | 12 pass, 0 fail, 0 skip |
| `um_vector2_queue` | 8 pass, 0 fail, 0 skip |
| `um_vector2_transport` | 8 pass, 0 fail, 0 skip |
| `um_vector2_fake_host` | 8 pass, 0 fail, 0 skip |
| `um_vector2_model` | 7 pass, 0 fail, 0 skip |
| `um_vector2_cmdline` | 5 pass, 0 fail, 0 skip |
| `um_vector2_netdev` | 13 pass, 0 fail, 0 skip |
| `um_vector2_ethtool` | 5 pass, 0 fail, 0 skip |
| `um_vector2_host_fd` | 13 pass, 0 fail, 0 skip |
| `um_vector2_host_tap` | 5 pass, 0 fail, 2 skip |

Total: 84 pass, 0 fail, 2 skip. The two skips are the trusted in-process TAP
host-open cases that are skipped when the build is allowed to open host TAP.

## Cleanup

Post-run checks found no leftover `v2fd*`, `v2mq*`, `v2ip*`, `v2pm*`, or
`soak-tap*` host links, and no leftover matching UML/vector2 processes.

## Current Disposition

This refresh proves that the current `next` binary still passes the vector2
unit and focused live gates, and that a minimal KVM-v2/vector2 Tier 3 Django
and FastAPI slice can complete on the current stack.

It does not close vector2 replacement readiness. Remaining work:

- allow the seccomp/vector2 7200-second Tier 3 soak to complete naturally;
- run full KVM-v2/vector2 Tier 3 coverage, not just the one-iteration smoke;
- add or refresh multiqueue fairness and performance gates; and
- keep v2 opt-in until the replacement gates pass.
