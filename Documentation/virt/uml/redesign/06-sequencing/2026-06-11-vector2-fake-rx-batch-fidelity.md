# UML vector2 fake RX batch fidelity

Date: 2026-06-11

Tree: `next`

## Purpose

The production vector2 fd and TAP RX backends now prepare one skb per
nonblocking read attempt.  That lazy path reduced real RX-slot churn in the
1 MiB host-to-guest diagnostic, but the KUnit fake host still prepared the
entire NAPI budget before deciding how many packets were available.

That mismatch made the fake-host and netdev tests less representative of the
path being tuned for publication.  This slice aligns the fake host with the
production RX shape so future KUnit changes exercise the same allocation and
release pattern as fd/TAP.

## What Changed

`arch/um/drivers/vector2_fake_host.c` now:

- calls `um_vec2_rx_batch_prepare_next()` once per attempted read;
- consumes at most one queued fake packet per prepared slot;
- releases exactly one speculative prepared slot when the fake backend reaches
  its read limit or sees no queued packet;
- preserves `rx_empty` as a zero-packet batch counter; and
- keeps partial progress as the return value if a later prepare attempt fails.

`arch/um/drivers/vector2_fake_host_test.c` now checks the lazy shape directly:

- two queued packets with budget four produce three prepared slots, two
  received slots, and one released speculative slot;
- an empty poll prepares and releases one slot, not the full budget;
- a one-packet read limit leaves the second queued packet in the fake backend
  and releases one speculative probe slot;
- allocation failure at the first slot fails closed without invoking the
  release callback; and
- a budget larger than the RX batch depth is rejected before any queued packet
  or allocation state is consumed.

## Validation

Build and whitespace:

```sh
make ARCH=um -j$(nproc)
git diff --check
```

Focused KUnit:

```sh
timeout --kill-after=10s 180s ./linux mem=256M \
  kunit.filter_glob='um_vector2_fake_host' kunit_shutdown=halt
```

Result:

```text
um_vector2_fake_host: pass:10 fail:0 skip:0 total:10
```

Broader vector2 KUnit:

```sh
timeout --kill-after=10s 240s ./linux mem=256M \
  kunit.filter_glob='um_vector2_*' kunit_shutdown=halt
```

Result:

```text
um_vector2_*: pass:94 fail:0 skip:2 total:96
```

The two skips are the existing trusted-TAP host-open cases in
`um_vector2_host_tap`.

## Remaining Work

This is a test-fidelity slice, not a performance closure.  It protects the
lazy RX behavior used by the current fd/TAP datapaths, but the 1 MiB
host-to-guest no-regression cell remains open.  The next vector2 performance
work should continue from the existing fixed-byte aggregate/comparison output
and the UML process scheduler metrics.
