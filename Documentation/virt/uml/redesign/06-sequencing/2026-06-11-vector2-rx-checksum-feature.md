# UML vector2 RX Checksum Feature Alignment

Date: 2026-06-11

Branch target: `next`

## Purpose

Align vector2's netdev feature advertisement with the vnet-header datapath.
The driver already consumes `struct virtio_net_hdr` on TAP and inherited-fd
receive paths through `virtio_net_hdr_to_skb()`, but it advertised only TX
checksum offload when `csum=1`.  Legacy vector advertises RX checksum support
for vnet-header TAP, and the fixed-byte diagnostics showed vector2 reporting
`rx-checksumming: off [fixed]` while `vnet_hdr_enabled: 1`.

## Change

- Add `NETIF_F_RXCSUM` to vector2 `dev->features` when `csum=1`.
- Keep `NETIF_F_RXCSUM` out of `dev->hw_features`, so ethtool reports it as a
  fixed feature, matching the legacy vector vnet-header behavior.
- Add KUnit coverage for the feature policy:
  - default config keeps RX checksum and HW checksum off;
  - `csum=1` enables HW TX checksum and fixed RX checksum; and
  - `gso=1,gro=1` still exposes the existing GSO/TSO/GRO features.

## Validation

Build:

```sh
make ARCH=um -j$(nproc)
```

Result: PASS.  The build rebuilt `arch/um/drivers/vector2_netdev.o`,
`arch/um/drivers/vector2_netdev_test.o`, and relinked `./linux`.

KUnit:

```sh
./linux mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt
```

Result: PASS.  `um_vector2_*` reported 89 pass, 0 fail, and 2 trusted-TAP
skips.  The new `vector2_netdev_checksum_features_follow_config_test` passed.

Focused live smokes:

```sh
UML_KERNEL=$PWD/linux \
  tools/testing/selftests/um/vector2-fd-handoff-smoke/run-vector2-fd-handoff-smoke.sh

UML_KERNEL=$PWD/linux \
  tools/testing/selftests/um/vector2-fd-multiqueue-smoke/run-vector2-fd-multiqueue-smoke.sh

UML_KERNEL=$PWD/linux \
  tools/testing/selftests/um/vector2-inproc-tap-smoke/run-vector2-inproc-tap-smoke.sh
```

Result: PASS for fd handoff, fd multiqueue, and trusted in-process TAP.

Fixed-byte host-to-guest diagnostic:

```sh
rm -rf /tmp/um-vector-perf-h2g-rxcsum
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-h2g-rxcsum \
UML_VECTOR_PERF_DRIVERS=vector,vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=2 \
UML_VECTOR_PERF_PORT=19120 \
  timeout 900s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Result: PASS as a diagnostic run.  Vector2 now reports
`rx-checksumming: on [fixed]`, `tx-checksumming: on`, GSO/TSO/GRO enabled, and
`vnet_hdr_enabled: 1`.  The small-transfer performance gap remains open:

```text
driver   repeat  host_mib_s
vector   1       1.574
vector   2       0.342
vector2  1       0.952
vector2  2       0.893
```

Normal guest-to-host TCP gate:

```sh
rm -rf /tmp/um-vector-netbench-rxcsum
OUT=/tmp/um-vector-netbench-rxcsum \
BENCH_PORT=5320 \
TAP=tcpbench-rxcsum \
KERNEL=$PWD/linux \
  timeout 900s \
  tools/testing/selftests/um/net-bench/run-tcp-throughput-via-umlctl.sh \
    --duration 8 --reps 3
```

Result: PASS.  Legacy vector median was 39854.4 Mbps, vector2 median was
39441.7 Mbps, and the vector2/legacy ratio was 0.990 against the 0.85 gate.

## Disposition

This closes the RX checksum feature-advertisement mismatch and keeps the normal
TCP publication gate green.  It does not close P4.3: the 1 MiB host-to-guest
fixed-byte cell is still below legacy, and UDP, syscall-rate, CPU-utilisation,
longer multiqueue fairness, the natural seccomp long run, and full KVM-v2
Tier 3 networking remain open.
