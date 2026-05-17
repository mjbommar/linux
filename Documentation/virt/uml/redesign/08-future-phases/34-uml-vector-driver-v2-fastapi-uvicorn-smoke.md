# UML vector driver v2 FastAPI uvicorn smoke

**Status:** R7/R8 validation follow-up - short seccomp FastAPI.
**Date:** 2026-05-17.

This note records the first real FastAPI + uvicorn workload smoke for
vector2 through `umlctl`.  Earlier Tier 3 FastAPI templates used the
stdlib HTTP shim until operator dependencies were available.  The local
`/home/mjbommar/uml-venv` now has FastAPI and uvicorn installed, so the
workload can exercise the real framework stack.

This is still not a replacement gate closure.  It is a short seccomp
validation point; kvm-v2 and long FastAPI soak repetitions remain open.

## Workload

New example:

```text
tools/uml/uml-launcher/examples/vector2-fastapi-smoke.toml
```

Key properties:

- `backend = "seccomp"`;
- `driver = "vector2"`;
- `host_mode = "auto"`, resolving to launcher-owned fd handoff;
- `queues = "auto"`, resolving from `ncpus = 2`;
- host TAP `v2fastapi0`;
- guest netdev `vec2.0`;
- inherited fd range `200..201`;
- read-only bind mount of `${HOME}/uml-venv` at `/opt/venv`;
- app source generated inside the guest under `/tmp`;
- uvicorn started in the background;
- guest-side HTTP battery against `/health` and `/sum/100`;
- final marker emitted only after the server is killed and the
  shutdown phase reaches `REPRO_DONE rc=0`.

## Dependency Check

Host venv:

```text
/home/mjbommar/uml-venv/bin/python
fastapi 0.136.1
uvicorn 0.46.0
```

## One-Shot Command

```sh
rm -rf /tmp/um-vector-fastapi-smoke
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  timeout 900s cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
    --bin umlctl -- gate loop \
    -f tools/uml/uml-launcher/examples/vector2-fastapi-smoke.toml \
    -W 1 -M 1 --timeout 240 \
    --pass-marker VECTOR2_FASTAPI_OK \
    --out /tmp/um-vector-fastapi-smoke/loop
test ! -e /sys/class/net/v2fastapi0
```

## Result

```text
PASS=1/1 FAIL=0 TIMEOUT=0
TAP_ABSENT
```

Relevant run evidence:

```text
[umlctl] network: driver=vector2 guest_dev=vec2.0 tap=v2fastapi0 transport=fd host_mode=fd queues=2 queue_spec=auto
[umlctl] network-fd: open tap=v2fastapi0 and inherit fds=200..201
Kernel command line: ... ncpus=2 vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=2 ...
uml-vector2: vec2.0 configured transport=fd mode=fd requested_queues=2 runtime_queues=2 depth=128
UMLCTL_NETWORK_QUEUE_SPEC=auto
UMLCTL_NETWORK_QUEUES=2
3 packets transmitted, 3 received, 0% packet loss
SERVER_READY
FASTAPI_HTTP ok=51 fail=0
VECTOR2_FASTAPI_OK
REPRO_DONE rc=0
```

No `SERVER_FAIL`, `VECTOR2_FASTAPI_FAIL`, Python traceback, kernel
panic, or TAP leak was observed in the captured output.

## Short Repetition

Command:

```sh
rm -rf /tmp/um-vector-fastapi-10
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  timeout 1800s cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
    --bin umlctl -- gate loop \
    -f tools/uml/uml-launcher/examples/vector2-fastapi-smoke.toml \
    -W 1 -M 10 --timeout 240 \
    --pass-marker VECTOR2_FASTAPI_OK \
    --out /tmp/um-vector-fastapi-10/loop
test ! -e /sys/class/net/v2fastapi0
```

Result:

```text
PASS=10/10 FAIL=0 TIMEOUT=0
TAP_ABSENT
```

The 10 captured `run-*.log` files all contained:

```text
FASTAPI_HTTP ok=51 fail=0
VECTOR2_FASTAPI_OK
REPRO_DONE rc=0
```

The first and last run logs also showed the expected vector2 auto-queue
fd contract:

```text
requested_queues=2 runtime_queues=2
UMLCTL_NETWORK_QUEUE_SPEC=auto
UMLCTL_NETWORK_QUEUES=2
3 packets transmitted, 3 received, 0% packet loss
```

No `SERVER_FAIL`, `VECTOR2_FASTAPI_FAIL`, Python traceback, kernel
panic, or `BUG:` line was found in the 10 run logs.

## Sandbox Audit

Command:

```sh
rm -rf /tmp/um-vector-fastapi-audit
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  timeout 900s cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
    --bin umlctl -- gate loop \
    -f tools/uml/uml-launcher/examples/vector2-fastapi-smoke.toml \
    -W 1 -M 1 --timeout 240 \
    --pass-marker VECTOR2_FASTAPI_OK \
    --out /tmp/um-vector-fastapi-audit/loop \
    --audit-vector-sandbox
test ! -e /sys/class/net/v2fastapi0
```

Result:

```text
PASS=1/1 FAIL=0 TIMEOUT=0
TAP_ABSENT
UML_PROCESS_ABSENT
```

The saved run log again showed:

```text
requested_queues=2 runtime_queues=2
UMLCTL_NETWORK_QUEUE_SPEC=auto
UMLCTL_NETWORK_QUEUES=2
3 packets transmitted, 3 received, 0% packet loss
SERVER_READY
FASTAPI_HTTP ok=51 fail=0
VECTOR2_FASTAPI_OK
REPRO_DONE rc=0
```

The gate preserved:

```text
/tmp/um-vector-fastapi-audit/loop/p0_default/w0/run-1.log
/tmp/um-vector-fastapi-audit/loop/p0_default/w0/strace-1.log
/tmp/um-vector-fastapi-audit/loop/p0_default/w0/strace-audit-1.log
```

The `strace-1.log` file contained 2068786 lines.  The audit log
reported:

```text
vector sandbox strace audit passed: no host TAP open, TUNSETIFF, AF_PACKET, bpf(), or UML network-helper exec
```

## Remaining Gate

This closes only the "dependencies are available, run a real
FastAPI/uvicorn vector2 seccomp smoke" item and adds short repetition
evidence plus one audited sandbox run.  Remaining FastAPI work:

- repeat the FastAPI smoke for a larger/longer soak count;
- run the same workload after the kvm-v2 baseline readiness blocker is
  fixed;
- add host-to-guest curl battery evidence if the Tier 3 soak harness
  grows that side channel;
- compare legacy vector and vector2 performance for the same workload.
