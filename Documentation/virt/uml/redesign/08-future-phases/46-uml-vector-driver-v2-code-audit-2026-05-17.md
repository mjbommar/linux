# UML vector driver v2 code-level audit

**Status:** independent code-level read.
**Date:** 2026-05-17.
**Tree:** branch `umlctl-deploy` at `561674bc73a4`.
**Files audited:** `arch/um/drivers/vector2_*.{c,h}` (30 files, ~7155 lines)
plus `Kconfig`/`Makefile` wiring; legacy `vector_kern.c` consulted only for
comparison.

This is a read-only audit of the v2 driver source. It does not paraphrase
`29-uml-vector-driver-v2-completion-audit.md`; it reads the code and reports
what the code does, not what status documents claim.

## 1. TL;DR

The vector2 driver is a coherent rewrite of the legacy UML vector path. The
typed config parser, queue ownership rings, lifecycle state model, transport
header helpers, and host-ops boundary are well factored, individually tested,
and clearly safer than `vector_user.c`'s destructive token splitter and
`vector_kern.c`'s open-coded queue management. Sandbox gating is correctly
expressed in the parser and TAP backend.

However, the integration glue between `vector2_host_fd.c`,
`vector2_host_tap.c`, and `vector2_netdev.c` has real defects:

  - `um_vec2_fd_channel_open()` publishes `vdev->channels` partway through
    a multi-queue loop, and the matching unwind in `um_vec2_fd_open()`
    frees the channel array without clearing `vdev->channels`. The result
    is a use-after-free on the next `um_vec2_fd_close()` walk.
  - The TAP RX batch silently converts `-EPROTO` (malformed frame) into a
    zero return, so `UM_VEC2_STAT_RX_PROTO_DROPS` never increments for TAP.
  - Several `net_device_ops` callbacks (`ndo_tx_timeout`, `ndo_set_features`,
    `ndo_fix_features`, `ndo_set_rx_mode`, `ndo_poll_controller`) are missing
    even though `watchdog_timeo` and per-channel NAPI suggest they were
    intended.

None of these defects are exercised by the existing KUnit suites. The
`vector_net_open+0x3a3` legacy NULL deref is provably gone (`ndo_open`
checks lifecycle, holds `vdev->lock`, and never dereferences `vp->fds`
without a backend channel allocation); v2 still has a credible UAF in a
different open path. Swap-readiness for FD transport is plausible if the
fd-handoff path is single-queue; for multi-queue inherited FDs the
above UAF should be fixed first.

## 2. Bugs found

### B1. Use-after-free in multi-queue FD open unwind  [HIGH]

`arch/um/drivers/vector2_host_fd.c:249-250` and
`arch/um/drivers/vector2_host_fd.c:340-344`.

`um_vec2_fd_channel_open()` ends with:

	vdev->channels = channel;
	vdev->num_channels = 1;

`channel` here is the per-iteration element `&channels[i]` passed in by the
caller, not the array base. `um_vec2_fd_open()` then continues the loop:

	for (i = 0; i < queues; i++) {
		ret = um_vec2_fd_channel_open(vdev, &channels[i], i,
					      vdev->cfg.fd + i);
		if (ret)
			goto out_close_channels;
	}
	vdev->channels = channels;
	vdev->num_channels = queues;
	return 0;

	out_close_channels:
		while (i--)
			um_vec2_fd_channel_close(&channels[i], vdev->netdev);
		kfree(channels);
		return ret;

If iteration 0 succeeds and iteration 1 fails (for example
`kzalloc(fdhost)` or `os_dup_file()` returns an error), the post-iter-0
write has set `vdev->channels = &channels[0]; vdev->num_channels = 1`.
The unwind closes channel 0, frees the `channels` array, and returns the
error - but it never clears `vdev->channels`.

`um_vec2_netdev_open()` then enters its open-failure branch:

	ret = um_vec2_open_backend(vdev);
	if (ret) {
		...
		if (um_vec2_unwind_open(vdev))
			netdev_err(dev, "vector v2 open unwind failed\n");
		goto out;
	}

`um_vec2_unwind_open()` calls `um_vec2_close_backend()` which calls
`um_vec2_fd_close()`. The latter iterates a freed array:

	struct um_vec2_channel *channels = vdev->channels;
	if (!channels)
		return;
	for (i = 0; i < vdev->num_channels; i++)
		um_vec2_fd_channel_close(&channels[i], vdev->netdev);

This dereferences `channels[0].host` from freed memory and then `kfree`s a
garbage pointer.

The bug is not reachable from the existing
`vector2_fd_multiqueue_missing_second_fd_unwinds_test` because that test
makes validation fail before `um_vec2_fd_channel_open()` is ever called.
It is reachable on production failure injection (slab OOM at
`fdhost`/`queue_pair_alloc`, or `os_set_fd_block` failure) for
`queues >= 2`.

**Fix sketch:** delete lines 249-250 in `um_vec2_fd_channel_open()` and
let only the post-loop assignment in `um_vec2_fd_open()` publish
`vdev->channels`. Optionally also clear `vdev->channels = NULL;
vdev->num_channels = 0;` after `kfree(channels)` in the unwind label.

### B2. TAP RX silently swallows `-EPROTO`  [MEDIUM]

`arch/um/drivers/vector2_host_tap.c:107-117`.

	ret = um_vec2_tap_read_skb(taphost, skb);
	if (!ret)
		break;
	if (ret < 0) {
		if (ret == -EPROTO)
			break;
		goto complete;
	}
	lens[received++] = ret;
	...
	ret = 0;
	complete:
		if (um_vec2_rx_batch_complete(...))
			return -EIO;
		if (received)
			return received;
		return ret == -EAGAIN ? 0 : ret;

When `um_vec2_tap_read_skb()` reports `-EPROTO` (short frame, bad
`virtio_net_hdr_to_skb`), the loop `break`s, then unconditionally executes
`ret = 0` before falling into `complete:`. The function then returns
either `received` or `0` and never returns `-EPROTO`.

`um_vec2_netdev_poll()` only bumps `UM_VEC2_STAT_RX_PROTO_DROPS` and
`dev->stats.rx_dropped` when the backend returns `-EPROTO`
(`vector2_netdev.c:261`). With the current TAP path, that stat is dead.
The FD backend handles `-EPROTO` correctly (`vector2_host_fd.c:107-109`
does `goto complete;` without resetting `ret`).

**Fix sketch:** replace the `break` on `-EPROTO` with `goto complete;` so
the negative `ret` survives into the post-loop disposition, matching the
FD backend. Drop the unconditional `ret = 0;` above `complete:` and
initialize `ret = 0` only when the loop completes normally.

### B3. Unreachable `vde_mode` trusted-host key  [LOW]

`arch/um/drivers/vector2_config.c:214-233` lists
`UM_VEC2_KEY_VDE_MODE` as trusted-host-only. The key id is referenced in
the trusted-host enum and the switch in `um_vec2_parse_value()`
(`vector2_config.c:501-504`), but `um_vec2_key_id()`
(`vector2_config.c:133-211`) has no string mapping for it. There is
no spec syntax that can ever resolve to `UM_VEC2_KEY_VDE_MODE`, so the
trusted-host check and the parse branch are dead code, and the
`cfg->mode_string` field is permanently zeroed.

This is not a security issue (dead code fails closed), but it indicates a
missing feature versus the legacy VDE option set.

**Fix sketch:** add `if (!strcmp(key, "vde_mode")) return
UM_VEC2_KEY_VDE_MODE;` to `um_vec2_key_id()` once VDE support is wired
up; until then, drop the key id and the parse branch to remove dead
state.

### B4. Channel transitions leak on attach-fd failure  [LOW]

`arch/um/drivers/vector2_host_tap.c:241-286`.

In `um_vec2_tap_channel_attach_fd()`:

	channel->rx_fd = fd; channel->tx_fd = fd;
	...
	ret = um_vec2_chan_transition(&channel->life,
				      UM_VEC2_CHAN_FD_ATTACHED);
	if (ret)
		goto out_free_queue;
	...
	out_free_queue:
		channel->host = NULL;
		um_vec2_queue_pair_free(channel, dev);
	out_free_host:
		kfree(taphost);
		return ret;

On transition failure the channel is left in `UM_VEC2_CHAN_ALLOCATED` (or
whatever earlier state) without going through `QUIESCING -> CLOSED`. The
FD backend's `um_vec2_fd_channel_open()` correctly drives QUIESCING/CLOSED
on its failure path (`vector2_host_fd.c:260-264`); the TAP backend does
not.

In practice the channel struct is freed by the caller, so the dangling
state does no harm. But the asymmetry with the FD backend is worth
closing to keep the state model self-consistent.

**Fix sketch:** mirror the FD backend's `um_vec2_chan_can_transition`
clauses before falling into `out_free_host`.

### B5. `ndo_stop` returns `-EINVAL` for non-running states  [LOW]

`arch/um/drivers/vector2_netdev.c:531-553`.

	switch (vdev->life.state) {
	case UM_VEC2_DEV_REGISTERED:
		break;
	case UM_VEC2_DEV_RUNNING:
	case UM_VEC2_DEV_OPENING:
		...
		break;
	default:
		ret = -EINVAL;
		break;
	}

If `vdev->life.state` is `QUIESCING` or `DEAD` when `ndo_stop` is called,
the function returns `-EINVAL`. `ndo_stop` is generally expected to
succeed; non-zero returns are propagated by `__dev_close_many()` and can
confuse `unregister_netdevice()` if the device is being torn down. The
practical impact is small because the only paths that reach `QUIESCING`
all transition back to `REGISTERED` before unlocking. Still, this is
brittle.

**Fix sketch:** treat unexpected states as success (return 0) and leave
diagnostics to `netdev_warn`.

## 3. Risks (less certain - investigate before acting)

### R1. `vdev->netdev` is published after register_netdevice succeeds

`arch/um/drivers/vector2_netdev.c:654-665`.

	rtnl_lock();
	ret = register_netdevice(dev);
	rtnl_unlock();
	if (ret)
		goto out_free_netdev;

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_REGISTERED);
	if (ret)
		goto out_unregister_netdev;

	vdev->netdev = dev;
	vdev->registered_queues = queues;

The netdev is live (visible via `ip link`) before `vdev->netdev = dev` is
assigned. `um_vec2_netdev_open()` does not dereference `vdev->netdev`,
but the FD and TAP open helpers do
(`vector2_host_fd.c:199`, `vector2_host_tap.c:246`). A racing
`ip link set up` between `register_netdevice` returning and the assignment
would derive `vdev` via `netdev_priv` (OK, set in
`um_vec2_netdev_init()`), call `ndo_open` which mutex-locks `vdev`, call
`um_vec2_fd_open()` -> `um_vec2_fd_channel_open()` which reads
`vdev->netdev` (NULL) -> the helper would crash on
`dev->mtu`/`dev->stats` access in `um_vec2_runtime_frame_len()`.

The race window is tiny because the assignments run between `rtnl_unlock`
and the next blocking call, and userspace cannot send netlink in that
gap without preemption. But this is not provably safe.

**Investigate:** is there a kernel guarantee that `ip link` netlink
events cannot reach `ndo_open` before the caller of `register_netdevice`
returns to rest? If not, move `vdev->netdev = dev;` to *before*
`register_netdevice()` and ensure `unregister_netdev` in the failure
path is correct.

### R2. MTU change at runtime is not reflected in backend frame_len

`arch/um/drivers/vector2_host_tap.c:241-272` and
`arch/um/drivers/vector2_host_fd.c:193-243`.

`taphost->frame_len` and `fdhost->frame_len` are computed once during
channel attach via `um_vec2_runtime_frame_len(dev, ...)`. The NAPI
poll's RX skb allocation uses `um_vec2_runtime_frame_len(dev, ...)`
freshly (`vector2_netdev.c:250`), but the host-side `read()` length is
the cached `frame_len`. A live MTU change via `ip link set <dev> mtu N`
will not enlarge the backend read; RX above the cached length will be
truncated by the host read into a too-small buffer.

Legacy `vector_kern.c` also does not provide `ndo_change_mtu`, so this
is not a regression in API surface. But `dev->max_mtu =
UM_VEC2_MAX_MTU` invites the operator to actually change MTU, at which
point v2 silently truncates.

**Investigate:** either provide `ndo_change_mtu` that quiesces/restarts
backends, or cap `dev->max_mtu` to the initial configured MTU until
backend reconfiguration is wired up.

### R3. `start_xmit` lifecycle check is unlocked

`arch/um/drivers/vector2_netdev.c:567`.

	if (!um_vec2_dev_can_xmit(&vdev->life)) { ... }

`vdev->life.state` is read without `vdev->lock`. `um_vec2_netdev_stop()`
writes it under `vdev->lock`. In normal flow `__LINK_STATE_START` and
`netif_tx_disable()` block `start_xmit` once `dev_close()` starts, so
the race is structurally prevented. But if any future caller calls into
`um_vec2_dev_transition()` from outside `ndo_stop` (e.g. on a backend
fatal error), the unlocked read in `start_xmit` could observe a torn
state.

**Investigate:** when the backend-dead path
(`vector2_netdev.c:285-292`) lands an asynchronous transition, ensure
either RCU or a smp_load_acquire ordering on `vdev->life.state` reads
from `start_xmit`.

### R4. `napi_schedule` with `vdev->lock` held during open

`arch/um/drivers/vector2_netdev.c:511-512`.

After transitioning to `RUNNING`, `um_vec2_netdev_open()` does
`napi_schedule()` while still holding `vdev->lock` and before
`mutex_unlock`. NAPI poll runs in softirq context without `vdev->lock`,
so it doesn't deadlock, but if `vdev->lock` ever becomes contended with
softirq context (it shouldn't today; only `ndo_open`/`ndo_stop` and
ethtool take it, all in process context with rtnl held), this would
break. Today this is safe but fragile.

**Investigate:** is there value in dropping `vdev->lock` before
scheduling NAPI? The state is already `RUNNING`; nothing further needs
the lock for the open path.

### R5. Sandbox vs `INPROC` policy depends on a single `IS_ENABLED` gate

`arch/um/drivers/vector2_host_tap.c:355-356`.

	if (!IS_ENABLED(CONFIG_UML_NET_VECTOR_V2_INPROC))
		return -EACCES;

The TAP backend rejects host TAP creation outside trusted in-process
builds. The parser separately rejects trusted-host options outside
trusted builds (`vector2_config.c:611-615`). Defense in depth is good,
but the two gates are independent. If a future commit teaches
`um_vec2_tap_open()` to also be invokable from a not-yet-existing
trusted launcher boundary, the parser gate becomes the only protection.
Worth recording for the future-features review.

### R6. NAPI weight derives from configured ring depth

`arch/um/drivers/vector2_netdev.c:201-204`.

	static unsigned int um_vec2_napi_weight(const struct um_vec2_dev *vdev)
	{
		return min_t(unsigned int, vdev->cfg.depth, UM_VEC2_NAPI_MAX_WEIGHT);
	}

If the operator passes `vec=0` (which forces `depth=1`,
`vector2_config.c:412-413`), the NAPI weight is 1 - effectively
disabling NAPI batching. This is consistent with the user's intent but
worth flagging; conventional drivers use a fixed weight independent of
queue depth.

## 4. Architectural assessment

### Module split

The split is unusually clean for a UML driver:

  - `vector2_config.{c,h}` - typed parser, no kernel-only state.
  - `vector2_queue.{c,h}` - opaque-owner rings, testable without
    `struct sk_buff`.
  - `vector2_transport.{c,h}` - bounds-checked GRE/L2TPv3 header
    helpers, side-effect free.
  - `vector2_model.{c,h}` - explicit device and channel state machines
    with formal `can_transition()` predicates.
  - `vector2_fake_host.{c,h}` - host-ops fake that replaces the legacy
    user-space syscall layer for tests.
  - `vector2_host_fd.c`, `vector2_host_tap.c` - real host backends behind
    the `um_vec2_host_ops` vtable.
  - `vector2_netdev.c` - net_device_ops, NAPI, IRQ wiring.
  - `vector2_runtime.c` - shared TX/RX queue allocators (small).
  - `vector2_core.c` - command-line aggregation and validation.
  - `vector2_cmdline.c` - `vec2.<n>:` / `vec2=<n>,` setup parser.

This is materially better than legacy where `vector_kern.c` is 1800 lines
of mixed concerns and `vector_user.c` is a 940-line option splitter.

### Layering

  - The `um_vec2_host_ops` vtable is a strong boundary: TX/RX batches
    take only opaque owners and ownership-transfer callbacks. The same
    interface is implemented by the real TAP and FD backends and by the
    deterministic `vector2_fake_host` used in KUnit. This is the right
    shape for replacing legacy transports incrementally.
  - The lifecycle state machine is genuinely enforced - every
    `um_vec2_dev_transition` and `um_vec2_chan_transition` call goes
    through `um_vec2_*_can_transition` and refuses illegal moves. This
    is a clear regression-resistance win over the legacy `vp->opened`
    bool.
  - The typed config (`struct um_vec2_config` with named fields and
    explicit `has_*` discriminants) replaces the legacy
    `uml_vector_parse` token table. Bounds checks and trusted-host
    gating happen at parse time, not in the data path.

### Integration points

  - `vector2_core.c` runs `late_initcall` aggregation. Per-device
    netdev registration is synchronous on that initcall, which means
    runtime open errors at boot will appear in dmesg next to the
    legacy driver's output. No `module_init`/`module_exit` parity is
    provided; v2 is non-removable.
  - There is no `__exit` teardown of the global `um_vec2_devices` list
    outside the failure path of `um_vec2_core_init`. If v2 is ever made
    modular this needs to change. (`um_vec2_core_free_all` exists but
    is only used on init failure.)
  - The IRQ wiring goes through `um_request_irq(UM_IRQ_ALLOC, ...)`
    which is the modern UML IRQ allocator. Legacy uses a static
    `irq_rr` round-robin from `VECTOR_BASE_IRQ`. v2 is correct here.
  - `dev->irq` is set only for channel 0 (`vector2_netdev.c:367-368`).
    Single-irq drivers usually expose only the primary; multi-queue
    devices typically don't bother setting `dev->irq`. This is fine.

### `net_device_ops` surface

v2's table (`vector2_netdev.c:25-32`):

  - `ndo_open`, `ndo_stop`, `ndo_start_xmit`, `ndo_select_queue`
  - `ndo_set_mac_address`, `ndo_validate_addr`

Legacy adds: `ndo_set_rx_mode`, `ndo_tx_timeout`, `ndo_fix_features`,
`ndo_set_features`, `ndo_poll_controller`. v2 sets `dev->watchdog_timeo
= HZ` (`vector2_netdev.c:619`) but provides no `ndo_tx_timeout` handler.
The kernel default just logs - no recovery. For a driver claiming the
legacy crash fixed, missing tx_timeout is a step backward in
observability.

`ndo_get_stats64` is also absent; the device falls back to
`dev->stats` which is updated piecewise in TX/RX completion. That's the
same approach legacy takes.

## 5. Coverage gaps

KUnit coverage summary (8 suites, 76 cases by my count):

  - `vector2_cmdline_test.c` - 5 cases. Form parsing only; does not
    cover overflow of the unit_buf (>15 chars).
  - `vector2_config_test.c` - 12 cases. Defaults, trusted-host gating,
    duplicate keys, bool policy, bounds, paired keys, L2TPv3 fields.
    Does not cover `vde_mode` (which is unreachable - see B3).
  - `vector2_model_test.c` - 7 cases. Happy path, illegal transitions,
    state names.
  - `vector2_queue_test.c` - 8 cases. Wraparound, partial completion,
    invalid inputs, RX prepare/complete/consume/reset, alloc failure
    unwind.
  - `vector2_transport_test.c` - 8 cases. GRE/L2TPv3 build/parse,
    short-buffer rejection, mismatch.
  - `vector2_fake_host_test.c` - 8 cases. TX complete/partial/error/dead,
    RX packets/empty/alloc-failure/dead.
  - `vector2_ethtool_test.c` - 4 cases. Stats while stopped, ring
    policy, drop stats, multiqueue stats aggregation.
  - `vector2_netdev_test.c` - 6 cases. Naming, MAC, open without
    backend, stop in registered state, xmit drop when not running,
    queue count, queue-to-CPU policy.
  - `vector2_host_fd_test.c` - 12 cases. Open/close, bad fd, wrong
    type, multiqueue open/close, multiqueue missing second fd, netdev
    open/stop including 1000-iter stress, bad-fd unwind, injected
    fail_open, missing-config 10000-iter stress, TX/RX over pipes.
  - `vector2_host_tap_test.c` - 6 cases. Attach/close, busy reject,
    TX frame, RX frame, sandbox fail, netdev open sandbox unwind.

### Gaps

  - **B1 reproducer:** no test injects `kzalloc` or `os_dup_file`
    failure for queue index >= 1. Adding a failing slab fault injector
    or a stub `os_dup_file` that fails on the second call would trip
    the UAF.
  - **B2 reproducer:** no test feeds the TAP backend a frame shorter
    than `sizeof(struct virtio_net_hdr)` and asserts `-EPROTO`
    propagation or RX_PROTO_DROPS bump. The existing TAP RX test only
    asserts the happy path.
  - **R1 reproducer:** no test races `register_netdevice` against
    `ndo_open`. This is hard to write in KUnit.
  - **R2 reproducer:** no test changes MTU after open and pushes a
    larger frame.
  - **R3 reproducer:** no test transitions the lifecycle from a
    parallel context while `start_xmit` is in flight.
  - **Backend-dead path:** `UM_VEC2_STAT_BACKEND_DEAD` is set when
    `tx_batch` or `rx_batch` returns `-ENODEV` in NAPI poll, and the
    handler stops all queues and turns off carrier
    (`vector2_netdev.c:285-292`). Once the device is `RUNNING` with
    carrier off and queues stopped, there is no recovery path - the
    device stays in `RUNNING` forever from the lifecycle model's
    perspective, but it can never xmit because `netif_tx_stop_all_queues`
    was called. The fake host's `_kill` is tested at the host-ops
    level, but not end-to-end through NAPI poll.
  - **ethtool stats while running:**
    `vector2_ethtool_multiqueue_stats_aggregate_test` constructs
    channels by hand without ever opening the device. There is no test
    that reads stats while a real backend is running. The
    `spin_lock_bh(&queue->tx_lock)` and `&queue->rx_lock` paths in
    `um_vec2_get_ethtool_stats` are therefore not exercised under
    contention.
  - **NAPI poll path:** there is no KUnit that drives
    `um_vec2_netdev_poll` directly with a fake host. The poll
    function's `goto backend_dead` and `tx_more && tx_done > 0`
    rescheduling branches are dead from a test perspective.
  - **XPS configuration failure:** `um_vec2_netdev_configure_xps()`
    logs a warning on failure but cannot be exercised in KUnit because
    `netif_set_xps_queue()` always succeeds for offline cpumasks.

## 6. Versus legacy

### Material improvements

  - **Typed config:** legacy's `uml_vector_parse` destructively splits
    the option string and stores pointers into a token table. v2 uses
    `strscpy`-validated bounded fields and a `seen` bitmap to reject
    duplicates. v2 has explicit `has_fd`, `has_mac`, `has_rx_key`,
    `has_tx_key` discriminants instead of legacy's "set if non-null
    string" implicit nullability.
  - **Sandbox gating at parse time:** legacy has no sandbox concept;
    it always trusts whatever option the user passes. v2 routes all
    host-resource keys (`ifname`, `src`, `dst`, `ifup`, `bpffile`,
    etc.) through `um_vec2_key_trusted_host_only()` and refuses them
    when `UM_VEC2_PARSE_TRUSTED_HOST` is clear (parser side) and via
    `CONFIG_UML_NET_VECTOR_V2_INPROC` (TAP backend side).
  - **Inherited-fd transport:** v2's `fd=` transport accepts a
    launcher-delegated fd range without ever invoking host-side TAP
    creation. Legacy has no equivalent; its FD pseudo-transport
    requires the UML process to open the fd itself.
  - **Explicit state machine:** v2's `dev_state` and `chan_state`
    enums catch illegal lifecycle moves at runtime. Legacy relies on
    `vp->opened` bool and ad-hoc ordering; the `vector_net_open+0x3a3`
    crash was precisely a state-confusion in that ordering.
  - **Queue ownership rings:** v2's `um_vec2_tx_desc_state` and
    `um_vec2_rx_slot_state` enums catch double-complete and
    double-prepare in unit tests. Legacy's `vector_queue` uses raw
    pointer arithmetic and asserts via `WARN_ON` in production paths
    only.
  - **Bounds-checked transport headers:** v2's GRE and L2TPv3 helpers
    take `(buf, len, offset)` and return `-EMSGSIZE` on overrun.
    Legacy builds headers in place on user-supplied buffers without
    explicit bounds checks.
  - **Per-queue ethtool stats:** v2 exposes per-queue TX/RX ring
    counters via ethtool strings (`queueN_tx_ring_used`, etc.).
    Legacy has only aggregate stats.
  - **`ndo_select_queue` and XPS:** v2 computes a deterministic
    queue-to-CPU mapping (`um_vec2_tx_queue_uses_cpu_ordinal`) and
    applies it via `netif_set_xps_queue`. Legacy has no XPS hook.

### Regressions vs legacy

  - **Missing `ndo_tx_timeout`:** legacy has `vector_net_tx_timeout`
    that resets stats and logs; v2 has none despite setting
    `watchdog_timeo`.
  - **Missing `ndo_set_features` / `ndo_fix_features`:** legacy
    accepts ethtool feature toggles for offloads; v2 hardcodes
    `dev->features` (actually, doesn't set any features at all - the
    `gro`/`gso`/`csum` parse flags do not feed into `dev->features` or
    `dev->hw_features` anywhere I can find).
  - **Missing `ndo_set_rx_mode`:** legacy handles multicast filtering;
    v2 ignores it. For TAP this matters because the host TAP
    interface may need promisc.
  - **Missing `ndo_poll_controller`:** legacy supports netconsole; v2
    does not.
  - **No `gro`/`gso`/`csum` config wiring:** `vector2_config.c` parses
    these flags but `vector2_netdev.c:um_vec2_netdev_init` never
    consults them. They are effectively no-ops today.
  - **No BPF/raw-socket transport:** v2 supports only TAP and FD;
    legacy supports raw, gre, l2tpv3, hybrid, bess, vde, fd, proxy.
    The config parser declares enum values for these but
    `um_vec2_open_backend` returns `-EOPNOTSUPP` for everything
    except TAP and FD (`vector2_netdev.c:39-46`).
  - **No legacy `vecN:` compatibility:** v2 uses `vec2.<n>:` /
    `vec2=<n>,` syntax. Migration requires command-line edits.

### The `vector_net_open+0x3a3` regression

Per `16-uml-vector-legacy-tap-crash-r0.md`, the legacy crash was a NULL
deref in `vector_net_open`. Reading the legacy code at
`vector_kern.c:1210-1310`, `vp->fds = uml_vector_user_open(...)` can
return NULL (line 1222-1225), and several later paths (BPF attach at
line 1306, IRQ request at line 1270 using `vp->fds->rx_fd`) would
dereference `vp->fds` if the open partly succeeded then later code
mis-recovered. The `goto out_close` path at line 1225 only handles the
immediate failure; intermediate failures (e.g. build_transport_data
failure after fds was set) hit `out_close` with `vp->fds` non-NULL,
which is then walked by `vector_net_close` -> double-free of fds
structure.

v2's `um_vec2_netdev_open` is structurally safer:

  - `um_vec2_dev_can_open` rejects re-entry at line 457-460.
  - `um_vec2_open_backend` failure goes through
    `um_vec2_unwind_open` which closes the backend (no-op if not yet
    allocated) and transitions the lifecycle back.
  - The two callable backends (`um_vec2_fd_open`,
    `um_vec2_tap_open`) check `if (vdev->channels) return -EBUSY` at
    entry, preventing double-open.
  - No code path in v2 accesses `vdev->channels[i]` without first
    checking `vdev->channels && vdev->num_channels` (e.g.
    `vector2_netdev.c:73-76`, `vector2_netdev.c:339-340`).

So the *specific* legacy crash is provably impossible in v2 by
construction. B1 above is a different UAF in a different code path.

## 7. Recommended next actions

Ordered by criticality:

  1. **B1 fix (HIGH):** drop `vdev->channels`/`num_channels` writes
     from `um_vec2_fd_channel_open()`; also clear `vdev->channels =
     NULL; vdev->num_channels = 0;` after the `kfree(channels)` in
     `um_vec2_fd_open`'s unwind. Add a KUnit that injects a failure
     inside `um_vec2_fd_channel_open` after channel 0 succeeds (the
     simplest approach is a stub `os_set_fd_block` that fails on the
     second call) and asserts the unwound state plus subsequent
     `um_vec2_fd_close()` does not crash.

  2. **B2 fix (MEDIUM):** in `um_vec2_tap_rx_batch()`, on EPROTO
     `goto complete;` instead of `break;`, and stop unconditionally
     zeroing `ret` before the label. Add a KUnit that writes a
     1-byte frame to the TAP fd, calls `rx_batch`, and asserts the
     return is `-EPROTO` and `UM_VEC2_STAT_RX_PROTO_DROPS` increments
     after a NAPI poll.

  3. **B5 fix (LOW):** make `um_vec2_netdev_stop` return 0 for any
     state and emit a `netdev_warn` for unexpected ones.

  4. **R1 investigate (HIGH if exploitable, LOW otherwise):** confirm
     the `register_netdevice` -> `vdev->netdev = dev` window can or
     cannot be raced by a netlink `ip link set up`. If it can, move
     the assignment before `register_netdevice`.

  5. **Wire `gro`/`gso`/`csum`/`fix_features`/`set_features`
     (MEDIUM):** feed parser flags into `dev->hw_features` and
     `dev->features` in `um_vec2_netdev_init`; add the
     `ndo_fix_features` / `ndo_set_features` hooks. Otherwise these
     parser knobs are observability lies.

  6. **Add `ndo_tx_timeout` (LOW):** at minimum log diagnostic
     ethtool counters and reset NAPI; matches legacy parity.

  7. **B3 fix (LOW):** wire `vde_mode` into the key table or remove
     the dead enum and switch branch.

  8. **B4 fix (LOW):** mirror FD backend's QUIESCING/CLOSED unwind
     in TAP backend's `um_vec2_tap_channel_attach_fd` failure paths.

  9. **R2 investigate (MEDIUM):** decide whether to cap
     `dev->max_mtu` at the parse-time MTU or add a real
     `ndo_change_mtu` that quiesces backends.

 10. **Test gaps:** add KUnit coverage for the NAPI poll path with a
     fake host (to exercise `backend_dead`, `tx_more` reschedule, and
     `rx_done = -EPROTO`/`-ENOMEM` branches end-to-end); add a
     contention test for ethtool-stats-while-running so the queue
     spinlock ordering is exercised.

### Swap-readiness verdict

For the **inherited-FD single-queue** path the driver is plausibly
swap-ready once B1 and R1 are addressed: the legacy crash motivation
is structurally gone, the lifecycle is enforced, sandbox gating is
clear, and KUnit covers the realistic open/close failure modes.

For **multi-queue FD**, B1 is a blocker because the same path that
launchers use for `umlctl` fd handoff will hit it on any allocator
failure beyond queue 0.

For **TAP transport** (trusted-host builds only), B2 should be fixed
before this path is used in any seccomp or KCSAN soak that asserts on
proto-drop counters; the underlying datapath is correct, only the
error visibility is broken.

For **all other legacy transports** (raw, gre, l2tpv3, hybrid, bess,
vde, proxy), v2 is not a replacement at all - the parser accepts the
keywords but `um_vec2_open_backend` rejects them with `-EOPNOTSUPP`.
Any plan to deprecate legacy `CONFIG_UML_NET_VECTOR` should treat
this as a hard prerequisite.
