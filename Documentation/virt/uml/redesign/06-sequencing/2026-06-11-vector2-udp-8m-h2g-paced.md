# Vector2 Paced 8 MiB UDP Host-To-Guest Refresh

Date: 2026-06-11
Branch: `next`

## Purpose

This note follows the buffered unpaced UDP refresh.  The unpaced 8 MiB
host-to-guest UDP cell still lost datagrams before exact-byte completion for
both legacy vector and vector2.  This slice checks whether a modest sender
pace turns the same larger host-to-guest transfer into exact-byte completion
and whether vector2 remains close to legacy vector under that shape.

## Command

```sh
rm -rf /tmp/um-vector-perf-udp-8m-h2g-pace20
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-udp-8m-h2g-pace20 \
UML_VECTOR_PERF_DRIVERS=vector,vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_PROTOCOL=udp \
UML_VECTOR_PERF_BYTES_LIST=8388608 \
UML_VECTOR_PERF_REPEAT=1 \
UML_VECTOR_PERF_PORT=19175 \
UML_VECTOR_PERF_UDP_PACE_USEC=20 \
  timeout 900s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

## Result

The paced 8 MiB host-to-guest UDP cell passed for both drivers.

```text
driver   direction       guest_mib_s  host_mib_s  guest_cpu_s  host_cpu_s
vector   host-to-guest   17.794       17.781      0.330000     0.051718
vector2  host-to-guest   17.792       17.761      0.160000     0.050644
```

`comparison.tsv` reported:

```text
host_mib_s_median_ratio                         0.998875
guest_mib_s_median_ratio                        0.999888
uml_system_cpu_seconds_median_ratio             0.944444
uml_sched_run_seconds_median_ratio              0.987265
uml_sched_pcount_delta_median_ratio             1.000000
uml_voluntary_ctxt_switches_delta_median_ratio  0.999795
```

Cleanup checks found no leftover `vperf-*` TAP devices and no matching
UML/`umlctl` processes.

## Current Reading

The larger UDP host-to-guest story is now split:

- unpaced 8 MiB host-to-guest still loses datagrams before exact-byte
  completion for both drivers;
- paced 8 MiB host-to-guest with `UML_VECTOR_PERF_UDP_PACE_USEC=20` completes
  exactly for both drivers and shows vector2 at practical parity with legacy
  vector on throughput and scheduler deltas.

This is useful publication evidence for a paced UDP shape, but it does not by
itself prove unpaced large host-to-guest UDP readiness.  The final publication
matrix should either keep separate paced and unpaced UDP acceptance rows or
explicitly document why exact-byte unpaced UDP is not the acceptance shape for
this lossy datagram test.
