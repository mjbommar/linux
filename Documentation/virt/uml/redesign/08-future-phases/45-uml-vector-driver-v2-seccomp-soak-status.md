# UML Vector Driver V2 Seccomp Soak Status

**Status:** stopped-clean long-soak evidence; not replacement approval.
**Date:** 2026-05-17.

This note records the end-of-day vector2 seccomp Tier 3 soak status.
The run was intentionally stopped by the operator before the 7200-second
budget completed.  It is therefore strong long-run evidence, but it is
not the final accepted 2-hour gate.

## Scope

The run isolated vector2 under the stable seccomp backend:

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

KVM-v2 was intentionally excluded.  KVM-v2 still has a separate
Django/server/socket workload instability that reproduces without
vector2, so mixing it into this soak would make the vector2 signal
harder to interpret.

## Command

```sh
rm -rf /home/mjbommar/projects/personal/.tmp/uml-vector2/vector2-seccomp-tier3-soak-2h

UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux \
UMLCTL=/home/mjbommar/projects/personal/linux/tools/uml/uml-launcher/target/release/umlctl \
COOLDOWN=5 \
SOAK_OUT=/home/mjbommar/projects/personal/.tmp/uml-vector2/vector2-seccomp-tier3-soak-2h \
bash tools/testing/selftests/um/soak/run-soak-daemon.sh \
  --budget-sec 7200 \
  --backends seccomp \
  --workloads tier3-django-v2,tier3-fastapi-v2 \
  --workers 1 \
  --iters-per-rotation 10 \
  --out /home/mjbommar/projects/personal/.tmp/uml-vector2/vector2-seccomp-tier3-soak-2h
```

The run used commit `3452635f7668` on branch `umlctl-deploy`.

## Result

The runner was stopped on request:

```text
Start: 2026-05-17T19:01:29Z
Stop:  2026-05-17T20:43:51Z
Elapsed: 6142s
Budget: 7200s (85.3% consumed)
Stop reason: requested
```

Scoreboard summary:

```text
total=970
verdicts={'PASS': 970}
tier3-django-v2/seccomp:  490/490 PASS
tier3-fastapi-v2/seccomp: 480/480 PASS
```

All scoreboard rows recorded:

```text
uml_network_driver=vector2
uml_netdev_name=vec2.0
uml_transport=tap
uml_host_mode=inproc
uml_queue_count=1
```

The copied logs contained 97 completed workload-loop logs and 970
per-run logs:

```text
loop_logs=97
clean_loop_10_10=97
run_logs=970
SERVER_READY present=970 missing=0
GUEST_CURL ok=100 fail=0 present=970 missing=0
TIER3_OK present=970 missing=0
```

The captured run logs were scanned for:

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

The scan returned no matches.

Post-stop cleanup checks:

```text
soak-tap0 absent
no matching run-soak-daemon, umlctl gate loop, or UML tier3 process
```

## Interpretation

This is the strongest vector2 seccomp Tier 3 soak evidence so far:

- it extends the earlier 1266-second pilot to 6142 seconds;
- it increases the completed run count from 200 to 970;
- it covers both Django-v2 and FastAPI-v2 in the soak daemon path;
- it repeats setup, guest HTTP readiness, 100 guest curls, and teardown
  across 97 completed 10-run workload batches;
- it shows no fatal, warning, KCSAN, data-race, or cleanup signal in
  the captured evidence.

It does not close the full long-soak gate because the run was stopped
at 85.3% of the planned 7200-second window.  The next acceptance run
should let the 7200-second seccomp/vector2 soak finish naturally, then
record the same scoreboard, marker, hidden-signature, and cleanup
checks.

## Current Open Items

Vector2 is still not replacement-ready.  The remaining blockers are:

- KVM-v2 Django/server/socket workload instability, reproduced even
  without vector2;
- KVM-v2 + vector2 Tier 3 acceptance after that backend blocker is
  fixed or bounded;
- a naturally completed 7200-second seccomp/vector2 soak;
- longer SMP fairness and queue profile coverage;
- broader KCSAN profiles and KVM-v2 reruns after baseline readiness;
- performance acceptance, including guest-to-host regression analysis;
- CI/preflight integration for the sandbox audit and soak gates.
