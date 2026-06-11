# UML vector2 fd vnet RX frame length

Status: correctness fix on `next`; vector2 publication gates remain open.
Date: 2026-06-11.
Tree: `next`.

## Purpose

The inherited-fd backend probes each launcher-owned TAP fd with `TUNGETIFF` and
records whether the live channel is using `IFF_VNET_HDR`.  The RX allocation
path still derived its buffer size from the configured transport enum, so
`transport=fd` allocated raw Ethernet frame space even when the inherited fd
was a vnet-header TAP.

That was the wrong ownership boundary.  Framing is a channel runtime property,
not a transport-name property.

## What Changed

- Added `um_vec2_rx_frame_len()` as the netdev RX allocation helper.
- Made NAPI RX allocation use `channel->vnet_hdr`.
- Added `vector2_netdev_rx_frame_len_follows_channel_test`, proving that:
  - raw fd channels allocate the raw frame length;
  - vnet-header fd channels allocate raw frame length plus
    `sizeof(struct virtio_net_hdr)`; and
  - the decision does not silently follow the configured transport enum.

## Validation

Commands run:

```sh
git diff --check
make ARCH=um -j$(nproc)
./linux mem=256M kunit.filter_glob='um_vector2_*' kunit_shutdown=halt

KERNEL=$PWD/linux \
  timeout 300s \
  tools/testing/selftests/um/vector2-fd-handoff-smoke/run-vector2-fd-handoff-smoke.sh
KERNEL=$PWD/linux \
  timeout 300s \
  tools/testing/selftests/um/vector2-fd-multiqueue-smoke/run-vector2-fd-multiqueue-smoke.sh
KERNEL=$PWD/linux \
  timeout 300s \
  tools/testing/selftests/um/vector2-inproc-tap-smoke/run-vector2-inproc-tap-smoke.sh
```

Results:

- `git diff --check`: PASS.
- `make ARCH=um -j$(nproc)`: PASS.
- `um_vector2_*` KUnit: 90 pass, 0 fail, 2 trusted-TAP skips.
- `vector2-fd-handoff-smoke`: PASS.
- `vector2-fd-multiqueue-smoke`: PASS.
- `vector2-inproc-tap-smoke`: PASS.

Fixed-byte host-to-guest diagnostic:

```sh
rm -rf /tmp/um-vector-perf-h2g-frame-len
UML_VECTOR_PERF_OUT=/tmp/um-vector-perf-h2g-frame-len \
UML_VECTOR_PERF_DRIVERS=vector,vector2 \
UML_VECTOR_PERF_DIRECTION=host-to-guest \
UML_VECTOR_PERF_BYTES_LIST=1048576 \
UML_VECTOR_PERF_REPEAT=3 \
UML_VECTOR_PERF_PORT=19134 \
  timeout 900s tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel "$PWD/linux"
```

Host-side MiB/s:

| Driver | Repeats | Median | Best |
| ------ | ------- | ------ | ---- |
| vector | 1.223, 0.976, 0.395 | 0.976 | 1.223 |
| vector2 | 0.890, 0.895, 1.097 | 0.895 | 1.097 |

Before landing the fix, two rejected diagnostic probes also narrowed the
small-transfer issue:

- A prototype that rescheduled NAPI after every productive RX poll regressed
  the target case and was reverted.
- Clean `queues=1` versus `queues=auto` vector2 reruns stayed in the same
  performance band, so queue count alone is not the main 1 MiB host-to-guest
  explanation.

## Disposition

This closes a concrete fd/vnet RX allocation correctness bug.  It does not
close the vector2 replacement-readiness work: the 1 MiB host-to-guest
fixed-byte cell still needs follow-up, and UDP, syscall-rate, CPU-utilisation,
multiqueue fairness, the natural seccomp long run, and full KVM-v2 Tier 3
networking remain open.
