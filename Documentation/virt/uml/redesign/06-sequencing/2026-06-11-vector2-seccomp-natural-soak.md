# UML Vector2 Seccomp Natural Soak

Date: 2026-06-11

Branch: `next`

Commit tested: `899e80995800`

Kernel: `7.1.0-rc7-00261-gbb092119158b`

Status: PASS. The seccomp/vector2 Tier 3 soak reached the natural 7200-second
budget stop.

## Scope

```text
workloads=tier3-django-v2,tier3-fastapi-v2
backends=seccomp
workers=1
iters-per-rotation=10
network_driver=vector2
guest_dev=vec2.0
transport=tap
host_mode=inproc
queue_count=1
```

KVM-v2 was intentionally excluded. This gate isolates vector2 on the stable
seccomp backend; KVM-v2 Tier 3 networking remains a separate publication gate.

## Command

```sh
timeout --kill-after=120 9000s \
  env UML_KERNEL=$PWD/linux \
      UMLCTL=$PWD/tools/uml/uml-launcher/target/debug/umlctl \
      COOLDOWN=5 \
      SOAK_OUT=/home/mjbommar/projects/personal/.tmp/uml-vector2/vector2-seccomp-tier3-soak-7200-next-899e80995800 \
  bash tools/testing/selftests/um/soak/run-soak-daemon.sh \
    --budget-sec 7200 \
    --backends seccomp \
    --workloads tier3-django-v2,tier3-fastapi-v2 \
    --workers 1 \
    --iters-per-rotation 10 \
    --out /home/mjbommar/projects/personal/.tmp/uml-vector2/vector2-seccomp-tier3-soak-7200-next-899e80995800
```

The outer `timeout` was only a guard. The daemon stopped by its own budget
condition before the outer timeout fired:

```text
budget elapsed (7221s >= 7200s); stopping
uml-soak STOP run_id=vector2-seccomp-tier3-soak-7200-next-899e80995800 rotations=37 elapsed=7221s
```

## Result

Scoreboard:

```text
rows=740
bad_rows=0
tier3-django-v2/seccomp: 370/370 PASS
tier3-fastapi-v2/seccomp: 370/370 PASS
```

All scoreboard rows recorded:

```text
uml_network_driver=vector2
uml_netdev_name=vec2.0
uml_vmm_backend=seccomp
uml_transport=tap
uml_host_mode=inproc
uml_queue_count=1
```

The daemon summary reached:

```text
Start: 2026-06-11T19:40:36Z
Now:   2026-06-11T21:40:57Z
Elapsed: 7221s
Budget: 7200s
Throttle pauses: 0 events, 0s total
```

The summary writer still reported `Stop: running` in `summary.md`, but the
daemon process exited 0 after printing the budget-elapsed stop line above.

## Marker And Signature Checks

The artifact tree contained 740 per-run logs. Every run log contained:

```text
SERVER_READY: 740/740
GUEST_CURL ok=100 fail=0: 740/740
TIER3_OK: 740/740
REPRO_DONE rc=0: 740/740
```

The per-run logs were scanned for:

```text
SERVER_FAIL
TIER3_FAIL
Fatal Python error
Segmentation fault
Aborted
Kernel panic
kernel BUG
BUG:
WARNING:
KCSAN
data-race
not ok
FAILED
```

The run-log scan returned no matches.

Post-run cleanup checks found no matching soak process, no matching
`umlctl gate loop` or Tier 3 process, and no matching soak/vector2 TAP device.

## Interpretation

This closes the natural seccomp/vector2 long-soak gate for the current
`next` tree. It is observed 100% pass evidence for this gate, not proof that
the true failure rate is mathematically zero.

Vector2 replacement/publication still needs the separate KVM-v2 Tier 3
networking gate, broader performance/fairness evidence, and the final
integration matrix before making stronger replacement claims.
