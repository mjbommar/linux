// SPDX-License-Identifier: GPL-2.0
/*
 * Netdev registration skeleton for UML vector networking v2.
 */

#define pr_fmt(fmt) "uml-vector2: " fmt

#include <linux/cpumask.h>
#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>

#include <irq_kern.h>
#include <irq_user.h>

#include "vector2_internal.h"

#define UM_VEC2_NAPI_MAX_WEIGHT	64U

static void um_vec2_netdev_tx_timeout(struct net_device *dev,
				      unsigned int txqueue);
static void um_vec2_netdev_set_rx_mode(struct net_device *dev);
#ifdef CONFIG_NET_POLL_CONTROLLER
static void um_vec2_netdev_poll_controller(struct net_device *dev);
#endif

static const struct net_device_ops um_vec2_netdev_ops = {
	.ndo_open		= um_vec2_netdev_open,
	.ndo_stop		= um_vec2_netdev_stop,
	.ndo_start_xmit		= um_vec2_netdev_start_xmit,
	.ndo_select_queue	= um_vec2_netdev_select_queue,
	.ndo_tx_timeout		= um_vec2_netdev_tx_timeout,
	.ndo_set_rx_mode	= um_vec2_netdev_set_rx_mode,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= um_vec2_netdev_poll_controller,
#endif
};

/*
 * Minimal watchdog handler: log the timeout and mark the queue start
 * timestamp so netif_tx_lock_bh can deliver the next xmit attempt.
 * Matches legacy vector_kern.c's vector_net_tx_timeout shape: the
 * driver does not auto-reset state today because the backend has no
 * "TX stuck" notion separate from BACKEND_DEAD.  Logging the event
 * is what tooling needs.  See audit P2.4.
 */
static void um_vec2_netdev_tx_timeout(struct net_device *dev,
				      unsigned int txqueue)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);

	netdev_warn(dev, "vector v2 ndo_tx_timeout queue=%u state=%s\n",
		    txqueue, um_vec2_dev_state_name(vdev->life.state));
	netif_trans_update(dev);
}

/*
 * ndo_set_rx_mode stub.  The host TAP backend negotiates flags at
 * /dev/net/tun open time (IFF_TAP | IFF_NO_PI | IFF_VNET_HDR) and
 * has no live interface for per-multicast/promisc filter changes
 * from the guest side.  Accept the call (avoid -EOPNOTSUPP noise
 * from "ip" / userspace) and rely on the host's TAP for upstream
 * filtering.  See audit P2.5.
 */
static void um_vec2_netdev_set_rx_mode(struct net_device *dev)
{
	netdev_dbg(dev, "vector v2 ndo_set_rx_mode flags=0x%x\n", dev->flags);
}

#ifdef CONFIG_NET_POLL_CONTROLLER
/*
 * netconsole support: drive a poll cycle on each channel's NAPI to
 * flush the receive ring without waiting for an IRQ.  Legacy parity
 * (vector_kern.c::vector_net_poll_controller).  See audit P2.6.
 */
static void um_vec2_netdev_poll_controller(struct net_device *dev)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);
	unsigned int i;

	if (!vdev->channels)
		return;
	for (i = 0; i < vdev->num_channels; i++) {
		struct um_vec2_channel *channel = &vdev->channels[i];

		if (channel->rx_irq != UM_VEC2_NO_IRQ) {
			disable_irq(channel->rx_irq);
			napi_schedule(&channel->napi);
			enable_irq(channel->rx_irq);
		}
	}
}
#endif

static void um_vec2_stop_datapath(struct net_device *dev,
				  struct um_vec2_dev *vdev);

static int um_vec2_open_backend(struct um_vec2_dev *vdev)
{
	switch (vdev->cfg.transport) {
	case UM_VEC2_TRANSPORT_TAP:
		return um_vec2_tap_open(vdev);
	case UM_VEC2_TRANSPORT_FD:
		return um_vec2_fd_open(vdev);
	default:
		/*
		 * The cmdline parser accepts seven additional transports
		 * (raw/gre/l2tpv3/hybrid/bess/vde/proxy) for forward
		 * compatibility + KUnit coverage of transport-specific
		 * keys, but only TAP and FD have runtime backends today.
		 * Surface that explicitly here rather than the bare
		 * -EOPNOTSUPP that downstream ip-link sees as "operation
		 * not supported".  See audit P4.2.
		 */
		pr_err("vec2.%u transport=%s is parsed but not implemented; use transport=tap or transport=fd\n",
		       vdev->unit,
		       um_vec2_transport_name(vdev->cfg.transport));
		return -EOPNOTSUPP;
	}
}

static void um_vec2_close_backend(struct um_vec2_dev *vdev)
{
	switch (vdev->cfg.transport) {
	case UM_VEC2_TRANSPORT_TAP:
		um_vec2_tap_close(vdev);
		break;
	case UM_VEC2_TRANSPORT_FD:
		um_vec2_fd_close(vdev);
		break;
	default:
		break;
	}
}

unsigned int um_vec2_netdev_queue_count(const struct um_vec2_dev *vdev)
{
	if (!vdev->cfg.queues)
		return 1;
	return vdev->cfg.queues;
}

static struct um_vec2_channel *
um_vec2_channel_for_mapping(struct um_vec2_dev *vdev, unsigned int mapping)
{
	if (!vdev->channels || !vdev->num_channels)
		return NULL;
	return &vdev->channels[mapping % vdev->num_channels];
}

static struct um_vec2_channel *
um_vec2_channel_for_skb(struct um_vec2_dev *vdev, const struct sk_buff *skb)
{
	return um_vec2_channel_for_mapping(vdev, skb_get_queue_mapping(skb));
}

bool um_vec2_tx_queue_uses_cpu_ordinal(unsigned int queue,
				       unsigned int cpu_ordinal,
				       unsigned int queues,
				       unsigned int cpu_count)
{
	if (!queues || !cpu_count || queue >= queues ||
	    cpu_ordinal >= cpu_count)
		return false;

	if (queues <= cpu_count)
		return cpu_ordinal % queues == queue;

	return queue % cpu_count == cpu_ordinal;
}

u16 um_vec2_netdev_select_queue(struct net_device *dev, struct sk_buff *skb,
				struct net_device *sb_dev)
{
	u16 queue;

	if (dev->real_num_tx_queues <= 1)
		return 0;

	queue = netdev_pick_tx(dev, skb, sb_dev);
	return netdev_cap_txqueue(dev, queue);
}

static void um_vec2_netdev_configure_xps(struct net_device *dev,
					 unsigned int queues)
{
	cpumask_var_t mask;
	unsigned int cpu;
	unsigned int cpu_count;
	unsigned int ordinal;
	unsigned int queue;
	int ret;

	if (queues <= 1)
		return;

	cpu_count = num_online_cpus();
	if (!cpu_count)
		return;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL)) {
		netdev_warn(dev, "vector v2 XPS setup skipped: no memory\n");
		return;
	}

	for (queue = 0; queue < queues; queue++) {
		cpumask_clear(mask);
		ordinal = 0;
		for_each_online_cpu(cpu) {
			if (um_vec2_tx_queue_uses_cpu_ordinal(queue, ordinal,
							      queues,
							      cpu_count))
				cpumask_set_cpu(cpu, mask);
			ordinal++;
		}

		ret = netif_set_xps_queue(dev, mask, queue);
		if (ret)
			netdev_warn(dev,
				    "vector v2 XPS setup failed for queue %u: %d\n",
				    queue, ret);
	}

	free_cpumask_var(mask);
}

static void um_vec2_tx_complete_skb(void *owner, unsigned int len,
				    void *cookie)
{
	struct net_device *dev = cookie;
	struct sk_buff *skb = owner;

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;
	dev_consume_skb_any(skb);
}

static void um_vec2_rx_free_skb(void *owner, unsigned int len, void *cookie)
{
	struct sk_buff *skb = owner;

	dev_kfree_skb_any(skb);
}

struct um_vec2_rx_poll_ctx {
	struct napi_struct *napi;
	struct net_device *dev;
	unsigned int frame_len;
};

static void *um_vec2_rx_alloc_skb(unsigned int slot, void *cookie)
{
	struct um_vec2_rx_poll_ctx *ctx = cookie;
	struct sk_buff *skb;

	skb = napi_alloc_skb(ctx->napi, ctx->frame_len);
	if (!skb)
		return NULL;
	skb->dev = ctx->dev;
	return skb;
}

static void um_vec2_rx_deliver_skb(void *owner, unsigned int len, void *cookie)
{
	struct um_vec2_rx_poll_ctx *ctx = cookie;
	struct sk_buff *skb = owner;

	skb->protocol = eth_type_trans(skb, ctx->dev);
	ctx->dev->stats.rx_packets++;
	ctx->dev->stats.rx_bytes += skb->len;
	napi_gro_receive(ctx->napi, skb);
}

static unsigned int um_vec2_napi_weight(const struct um_vec2_dev *vdev)
{
	return min_t(unsigned int, vdev->cfg.depth, UM_VEC2_NAPI_MAX_WEIGHT);
}

static int um_vec2_netdev_poll(struct napi_struct *napi, int budget)
{
	struct um_vec2_channel *channel =
		container_of(napi, struct um_vec2_channel, napi);
	struct um_vec2_dev *vdev = channel->vdev;
	struct net_device *dev = vdev->netdev;
	struct um_vec2_queue_pair *queue = channel->queue;
	struct um_vec2_rx_poll_ctx rx_ctx;
	bool tx_more = false;
	int tx_done = 0;
	int rx_done = 0;

	um_vec2_stat_inc(vdev, UM_VEC2_STAT_NAPI_POLLS);

	if (!channel->host || !queue)
		goto complete;

	spin_lock(&queue->tx_lock);
	if (!um_vec2_tx_ring_empty(&queue->tx)) {
		tx_done = channel->host->ops->tx_batch(channel->host,
						       &queue->tx,
						       queue->tx.depth,
						       um_vec2_tx_complete_skb,
						       dev);
		if (tx_done > 0) {
			netif_wake_subqueue(dev, channel->index);
			netif_trans_update(dev);
		}
		tx_more = !um_vec2_tx_ring_empty(&queue->tx);
		if (!tx_done && tx_more)
			um_vec2_stat_inc(vdev,
					 UM_VEC2_STAT_TX_TRANSIENT_ERRORS);
		else if (tx_done < 0 && tx_done != -ENODEV)
			um_vec2_stat_inc(vdev, UM_VEC2_STAT_TX_FATAL_ERRORS);
	}
	spin_unlock(&queue->tx_lock);

	if (tx_done == -ENODEV) {
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_BACKEND_DEAD);
		goto backend_dead;
	}

	rx_ctx.napi = napi;
	rx_ctx.dev = dev;
	rx_ctx.frame_len = um_vec2_runtime_frame_len(dev,
						     vdev->cfg.transport ==
						     UM_VEC2_TRANSPORT_TAP);

	spin_lock(&queue->rx_lock);
	rx_done = channel->host->ops->rx_batch(channel->host, &queue->rx,
					       budget, um_vec2_rx_alloc_skb,
					       um_vec2_rx_free_skb, &rx_ctx);
	if (rx_done > 0) {
		um_vec2_rx_batch_consume(&queue->rx, rx_done,
					 um_vec2_rx_deliver_skb, &rx_ctx);
	} else if (rx_done == -EPROTO) {
		dev->stats.rx_dropped++;
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_RX_PROTO_DROPS);
	} else if (rx_done == -ENOMEM) {
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_RX_ALLOC_ERRORS);
	} else if (rx_done < 0 && rx_done != -ENODEV) {
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_RX_FATAL_ERRORS);
	}
	spin_unlock(&queue->rx_lock);

	if (rx_done == -ENODEV) {
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_BACKEND_DEAD);
		goto backend_dead;
	}
	if (rx_done < 0)
		rx_done = 0;

complete:
	if (rx_done < budget)
		napi_complete_done(napi, rx_done);
	if (tx_more && tx_done > 0)
		napi_schedule(napi);
	return rx_done;

backend_dead:
	/*
	 * Terminal state: backend signalled -ENODEV (host fd closed,
	 * TAP vanished, etc.).  We stop xmit + drop carrier so the
	 * netdev framework + tooling see "link down" + queue stopped.
	 * Recovery requires "ip link set vec2.X down && up" — the
	 * B5-fixed um_vec2_netdev_stop now handles the carrier-off
	 * + queues-stopped state idempotently, so a subsequent
	 * "down" drives lifecycle back to REGISTERED and "up"
	 * re-opens cleanly.  Auto-recovery is intentionally not
	 * attempted: the operator has to fix whatever the host
	 * condition was before retry can succeed.  See audit P3.3 +
	 * 47-uml-vector-driver-v2-audit-risks-resolution-2026-05-17.md.
	 */
	netif_tx_stop_all_queues(dev);
	netif_carrier_off(dev);
	if (rx_done < 0)
		rx_done = 0;
	if (rx_done < budget)
		napi_complete_done(napi, rx_done);
	return rx_done;
}

static irqreturn_t um_vec2_rx_interrupt(int irq, void *dev_id)
{
	struct um_vec2_channel *channel = dev_id;
	struct um_vec2_dev *vdev;
	struct net_device *dev;

	if (!channel)
		return IRQ_NONE;
	vdev = channel->vdev;
	dev = vdev->netdev;
	if (!dev || !channel->napi_enabled || !netif_running(dev))
		return IRQ_NONE;
	um_vec2_stat_inc(vdev, UM_VEC2_STAT_RX_IRQS);
	napi_schedule(&channel->napi);
	return IRQ_HANDLED;
}

static irqreturn_t um_vec2_tx_interrupt(int irq, void *dev_id)
{
	struct um_vec2_channel *channel = dev_id;
	struct um_vec2_dev *vdev;
	struct net_device *dev;

	if (!channel)
		return IRQ_NONE;
	vdev = channel->vdev;
	dev = vdev->netdev;
	if (!dev || !channel->napi_enabled || !netif_running(dev))
		return IRQ_NONE;
	um_vec2_stat_inc(vdev, UM_VEC2_STAT_TX_IRQS);
	napi_schedule(&channel->napi);
	return IRQ_HANDLED;
}

static int um_vec2_start_datapath(struct net_device *dev,
				  struct um_vec2_dev *vdev)
{
	struct um_vec2_channel *channel;
	unsigned int i;
	int ret;

	if (vdev->cfg.transport != UM_VEC2_TRANSPORT_TAP &&
	    vdev->cfg.transport != UM_VEC2_TRANSPORT_FD)
		return 0;
	if (!vdev->channels || !vdev->num_channels)
		return -EINVAL;

	for (i = 0; i < vdev->num_channels; i++) {
		channel = &vdev->channels[i];
		if (!channel->host || !channel->queue) {
			ret = -EINVAL;
			goto out_stop_started;
		}

		netif_napi_add_weight(dev, &channel->napi,
				      um_vec2_netdev_poll,
				      um_vec2_napi_weight(vdev));
		channel->napi_added = true;

		if (channel->rx_fd == UM_VEC2_NO_FD ||
		    channel->tx_fd == UM_VEC2_NO_FD) {
			ret = -EBADF;
			goto out_stop_started;
		}

		ret = um_request_irq(UM_IRQ_ALLOC, channel->rx_fd, IRQ_READ,
				     um_vec2_rx_interrupt, IRQF_SHARED,
				     dev->name, channel);
		if (ret < 0)
			goto out_stop_started;

		channel->rx_irq = ret;
		if (!i)
			dev->irq = ret;

		ret = um_request_irq(UM_IRQ_ALLOC, channel->tx_fd, IRQ_WRITE,
				     um_vec2_tx_interrupt, IRQF_SHARED,
				     dev->name, channel);
		if (ret < 0)
			goto out_stop_started;

		channel->tx_irq = ret;

		ret = um_vec2_chan_transition(&channel->life,
					      UM_VEC2_CHAN_IRQ_ATTACHED);
		if (ret)
			goto out_stop_started;

		napi_enable(&channel->napi);
		channel->napi_enabled = true;

		ret = um_vec2_chan_transition(&channel->life,
					      UM_VEC2_CHAN_NAPI_ENABLED);
		if (ret)
			goto out_stop_started;

		ret = um_vec2_chan_transition(&channel->life,
					      UM_VEC2_CHAN_ACTIVE);
		if (ret)
			goto out_stop_started;
	}

	return 0;

out_stop_started:
	um_vec2_stop_datapath(dev, vdev);
	return ret;
}

static void um_vec2_stop_datapath(struct net_device *dev,
				  struct um_vec2_dev *vdev)
{
	unsigned int i;

	if (!vdev->channels || !vdev->num_channels)
		return;

	for (i = 0; i < vdev->num_channels; i++) {
		struct um_vec2_channel *channel = &vdev->channels[i];

		if (channel->napi_enabled) {
			napi_disable(&channel->napi);
			channel->napi_enabled = false;
		}
		if (channel->rx_irq != UM_VEC2_NO_IRQ) {
			um_free_irq(channel->rx_irq, channel);
			channel->rx_irq = UM_VEC2_NO_IRQ;
			if (!i)
				dev->irq = 0;
		}
		if (channel->tx_irq != UM_VEC2_NO_IRQ) {
			um_free_irq(channel->tx_irq, channel);
			channel->tx_irq = UM_VEC2_NO_IRQ;
		}
		if (channel->napi_added) {
			netif_napi_del(&channel->napi);
			channel->napi_added = false;
		}
	}
}

static int um_vec2_unwind_open(struct um_vec2_dev *vdev)
{
	int ret;

	um_vec2_close_backend(vdev);

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_QUIESCING);
	if (ret)
		return ret;

	return um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_REGISTERED);
}

int um_vec2_netdev_open(struct net_device *dev)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);
	u64 attempt;
	int ret;

	mutex_lock(&vdev->lock);
	attempt = um_vec2_stat_inc_return(vdev, UM_VEC2_STAT_OPEN_ATTEMPTS);
	if (!um_vec2_dev_can_open(&vdev->life)) {
		ret = -EINVAL;
		goto out;
	}

	if (vdev->cfg.fail_open_after &&
	    attempt >= vdev->cfg.fail_open_after) {
		netdev_info(dev,
			    "vector v2 injected open failure at attempt %llu threshold %u\n",
			    (unsigned long long)attempt,
			    vdev->cfg.fail_open_after);
		ret = -EIO;
		goto out;
	}

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_OPENING);
	if (ret)
		goto out;

	netif_carrier_off(dev);
	netif_tx_stop_all_queues(dev);

	ret = um_vec2_open_backend(vdev);
	if (ret) {
		netdev_info(dev, "vector v2 host backend is not available: %d\n",
			    ret);
		if (um_vec2_unwind_open(vdev))
			netdev_err(dev, "vector v2 open unwind failed\n");
		goto out;
	}

	ret = um_vec2_start_datapath(dev, vdev);
	if (ret) {
		netdev_err(dev, "vector v2 datapath start failed: %d\n", ret);
		if (um_vec2_unwind_open(vdev))
			netdev_err(dev, "vector v2 open unwind failed\n");
		goto out;
	}

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_RUNNING);
	if (ret) {
		um_vec2_stop_datapath(dev, vdev);
		if (um_vec2_unwind_open(vdev))
			netdev_err(dev, "vector v2 open unwind failed\n");
		goto out;
	}

	if (vdev->cfg.transport == UM_VEC2_TRANSPORT_TAP ||
	    vdev->cfg.transport == UM_VEC2_TRANSPORT_FD) {
		unsigned int i;

		um_vec2_netdev_configure_xps(dev, dev->real_num_tx_queues);
		netif_carrier_on(dev);
		netif_tx_start_all_queues(dev);
		/*
		 * napi_schedule() under vdev->lock is safe: it sets
		 * NAPI_STATE_SCHED and raises a softirq, taking no
		 * external locks itself.  The softirq runs
		 * um_vec2_netdev_poll in a separate context which never
		 * takes vdev->lock, so there is no AB-BA ordering risk.
		 * See audit R4 +
		 * 47-uml-vector-driver-v2-audit-risks-resolution-2026-05-17.md.
		 */
		for (i = 0; i < vdev->num_channels; i++)
			napi_schedule(&vdev->channels[i].napi);
		ret = 0;
	}

out:
	if (ret)
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_OPEN_FAILURES);
	mutex_unlock(&vdev->lock);
	return ret;
}

int um_vec2_netdev_stop(struct net_device *dev)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);
	int ret = 0;

	mutex_lock(&vdev->lock);
	netif_tx_stop_all_queues(dev);
	netif_carrier_off(dev);

	switch (vdev->life.state) {
	case UM_VEC2_DEV_REGISTERED:
		break;
	case UM_VEC2_DEV_RUNNING:
	case UM_VEC2_DEV_OPENING:
		ret = um_vec2_dev_transition(&vdev->life,
					     UM_VEC2_DEV_QUIESCING);
		if (ret)
			break;
		um_vec2_stop_datapath(dev, vdev);
		um_vec2_close_backend(vdev);
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_CLOSES);
		ret = um_vec2_dev_transition(&vdev->life,
					     UM_VEC2_DEV_REGISTERED);
		break;
	default:
		/*
		 * ndo_stop is expected to be idempotent: returning an
		 * error here would make "ip link set vec2.X down" fail
		 * for already-stopped devices, which the netdev framework
		 * (and operator tooling) treats as a hard failure.  Log
		 * the unexpected state for diagnostics and return 0.
		 * See audit B5.
		 */
		netdev_warn(dev,
			    "vector v2 ndo_stop called in unexpected state %s; treating as already stopped\n",
			    um_vec2_dev_state_name(vdev->life.state));
		break;
	}

	mutex_unlock(&vdev->lock);
	return ret;
}

netdev_tx_t um_vec2_netdev_start_xmit(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);
	struct um_vec2_channel *channel;
	struct um_vec2_queue_pair *queue;
	unsigned int len = skb->len;
	int ret;

	um_vec2_stat_inc(vdev, UM_VEC2_STAT_TX_XMIT_CALLS);

	/*
	 * The lifecycle check below is intentionally unlocked.  The
	 * netdev framework guarantees `__LINK_STATE_START` is cleared
	 * and any in-flight `ndo_start_xmit` has drained (via
	 * synchronize_net) before our `um_vec2_netdev_stop` runs, so
	 * a parallel ndo_stop cannot tear down the backend while we
	 * are here.  Adding `vdev->lock` would defeat the spin_lock_bh
	 * fast path with no real-world benefit.  See audit R3 +
	 * 47-uml-vector-driver-v2-audit-risks-resolution-2026-05-17.md.
	 */
	if (!um_vec2_dev_can_xmit(&vdev->life)) {
		dev->stats.tx_dropped++;
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_TX_DROPPED);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	channel = um_vec2_channel_for_skb(vdev, skb);
	if (!channel || !channel->host || !channel->queue) {
		dev->stats.tx_dropped++;
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_TX_DROPPED);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	queue = channel->queue;
	spin_lock_bh(&queue->tx_lock);
	if (um_vec2_tx_ring_full(&queue->tx)) {
		netif_stop_subqueue(dev, channel->index);
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_TX_BUSY);
		spin_unlock_bh(&queue->tx_lock);
		return NETDEV_TX_BUSY;
	}

	ret = um_vec2_tx_ring_enqueue(&queue->tx, skb, len);
	if (ret) {
		spin_unlock_bh(&queue->tx_lock);
		dev->stats.tx_dropped++;
		um_vec2_stat_inc(vdev, UM_VEC2_STAT_TX_DROPPED);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (um_vec2_tx_ring_full(&queue->tx))
		netif_stop_subqueue(dev, channel->index);
	spin_unlock_bh(&queue->tx_lock);

	napi_schedule(&channel->napi);
	return NETDEV_TX_OK;
}

void um_vec2_netdev_init(struct um_vec2_dev *vdev, struct net_device *dev)
{
	struct um_vec2_netdev_priv *priv = netdev_priv(dev);

	priv->vdev = vdev;
	snprintf(dev->name, sizeof(dev->name), "%s.%u",
		 UM_VEC2_NAME_PREFIX, vdev->unit);

	/*
	 * Cap dev->max_mtu at the parse-time configured MTU.  The host
	 * backends compute their per-channel frame buffer at channel
	 * attach time (taphost->frame_len / fdhost->frame_len = mtu +
	 * ETH_HLEN + VLAN_HLEN [+ virtio_net_hdr]).  A runtime "ip link
	 * set vec2.X mtu N" that raises the MTU above cfg.mtu would
	 * leave the backend buffer too small and produce truncated
	 * frames.  Lowering the MTU at runtime is safe because the
	 * backend's larger buffer still holds any frame the netdev
	 * framework now accepts.  See audit R2.
	 */
	dev->mtu = vdev->cfg.mtu;
	dev->min_mtu = UM_VEC2_MIN_MTU;
	dev->max_mtu = vdev->cfg.mtu;
	dev->netdev_ops = &um_vec2_netdev_ops;

	/*
	 * Wire parser-time feature toggles into the kernel netdev surface.
	 * Pre-fix, cfg.gro/gso/csum were parsed but never consulted: the
	 * ethtool / "ip link show" view of features was a lie.  Mirror
	 * the legacy driver's baseline (SG + FRAGLIST) and layer the
	 * parser flags on top so reviewers + tooling see the same shape
	 * the cmdline asked for.  See audit P1.2.
	 */
	dev->hw_features = NETIF_F_SG | NETIF_F_FRAGLIST;
	if (vdev->cfg.gro)
		dev->hw_features |= NETIF_F_GRO;
	if (vdev->cfg.gso) {
		/*
		 * GSO + TSO go together: the TCP stack only generates
		 * large GSO skbs when NETIF_F_TSO* is advertised, and
		 * `virtio_net_hdr_from_skb` in our tap/fd write path
		 * encodes the gso_type for the host kernel to segment
		 * (saves the per-MTU-frame syscall cost — memo 01
		 * Step 2 root cause).
		 */
		dev->hw_features |= NETIF_F_GSO;
		dev->hw_features |= NETIF_F_TSO | NETIF_F_TSO6;
	}
	if (vdev->cfg.csum)
		dev->hw_features |= NETIF_F_HW_CSUM;
	dev->features = dev->hw_features;
	dev->watchdog_timeo = HZ;
	dev->irq = 0;

	if (vdev->cfg.has_mac)
		eth_hw_addr_set(dev, vdev->cfg.mac);
	else
		eth_hw_addr_random(dev);

	um_vec2_ethtool_attach(dev);
	netif_carrier_off(dev);
}

int um_vec2_netdev_register(struct um_vec2_dev *vdev)
{
	struct net_device *dev;
	unsigned int queues;
	int ret;

	if (vdev->life.state != UM_VEC2_DEV_CONFIGURED)
		return -EINVAL;

	queues = um_vec2_netdev_queue_count(vdev);
	dev = alloc_etherdev_mqs(sizeof(struct um_vec2_netdev_priv), queues,
				 queues);
	if (!dev)
		return -ENOMEM;

	um_vec2_netdev_init(vdev, dev);
	ret = netif_set_real_num_tx_queues(dev, queues);
	if (ret)
		goto out_free_netdev;
	ret = netif_set_real_num_rx_queues(dev, queues);
	if (ret)
		goto out_free_netdev;

	/*
	 * Publish vdev->netdev BEFORE register_netdevice() makes dev
	 * visible to the kernel netdev framework.  After register_netdevice()
	 * returns, a racing netlink "ip link set vec2.X up" can call
	 * ndo_open() (um_vec2_netdev_open) which dispatches into the host
	 * backend.  The TAP backend has no vdev->netdev NULL guard and
	 * dereferences dev->mtu in um_vec2_runtime_frame_len(); without
	 * this ordering a NULL deref is possible in the window between
	 * register_netdevice() returning and the post-register
	 * vdev->netdev = dev assignment.  See audit R1.
	 *
	 * On any failure below, NULL the field back out so an out-path
	 * free_netdev() doesn't leave vdev->netdev pointing at freed memory.
	 */
	vdev->netdev = dev;

	rtnl_lock();
	ret = register_netdevice(dev);
	rtnl_unlock();
	if (ret) {
		vdev->netdev = NULL;
		goto out_free_netdev;
	}

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_REGISTERED);
	if (ret) {
		vdev->netdev = NULL;
		goto out_unregister_netdev;
	}

	vdev->registered_queues = queues;
	pr_info("registered netdev %s for vec2.%u\n", dev->name, vdev->unit);
	return 0;

out_unregister_netdev:
	unregister_netdev(dev);
out_free_netdev:
	free_netdev(dev);
	return ret;
}

void um_vec2_netdev_unregister(struct um_vec2_dev *vdev)
{
	struct net_device *dev = vdev->netdev;

	if (!dev)
		return;

	vdev->netdev = NULL;
	vdev->registered_queues = 0;
	unregister_netdev(dev);
	free_netdev(dev);
}
