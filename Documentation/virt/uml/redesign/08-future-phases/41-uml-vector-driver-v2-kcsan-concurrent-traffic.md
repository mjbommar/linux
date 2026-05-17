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
flows=4
tcp_bytes_per_flow=1048576
udp_packets_per_flow=256
udp_payload=512
udp_delay_us=1000
ready_hold_secs=300
backend=seccomp
network.driver=vector2
network.host_mode=auto
network.queues=auto
runtime.ncpus=4
```

The script:

- starts host TCP and UDP sinks for guest-to-host traffic;
- generates a temporary Umlfile using vector2 fd handoff;
- boots UML with `queues = "auto"`, resolving to four inherited TAP fds;
- runs guest TCP and UDP senders concurrently across four vCPUs;
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

## What This Closes

This checkpoint closes the specific "no concurrent TCP/UDP KCSAN
traffic" gap from the previous audit.  It also records the first
KCSAN-era queue distribution profile where all four vector2 TX queues
and all four vector2 RX queues moved during concurrent traffic, plus
three consecutive repeat passes of the same gate.

## What Remains Open

The broader multiqueue validation gate still needs:

- longer SMP traffic durations;
- varied queue/CPU counts and flow counts;
- comparison on additional hosts and kernels;
- kvm-v2 reruns after the separate kvm-v2 baseline blocker is fixed;
- final performance disposition for the measured guest-to-host
  regression.
