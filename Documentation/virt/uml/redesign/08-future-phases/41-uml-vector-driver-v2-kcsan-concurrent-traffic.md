# UML vector driver v2 KCSAN concurrent traffic gate

**Status:** KCSAN concurrent TCP/UDP evidence for vector2 fd multiqueue.
**Date:** 2026-05-17.

This note records the first reusable KCSAN gate that drives concurrent
TCP and UDP traffic through vector2 in both directions while checking
per-queue movement.  The gate uses `umlctl` vector2 fd handoff, so the
guest receives launcher-owned TAP fds instead of opening TAP inside the
sandboxed UML process.

The gate is intentionally not a throughput benchmark.  It is a
concurrency and safety signal: packet movement, queue distribution,
teardown, and absence of KCSAN or kernel warning signatures matter more
than raw bandwidth.

## Harness

Script:

```text
tools/uml/uml-launcher/scripts/vector2-kcsan-concurrent-traffic.sh
```

Default workload:

```text
runtime.ncpus=4
network.queues=auto
flows=4
tcp_bytes_per_flow=1048576
udp_packets_per_flow=256
udp_payload=512
udp_delay_us=1000
ready_hold_secs=300
backend=seccomp
network.driver=vector2
network.host_mode=auto
```

The helper also accepts `UML_VECTOR2_KCSAN_NCPUS` and
`UML_VECTOR2_KCSAN_QUEUES=N|auto`, so the same gate can be reused for
fixed queue counts, different vCPU counts, and higher flow counts
without hand-editing the generated Umlfile.

The script:

- starts host TCP and UDP sinks for guest-to-host traffic;
- generates a temporary Umlfile using vector2 fd handoff;
- boots UML with the configured queue spec, defaulting to
  `queues = "auto"` and four inherited TAP fds;
- runs guest TCP and UDP senders concurrently across the configured
  vCPU count;
- starts guest TCP and UDP sinks and sends host-to-guest traffic;
- captures `ethtool -S vec2.0`;
- requires TX and RX queue counters to move on at least two queues;
- tears the instance down and checks that the TAP and UML process are gone.

The default UDP send path is paced at 1 ms per datagram.  This keeps the
gate focused on vector2/KCSAN behavior instead of userspace UDP receive
buffer loss under an instrumented UML guest.

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
rm -rf /tmp/um-vector2-kcsan-traffic
tools/uml/uml-launcher/scripts/vector2-kcsan-concurrent-traffic.sh \
  --kernel /home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux \
  --out /tmp/um-vector2-kcsan-traffic
```

Runtime vector2 shape from `umlctl up`:

```text
driver=vector2
guest_dev=vec2.0
tap=v2kcstraffic0
transport=fd
host_mode=fd
queues=4
queue_spec=auto
inherited fds=200..203
```

## Result

Summary:

```text
VECTOR2_KCSAN_CONCURRENT_TRAFFIC_SUMMARY
kernel=/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux
backend=seccomp
ncpus=4
queue_spec=auto
flows=4
tcp_bytes_per_flow=1048576
udp_packets_per_flow=256
udp_payload=512
udp_delay_us=1000
ready_hold_secs=300
HOST_G2H_TCP_DONE connections=4 bytes=4194304 expected=4194304
HOST_G2H_UDP_DONE packets=1024 unique=1024 bytes=524288 expected_packets=1024 expected_bytes=524288
HOST_H2G_SEND_DONE flows=4 tcp_bytes=4194304 udp_packets=1024 seconds=3.275178
VECTOR2_KCSAN_G2H_GUEST_OK flows=4 tcp_bytes=4194304 udp_packets=1024 seconds=15.170087
VECTOR2_KCSAN_H2G_GUEST_DONE tcp_connections=4 tcp_bytes=4194304 expected_tcp=4194304 udp_packets=1024 expected_udp_packets=1024 udp_bytes=524288 expected_udp_bytes=524288
VECTOR2_KCSAN_H2G_GUEST_OK
VECTOR2_KCSAN_QUEUE_TX values=1815,465,1799,1589 nonzero=4
VECTOR2_KCSAN_QUEUE_RX values=3040,726,1183,1788 nonzero=4
VECTOR2_KCSAN_QUEUE_DISTRIBUTION_OK
VECTOR2_KCSAN_TRAFFIC_OK
TAP_ABSENT
UML_PROCESS_ABSENT
```

The captured logs were scanned for:

```text
WARNING
kernel BUG
BUG:
Kernel panic
BUG: KCSAN
data-race
KCSAN:
not ok
FAILED
VECTOR2_KCSAN_.*FAIL
phase .* failed
```

The scan returned no matches.

## Repeat Run

The same default harness was then repeated three more times:

```sh
for i in 1 2 3; do
  out="/tmp/um-vector2-kcsan-traffic-repeat/run-$i"
  tools/uml/uml-launcher/scripts/vector2-kcsan-concurrent-traffic.sh \
    --kernel /home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux \
    --out "$out" >"/tmp/um-vector2-kcsan-traffic-repeat/run-$i.summary"
done
```

All three repeat runs passed with exact TCP byte counts, exact UDP
packet/byte counts, all four TX queues non-zero, all four RX queues
non-zero, `VECTOR2_KCSAN_TRAFFIC_OK`, `TAP_ABSENT`, and
`UML_PROCESS_ABSENT`.

Queue distribution summaries:

```text
run-1 TX=1898,371,1071,1798 RX=2126,1212,1496,1433
run-2 TX=1123,1556,1892,649 RX=1718,1587,2286,667
run-3 TX=1424,614,1291,1118 RX=850,1735,2005,1113
```

The repeat logs and summaries were scanned for the same warning, BUG,
panic, KCSAN, data-race, failure, failed-phase, `not ok`, and `FAILED`
signatures.  The scan returned no matches.

## Varied Queue And Flow Profiles

The harness was then extended to generate non-default queue and vCPU
counts through environment variables.  Syntax and help validation
passed:

```sh
bash -n tools/uml/uml-launcher/scripts/vector2-kcsan-concurrent-traffic.sh
tools/uml/uml-launcher/scripts/vector2-kcsan-concurrent-traffic.sh --help
```

Fixed two-queue profile:

```sh
rm -rf /tmp/um-vector2-kcsan-varied-q2-f6
UML_VECTOR2_KCSAN_NCPUS=2 \
UML_VECTOR2_KCSAN_QUEUES=2 \
UML_VECTOR2_KCSAN_FLOWS=6 \
UML_VECTOR2_KCSAN_TCP_BYTES=524288 \
UML_VECTOR2_KCSAN_UDP_PACKETS=128 \
UML_VECTOR2_KCSAN_READY_HOLD_SECS=5 \
UML_VECTOR2_KCSAN_TIMEOUT=420 \
UML_VECTOR2_KCSAN_WAIT_TIMEOUT=300 \
tools/uml/uml-launcher/scripts/vector2-kcsan-concurrent-traffic.sh \
  --kernel /home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux \
  --out /tmp/um-vector2-kcsan-varied-q2-f6
```

Result:

```text
VECTOR2_KCSAN_CONCURRENT_TRAFFIC_SUMMARY
kernel=/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux
backend=seccomp
ncpus=2
queue_spec=2
flows=6
tcp_bytes_per_flow=524288
udp_packets_per_flow=128
udp_payload=512
udp_delay_us=1000
ready_hold_secs=5
HOST_G2H_TCP_DONE connections=6 bytes=3145728 expected=3145728
HOST_G2H_UDP_DONE packets=768 unique=768 bytes=393216 expected_packets=768 expected_bytes=393216
HOST_H2G_SEND_DONE flows=6 tcp_bytes=3145728 udp_packets=768 seconds=1.262317
VECTOR2_KCSAN_G2H_GUEST_OK flows=6 tcp_bytes=3145728 udp_packets=768 seconds=15.211839
VECTOR2_KCSAN_H2G_GUEST_DONE tcp_connections=6 tcp_bytes=3145728 expected_tcp=3145728 udp_packets=768 expected_udp_packets=768 udp_bytes=393216 expected_udp_bytes=393216
VECTOR2_KCSAN_H2G_GUEST_OK
VECTOR2_KCSAN_QUEUE_TX values=1792,1530 nonzero=2
VECTOR2_KCSAN_QUEUE_RX values=3170,1562 nonzero=2
VECTOR2_KCSAN_QUEUE_DISTRIBUTION_OK
VECTOR2_KCSAN_TRAFFIC_OK
TAP_ABSENT
UML_PROCESS_ABSENT
```

The runtime logs and summary were scanned for warning, BUG, panic,
KCSAN, data-race, failed-phase, `not ok`, `FAILED`, and
`VECTOR2_KCSAN_.*FAIL` signatures.  The scan returned no matches.

Heavier four-queue profile:

```sh
rm -rf /tmp/um-vector2-kcsan-varied-q4-f8-paced
UML_VECTOR2_KCSAN_NCPUS=4 \
UML_VECTOR2_KCSAN_QUEUES=auto \
UML_VECTOR2_KCSAN_FLOWS=8 \
UML_VECTOR2_KCSAN_TCP_BYTES=2097152 \
UML_VECTOR2_KCSAN_UDP_PACKETS=256 \
UML_VECTOR2_KCSAN_UDP_DELAY_US=5000 \
UML_VECTOR2_KCSAN_READY_HOLD_SECS=5 \
UML_VECTOR2_KCSAN_TIMEOUT=600 \
UML_VECTOR2_KCSAN_WAIT_TIMEOUT=420 \
tools/uml/uml-launcher/scripts/vector2-kcsan-concurrent-traffic.sh \
  --kernel /home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux \
  --out /tmp/um-vector2-kcsan-varied-q4-f8-paced
```

Result:

```text
VECTOR2_KCSAN_CONCURRENT_TRAFFIC_SUMMARY
kernel=/home/mjbommar/projects/personal/.build/um-vector-r8c-kcsan/linux
backend=seccomp
ncpus=4
queue_spec=auto
flows=8
tcp_bytes_per_flow=2097152
udp_packets_per_flow=256
udp_payload=512
udp_delay_us=5000
ready_hold_secs=5
HOST_G2H_TCP_DONE connections=8 bytes=16777216 expected=16777216
HOST_G2H_UDP_DONE packets=2048 unique=2048 bytes=1048576 expected_packets=2048 expected_bytes=1048576
HOST_H2G_SEND_DONE flows=8 tcp_bytes=16777216 udp_packets=2048 seconds=7.754025
VECTOR2_KCSAN_G2H_GUEST_OK flows=8 tcp_bytes=16777216 udp_packets=2048 seconds=48.247344
VECTOR2_KCSAN_H2G_GUEST_DONE tcp_connections=8 tcp_bytes=16777216 expected_tcp=16777216 udp_packets=2048 expected_udp_packets=2048 udp_bytes=1048576 expected_udp_bytes=1048576
VECTOR2_KCSAN_H2G_GUEST_OK
VECTOR2_KCSAN_QUEUE_TX values=5798,3303,2893,3867 nonzero=4
VECTOR2_KCSAN_QUEUE_RX values=8149,3720,6256,4169 nonzero=4
VECTOR2_KCSAN_QUEUE_DISTRIBUTION_OK
VECTOR2_KCSAN_TRAFFIC_OK
TAP_ABSENT
UML_PROCESS_ABSENT
```

The same restricted scan over runtime logs and summary returned no
matches.  TAP and UML process cleanup were clean.

An unpaced variant of the heavy profile with eight flows and 4096
host-to-guest UDP packets did not provide pass evidence: TCP completed
exactly in both directions, guest-to-host UDP completed exactly, but
the guest received only 3247 of 4096 host-to-guest UDP packets before
`VECTOR2_KCSAN_H2G_GUEST_FAIL`.  The runtime logs showed no warning,
panic, BUG, KCSAN, or data-race signatures.  This is kept as a workload
bound: under KCSAN, the host-to-guest UDP side needs pacing at this
flow count if exact packet accounting is required.

## What This Closes

This checkpoint closes the specific "no concurrent TCP/UDP KCSAN
traffic" gap from the previous audit.  It also records the first
KCSAN-era queue distribution profile where all four vector2 TX queues
and all four vector2 RX queues moved during concurrent traffic, plus
three consecutive repeat passes of the same gate.  The follow-up varied
profiles add fixed two-queue/two-vCPU/six-flow evidence and a paced
eight-flow/four-queue profile with larger TCP and UDP volumes.

## What Remains Open

The broader multiqueue validation gate still needs:

- longer SMP traffic durations;
- additional queue/CPU/flow profiles beyond the initial varied
  seccomp/KCSAN samples above;
- comparison on additional hosts and kernels;
- kvm-v2 reruns after the separate kvm-v2 baseline blocker is fixed;
- final performance disposition for the measured guest-to-host
  regression.
