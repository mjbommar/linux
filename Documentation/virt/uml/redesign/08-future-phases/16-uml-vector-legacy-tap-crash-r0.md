# uml-vector legacy TAP open crash R0 note

**Status:** R0 implementation note.
**Date:** 2026-05-17.

This note records the legacy-driver fix required by the vector v2
buildout plan before the v2 runtime driver can be used as a meaningful
replacement target.

## Reproducer

The Tier 3 Django load-test failure used a legacy vector TAP device:

```text
vec0:transport=tap,ifname=soak-tap0,depth=128
```

The failure is reproduced by booting a UML kernel with that `vec0`
argument and bringing the device up:

```text
ip link set vec0 up
```

The observed splat was a kernel-mode fault at address `0x18` in
`vector_net_open()`.  Symbol lookup against the failing image resolved
the fault to `vector_reset_stats()` taking
`vp->rx_queue->head_lock`.

## Root Cause

The legacy vector driver supports both vectorized and non-vectorized
I/O paths.  TAP devices use the non-vectorized path by default:

- `get_transport_options()` returns no `VECTOR_RX` or `VECTOR_TX` bit
  for `transport=tap`;
- `vector_net_open()` allocates `header_rxbuffer` and
  `header_txbuffer` for that path;
- no `rx_queue` or `tx_queue` is allocated.

Several call sites nevertheless assumed both queues existed:

- `vector_reset_stats()`;
- `vector_poll()`;
- ethtool ring queries;
- ethtool stats snapshots.

The immediate NULL dereference was in `vector_reset_stats()`, but
fixing only that site would leave the next scheduled NAPI poll able to
hit the same class of crash.

## Fix

The R0 fix keeps the old driver behavior and makes queue ownership
explicit at the legacy/vector boundary:

- queue locks are taken only when the matching queue exists;
- legacy RX runs without dereferencing `rx_queue`;
- ethtool ring parameters report zero pending entries when a queue is
  absent;
- ethtool stats snapshots tolerate stopped and non-vector TAP devices;
- failed opens check queue allocation before using the queue;
- `vector_net_close()` nulls queue and buffer pointers after freeing;
- NAPI teardown runs only after NAPI was actually added.

This is a containment fix for the legacy driver.  It does not make
vector v2 production-ready and must not be counted as v2 runtime-driver
progress beyond the R0 baseline gate.

## Validation

Required validation for this R0 change:

```text
git diff --check
make ARCH=um O=/tmp/um-vector-r0 defconfig
scripts/config --file /tmp/um-vector-r0/.config -e UML_NET_VECTOR
make ARCH=um O=/tmp/um-vector-r0 olddefconfig
make ARCH=um O=/tmp/um-vector-r0 -j$(nproc)
```

Manual runtime validation should then use the same TAP command line as
the failure:

```text
/tmp/um-vector-r0/linux \
  mem=256M \
  vec0:transport=tap,ifname=soak-tap0,depth=128
```

Inside the guest:

```text
ip link set vec0 up
ip link set vec0 down
```

The expected result is no splat on open, poll, ethtool stats, or close.
This is the baseline from which the vector v2 runtime driver should be
compared.

## Validation Run

2026-05-17 validation against this patch:

```text
git diff --check
git diff | scripts/checkpatch.pl --strict -
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r0 defconfig
scripts/config --file /home/mjbommar/projects/personal/.build/um-vector-r0/.config -e UML_NET_VECTOR
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r0 olddefconfig
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r0 -j$(nproc)
```

The full UML build completed and produced:

```text
/home/mjbommar/projects/personal/.build/um-vector-r0/linux
```

Manual open/close smoke used:

```text
/home/mjbommar/projects/personal/.build/um-vector-r0/linux \
  mem=256M noreboot con=fd:0,fd:1 ssl=null \
  root=/dev/root rootfstype=hostfs rootflags=/ rw \
  vec0:transport=tap,ifname=soak-tap0,depth=128 \
  init=/tmp/uml-vector-r0-init.sh loglevel=7
```

Guest-side results:

```text
VECTOR_R0_SHOW_RC=0
VECTOR_R0_UP_RC=0
VECTOR_R0_DETAIL_RC=0
VECTOR_R0_ETHTOOL_STATS_RC=0
VECTOR_R0_ETHTOOL_RING_RC=0
VECTOR_R0_DOWN_RC=0
```

The boot log had no `Kernel panic`, `BUG:`, `Oops`,
`Kernel mode fault`, `general protection fault`, or `NULL pointer`
signature.
