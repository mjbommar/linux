# UML vector driver v2 KCSAN FastAPI smoke

**Status:** partial KCSAN workload evidence.
**Date:** 2026-05-17.

This note records a real FastAPI + uvicorn vector2 fd-handoff workload
under the KCSAN UML kernel.  It is heavier than the earlier auto-queue
ping smoke because it starts Python userspace, serves HTTP inside the
guest, performs 51 HTTP requests, and uses vector2 networking through
launcher-owned TAP fds.

It still does not close the full KCSAN gate.  Concurrent TCP/UDP flows,
longer runtime, and queue fairness profiles remain open.

## Kernel

Kernel:

```text
/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux
```

Relevant config:

```text
CONFIG_SMP=y
CONFIG_KCSAN=y
CONFIG_KCSAN_EARLY_ENABLE=y
CONFIG_UML_NET_VECTOR_V2=y
# CONFIG_UML_NET_VECTOR_V2_INPROC is not set
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
```

## Command

```sh
rm -rf /tmp/um-vector-fastapi-kcsan
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux \
  timeout 900s tools/uml/uml-launcher/target/debug/umlctl gate loop \
    -f tools/uml/uml-launcher/examples/vector2-fastapi-smoke.toml \
    -W 1 -M 1 --timeout 360 \
    --pass-marker VECTOR2_FASTAPI_OK \
    --fail-marker 'VECTOR2_FASTAPI_FAIL|SERVER_FAIL|VERIFY_FAIL|kernel BUG|Kernel panic|BUG: KCSAN|data-race|KCSAN:|WARNING:' \
    --out /tmp/um-vector-fastapi-kcsan
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

## Result

Gate-loop result:

```text
==> default PASS=1/1 FAIL=0 TIMEOUT=0
elapsed=137s
```

The copied run log included:

```text
SERVER_READY
FASTAPI_HTTP ok=51 fail=0
VECTOR2_FASTAPI_OK
REPRO_DONE rc=0
```

Post-run cleanup:

```text
TAP_ABSENT
UML_PROCESS_ABSENT
```

Signature scan of the copied run log found no:

```text
SERVER_FAIL
VECTOR2_FASTAPI_FAIL
BUG: KCSAN
data-race
KCSAN:
WARNING:
kernel BUG
BUG:
Kernel panic
not ok
FAILED
```

The run log contains the normal boot line:

```text
RCU lockdep checking is enabled.
```

That line is informational and is not a lockdep splat.

## What This Closes

This checkpoint adds workload-level KCSAN evidence for vector2 fd
handoff.  The driver has now passed:

- the earlier repeated auto-queue KCSAN smoke;
- the queue-lock bottom-half lockdep fix validation;
- one real FastAPI + uvicorn vector2 fd KCSAN run.

## What Remains Open

The remaining KCSAN/concurrency gate still needs:

- concurrent TCP and UDP flows under KCSAN;
- longer multiqueue runtime than this one FastAPI smoke;
- queue fairness and per-queue distribution under concurrent traffic;
- kvm-v2 reruns after the separate kvm-v2 baseline blocker is fixed.
