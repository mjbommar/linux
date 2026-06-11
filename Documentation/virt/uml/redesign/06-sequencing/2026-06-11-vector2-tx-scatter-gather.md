# UML vector2 TX scatter-gather fix

Status: landed implementation candidate; TCP performance gate improved but still
failing.
Date: 2026-06-11.
Tree: `next`.

## Problem

The first current-head TCP `net-bench` run after harness cleanup showed vector2
functionally passed all iterations but failed guest-to-host throughput parity:

- legacy vector median: 39805.4 Mbps;
- vector2 median: 18949.8 Mbps;
- ratio: 0.476, below the 0.85 gate.

Code review found one concrete difference from normal legacy vector TX:
vector2 linearized each skb and copied a virtio-net header into the skb head
before writing to the host TAP fd.  Legacy vector builds a scatter-gather iovec
from the virtio header, skb linear data, and skb frags, then submits that frame
without forcing a linear skb.

## Change

- Added `um_vec2_write_skb()` as a shared runtime helper.
- The helper builds an iovec from:
  - optional `struct virtio_net_hdr`;
  - skb linear data; and
  - skb page frags.
- Switched the inherited-fd and trusted-TAP backends to the shared helper.
- Removed the unconditional TX `skb_linearize()` from the fd backend.
- Removed the TAP backend's `skb_cow_head()` / `skb_linearize()` /
  `skb_push()` header-copy path.
- Added fd and TAP KUnit coverage that sends a fragmented skb and asserts the
  skb still has a fragment at TX completion time.

## Validation

Build:

```sh
make ARCH=um -j$(nproc)
```

KUnit:

```sh
timeout 180s ./linux mem=256M kunit.filter_glob='um_vector2_*' \
        kunit_shutdown=halt >/tmp/um-vector2-sg-kunit.log 2>&1
python3 tools/testing/kunit/kunit.py parse /tmp/um-vector2-sg-kunit.log
```

Result:

- `um_vector2_*`: 86 pass, 0 fail, 2 trusted-TAP skips, 88 total.
- New `vector2_fd_tx_batch_preserves_frags_test`: PASS.
- New `vector2_tap_tx_batch_preserves_frags_test`: PASS.

TCP gate rerun:

```sh
OUT=/tmp/uml-net-bench-run-sg-1781174499 \
KERNEL=$PWD/linux \
tools/testing/selftests/um/net-bench/run-tcp-throughput-via-umlctl.sh \
        --duration 8 \
        --reps 3
```

| Driver | Per-rep Mbps | Median Mbps |
| ------ | ------------ | ----------- |
| legacy vector | 40956.4, 40041.7, 38891.1 | 40041.7 |
| vector2 | 21846.1, 25015.6, 22953.7 | 22953.7 |

The ratio improved from 0.476 to 0.573, but the gate still fails:

```text
VERDICT: FAIL
```

## Remaining Work

The remaining gap is no longer explained by forced skb linearization alone.
Legacy vector's default TAP TX path uses `sendmmsg()` to submit batches of
prepared messages, while vector2 still submits one TAP packet per `writev()`.

The next implementation slice should add bounded vector2 TX batching for TAP/fd
using the same scatter-gather message shape, then rerun:

- `um_vector2_*` KUnit;
- `vector2-fd-handoff-smoke`;
- `vector2-fd-multiqueue-smoke`;
- TCP `net-bench`; and
- checkpatch/whitespace.
