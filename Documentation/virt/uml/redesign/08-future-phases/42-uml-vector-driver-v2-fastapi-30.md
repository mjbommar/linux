# UML vector driver v2 FastAPI 30-pass repetition

**Status:** longer FastAPI/uvicorn seccomp repetition evidence.
**Date:** 2026-05-17.

This note records a 30-iteration `umlctl gate loop` pass for the real
FastAPI + uvicorn vector2 fd-handoff workload.  It extends the earlier
one-shot and 10/10 repetitions without changing the workload shape.

This is still not a substitute for an hours-long soak or kvm-v2
validation.  It does close the narrower audit gap that asked for the
FastAPI/uvicorn vector2 seccomp smoke to be repeated beyond the short
10/10 run.

## Kernel

Kernel:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux
```

Runtime vector2 shape:

```text
driver=vector2
guest_dev=vec2.0
tap=v2fastapi0
transport=fd
host_mode=fd
queues=2
queue_spec=auto
inherited fds=200..201
```

## Command

```sh
rm -rf /tmp/um-vector-fastapi-30
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  timeout 1800s tools/uml/uml-launcher/target/debug/umlctl gate loop \
    -f tools/uml/uml-launcher/examples/vector2-fastapi-smoke.toml \
    -W 1 -M 30 --timeout 300 \
    --pass-marker VECTOR2_FASTAPI_OK \
    --fail-marker 'VECTOR2_FASTAPI_FAIL|SERVER_FAIL|VERIFY_FAIL|kernel BUG|Kernel panic|BUG:|WARNING:|KCSAN:|data-race' \
    --out /tmp/um-vector-fastapi-30
```

## Result

Gate-loop result:

```text
==> default PASS=30/30 FAIL=0 TIMEOUT=0 rate=100.0% (Wilson 95% CI [88.6%, 100.0%]) elapsed=298s
```

Per-run marker counts across the 30 copied run logs:

```text
SERVER_READY=30
FASTAPI_HTTP=30
REPRO_DONE=30
```

Every run contained:

```text
SERVER_READY
FASTAPI_HTTP ok=51 fail=0
VECTOR2_FASTAPI_OK
REPRO_DONE rc=0
```

The copied per-run logs were scanned for:

```text
VECTOR2_FASTAPI_FAIL
SERVER_FAIL
VERIFY_FAIL
kernel BUG
Kernel panic
BUG:
WARNING:
KCSAN:
data-race
not ok
FAILED
```

The scan returned no matches.

Post-run cleanup:

```text
TAP_ABSENT
UML_PROCESS_ABSENT
```

## What This Closes

The FastAPI/uvicorn vector2 seccomp workload now has a 30/30 repetition
checkpoint through launcher-owned fd handoff.

## What Remains Open

The remaining workload-soak gap is broader than this checkpoint:

- hours-long vector2 workload soaks;
- kvm-v2 FastAPI/Django reruns after the separate kvm-v2 baseline
  blocker is fixed;
- CI/preflight integration for the repeated workload gate;
- cross-host and cross-kernel repetition.
