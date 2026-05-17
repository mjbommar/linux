# UML vector driver v2 R8d KCSAN auto-queue smoke

**Status:** R8 partial - superseded by the R8e lockdep follow-up.
**Date:** 2026-05-17.

This note records a focused KCSAN run for the `umlctl` vector2
auto-queue path.  It is useful evidence that the launcher-owned fd
multiqueue path boots and moves traffic under a KCSAN UML kernel, but
it is not the full long SMP/KCSAN validation gate.

The later R8e follow-up extends this path to ten iterations, documents
the lockdep warning found by scanning all logs, fixes the queue locking
discipline, and records a clean post-fix `PASS=10/10` KCSAN run.  See
`35-uml-vector-driver-v2-r8e-kcsan-lockdep.md`.

## Kernel

Build directory:

```text
/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan
```

Runtime version:

```text
7.0.0-ga1df06608323-dirty
```

Relevant config:

```text
CONFIG_SMP=y
CONFIG_UML_NET_VECTOR_V2=y
# CONFIG_UML_NET_VECTOR_V2_INPROC is not set
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
CONFIG_DEBUG_FS=y
CONFIG_KCSAN=y
CONFIG_KCSAN_EARLY_ENABLE=y
```

## Command

```sh
rm -rf /tmp/um-vector-auto-kcsan
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux \
  timeout 900s cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
    --bin umlctl -- gate loop \
    -f tools/uml/uml-launcher/examples/vector2-auto-queues.toml \
    -W 1 -M 1 --timeout 240 \
    --pass-marker VECTOR2_AUTO_QUEUES_OK \
    --out /tmp/um-vector-auto-kcsan/loop
test ! -e /sys/class/net/v2autoq0
```

## Result

```text
PASS=1/1 FAIL=0 TIMEOUT=0
TAP_ABSENT
```

The generated worker Umlfile preserved the ergonomic user intent:

```toml
[runtime]
ncpus = 4

[network]
driver = "vector2"
host_mode = "auto"
queues = "auto"
```

The runtime output showed the resolved vector2 contract:

```text
[umlctl] network: driver=vector2 guest_dev=vec2.0 tap=v2autoq0 transport=fd host_mode=fd queues=4 queue_spec=auto
Kernel command line: ... vec2.0:transport=fd,mode=fd,fd=200,depth=128,queues=4 ...
uml-vector2: vec2.0 configured transport=fd mode=fd requested_queues=4 runtime_queues=4 depth=128
UMLCTL_NETWORK_QUEUE_SPEC=auto
UMLCTL_NETWORK_QUEUES=4
numtxqueues 4 numrxqueues 4
VECTOR2_AUTO_QUEUES_OK
```

The captured run log did not contain KCSAN data-race signatures:

```text
BUG: KCSAN
data-race
KCSAN:
WARNING:.*data race
```

The only KCSAN line found was the normal boot-time mode banner:

```text
kcsan: non-strict mode configured - use CONFIG_KCSAN_STRICT=y to see all data races
```

## Remaining Gate

This smoke proved that the auto queue sizing path and vector2 fd
multiqueue path could pass one KCSAN-instrumented seccomp run.  The
later R8e scan found and fixed a queue-lock lockdep warning on this
same path.  Remaining work:

- longer KCSAN traffic with concurrent TCP/UDP flows;
- queue fairness and per-queue distribution measurements;
- kvm-v2 readiness and vector2 replay after the kvm-v2 baseline is
  fixed;
- legacy-vs-v2 performance baselines.
