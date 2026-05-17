# UML vector driver v2 — audit risks R3 / R4 + P3.3 resolution

**Status:** decisions recorded; no code changes required for R3/R4.
**Date:** 2026-05-17.
**Companion to:** `46-uml-vector-driver-v2-code-audit-2026-05-17.md`
§R3, §R4, §P3.3.

## R3 — `start_xmit` lifecycle check is unlocked

### Concern

`um_vec2_netdev_start_xmit()` reads `vdev->life.state` via
`um_vec2_dev_can_xmit(&vdev->life)` without taking `vdev->lock`.  A
parallel `ndo_stop` transitions the lifecycle from `RUNNING` ->
`QUIESCING` -> `REGISTERED` and tears down channels.  If the two
race, `start_xmit` could deref freed channel state.

### Resolution

**Safe by netdev framework invariant.**

The `ndo_stop` path in v2 is `um_vec2_netdev_stop`, invoked through
`netdev_ops`.  The kernel netdev framework guarantees that before
`ndo_stop` is called, `netif_tx_disable()` has:

  - cleared `__LINK_STATE_START` on the device,
  - waited (via `synchronize_net`) for any in-flight
    `dev_queue_xmit` -> `ndo_start_xmit` callsite to drain.

By the time `um_vec2_netdev_stop` runs, no `start_xmit` can be in
flight, and no new one can start, because the framework gates xmit
on `__LINK_STATE_START`.  This is the same contract every
in-tree netdev relies on; the unlocked lifecycle check in `start_xmit`
is safe under that contract.

### Risk surface

If a future contributor adds a lifecycle transition path *outside*
`netdev_ops` dispatch (e.g. a direct debugfs hook that flips
state without taking `vdev->lock` and without `netif_tx_disable`),
the framework invariant is broken and the race becomes real.

### Action

  - **Document the invariant** in a comment on `um_vec2_netdev_start_xmit`'s
    lifecycle check.
  - **No locking change** — adding `vdev->lock` would defeat the
    spin_lock_bh fast-path performance with no real-world benefit.

## R4 — `napi_schedule` with `vdev->lock` held during open

### Concern

`um_vec2_netdev_open()` (line 528 takes `vdev->lock`; line 592 calls
`napi_schedule(&vdev->channels[i].napi)` for each channel before
unlocking at line 599).  The audit raised this as a lock-ordering
risk: scheduling NAPI while holding a mutex.

### Resolution

**Safe.**

`napi_schedule()` is non-blocking and does not take any external
locks: it sets `NAPI_STATE_SCHED` and raises a softirq.  The softirq
runs in a separate context (ksoftirqd or the next softirq window)
and executes `um_vec2_netdev_poll`, which never takes `vdev->lock`.
There is no path through `napi_schedule` -> `vdev->lock` so there
is no AB-BA possibility.

The kernel pattern is explicit: holding a mutex across
`napi_schedule` is acceptable, only blocking inside the NAPI poll
function would be a problem.

### Action

  - **Document the safety** in a comment on the napi_schedule loop.
  - **No locking change.**

## P3.3 — `BACKEND_DEAD` recovery decision

### Concern

When the host backend fails fatally (`tx_batch` or `rx_batch`
returns `-ENODEV`), `um_vec2_netdev_poll` increments
`UM_VEC2_STAT_BACKEND_DEAD`, calls `netif_tx_stop_all_queues`, and
`netif_carrier_off`.  But the device stays in `RUNNING` from the
lifecycle model's perspective.  No code path re-tries the backend,
re-enables the queues, or transitions the lifecycle back to a
state where the device can be xmit'd to.

### Decision

**Terminal-state policy: DEAD is terminal, requires
`ip link set vec2.X down && up`.**

Rationale:

  - The backend died because the host fd was closed, the TAP
    interface vanished, or the host signalled `-ENODEV`.
    Auto-recovery would require knowing what changed and
    re-opening; that is a high-complexity feature for a low-value
    case (operator should fix the host condition and reset the
    netdev).
  - `ip link set vec2.X down` runs through `um_vec2_netdev_stop`
    which already handles the "carrier off, queues stopped"
    state via the new B5 fix (idempotent stop), driving
    lifecycle back through QUIESCING -> REGISTERED.
  - `ip link set vec2.X up` then re-opens cleanly.

### Action

  - **Document the policy** in a comment on the `backend_dead`
    label in `um_vec2_netdev_poll`.
  - **No code change** beyond the comment.  The B5 fix (idempotent
    `ndo_stop`) already provides the recovery primitive via
    `ip link set down && up`.

## Code changes landing alongside this memo

A single commit adds the three documentation comments:

  - `vector2_netdev.c::um_vec2_netdev_start_xmit` — R3 invariant.
  - `vector2_netdev.c::um_vec2_netdev_open` napi_schedule loop — R4
    safety.
  - `vector2_netdev.c::um_vec2_netdev_poll` backend_dead label —
    P3.3 terminal-state policy.

No behavior change.

## References

  - `Documentation/virt/uml/redesign/08-future-phases/46-uml-vector-driver-v2-code-audit-2026-05-17.md`
    §R3, §R4, §P3.3.
