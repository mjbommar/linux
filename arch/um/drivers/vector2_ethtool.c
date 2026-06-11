// SPDX-License-Identifier: GPL-2.0
/*
 * Ethtool surface for UML vector networking v2.
 *
 * These callbacks deliberately tolerate stopped devices: runtime-only queue
 * pointers are sampled only after NULL checks and while holding the v2
 * lifecycle mutex.  Queue counters are protected by their queue locks with
 * bottom halves disabled because NAPI takes the same locks from softirq.
 */

#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/string.h>

#include "vector2_internal.h"

enum um_vec2_ethtool_stat {
	UM_VEC2_ETHTOOL_STAT_LIFECYCLE_STATE,
	UM_VEC2_ETHTOOL_STAT_TRANSPORT,
	UM_VEC2_ETHTOOL_STAT_HOST_MODE,
	UM_VEC2_ETHTOOL_STAT_CONFIGURED_DEPTH,
	UM_VEC2_ETHTOOL_STAT_CONFIGURED_QUEUES,
	UM_VEC2_ETHTOOL_STAT_CONFIGURED_MTU,
	UM_VEC2_ETHTOOL_STAT_CONFIGURED_COALESCE_USECS,
	UM_VEC2_ETHTOOL_STAT_VNET_HDR,
	UM_VEC2_ETHTOOL_STAT_OPEN_ATTEMPTS,
	UM_VEC2_ETHTOOL_STAT_OPEN_FAILURES,
	UM_VEC2_ETHTOOL_STAT_CLOSES,
	UM_VEC2_ETHTOOL_STAT_NAPI_POLLS,
	UM_VEC2_ETHTOOL_STAT_RX_IRQS,
	UM_VEC2_ETHTOOL_STAT_TX_RETRY_WAKEUPS,
	UM_VEC2_ETHTOOL_STAT_TX_XMIT_CALLS,
	UM_VEC2_ETHTOOL_STAT_TX_BUSY,
	UM_VEC2_ETHTOOL_STAT_TX_DROPPED,
	UM_VEC2_ETHTOOL_STAT_TX_TRANSIENT_ERRORS,
	UM_VEC2_ETHTOOL_STAT_TX_FATAL_ERRORS,
	UM_VEC2_ETHTOOL_STAT_RX_ALLOC_ERRORS,
	UM_VEC2_ETHTOOL_STAT_RX_PROTO_DROPS,
	UM_VEC2_ETHTOOL_STAT_RX_FATAL_ERRORS,
	UM_VEC2_ETHTOOL_STAT_BACKEND_DEAD,
	UM_VEC2_ETHTOOL_STAT_TX_RING_DEPTH,
	UM_VEC2_ETHTOOL_STAT_TX_RING_USED,
	UM_VEC2_ETHTOOL_STAT_TX_RING_MAX_USED,
	UM_VEC2_ETHTOOL_STAT_TX_RING_ENQUEUED,
	UM_VEC2_ETHTOOL_STAT_TX_RING_COMPLETED,
	UM_VEC2_ETHTOOL_STAT_TX_RING_RELEASED,
	UM_VEC2_ETHTOOL_STAT_RX_BATCH_DEPTH,
	UM_VEC2_ETHTOOL_STAT_RX_BATCH_FILLED,
	UM_VEC2_ETHTOOL_STAT_RX_BATCH_PREPARED_TOTAL,
	UM_VEC2_ETHTOOL_STAT_RX_BATCH_RECEIVED_TOTAL,
	UM_VEC2_ETHTOOL_STAT_RX_BATCH_CONSUMED_TOTAL,
	UM_VEC2_ETHTOOL_STAT_RX_BATCH_RELEASED_TOTAL,
	UM_VEC2_ETHTOOL_STAT_MAX,
};

enum um_vec2_ethtool_queue_stat {
	UM_VEC2_ETHTOOL_QUEUE_TX_RING_DEPTH,
	UM_VEC2_ETHTOOL_QUEUE_TX_RING_USED,
	UM_VEC2_ETHTOOL_QUEUE_TX_RING_MAX_USED,
	UM_VEC2_ETHTOOL_QUEUE_TX_RING_ENQUEUED,
	UM_VEC2_ETHTOOL_QUEUE_TX_RING_COMPLETED,
	UM_VEC2_ETHTOOL_QUEUE_TX_RING_RELEASED,
	UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_DEPTH,
	UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_FILLED,
	UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_PREPARED_TOTAL,
	UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_RECEIVED_TOTAL,
	UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_CONSUMED_TOTAL,
	UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_RELEASED_TOTAL,
	UM_VEC2_ETHTOOL_QUEUE_STAT_MAX,
};

static const char um_vec2_ethtool_stat_names
	[UM_VEC2_ETHTOOL_STAT_MAX][ETH_GSTRING_LEN] = {
	[UM_VEC2_ETHTOOL_STAT_LIFECYCLE_STATE] =
		"lifecycle_state",
	[UM_VEC2_ETHTOOL_STAT_TRANSPORT] =
		"transport",
	[UM_VEC2_ETHTOOL_STAT_HOST_MODE] =
		"host_mode",
	[UM_VEC2_ETHTOOL_STAT_CONFIGURED_DEPTH] =
		"configured_depth",
	[UM_VEC2_ETHTOOL_STAT_CONFIGURED_QUEUES] =
		"configured_queues",
	[UM_VEC2_ETHTOOL_STAT_CONFIGURED_MTU] =
		"configured_mtu",
	[UM_VEC2_ETHTOOL_STAT_CONFIGURED_COALESCE_USECS] =
		"configured_coalesce_usecs",
	[UM_VEC2_ETHTOOL_STAT_VNET_HDR] =
		"vnet_hdr_enabled",
	[UM_VEC2_ETHTOOL_STAT_OPEN_ATTEMPTS] =
		"open_attempts",
	[UM_VEC2_ETHTOOL_STAT_OPEN_FAILURES] =
		"open_failures",
	[UM_VEC2_ETHTOOL_STAT_CLOSES] =
		"closes",
	[UM_VEC2_ETHTOOL_STAT_NAPI_POLLS] =
		"napi_polls",
	[UM_VEC2_ETHTOOL_STAT_RX_IRQS] =
		"rx_irqs",
	[UM_VEC2_ETHTOOL_STAT_TX_RETRY_WAKEUPS] =
		"tx_retry_wakeups",
	[UM_VEC2_ETHTOOL_STAT_TX_XMIT_CALLS] =
		"tx_xmit_calls",
	[UM_VEC2_ETHTOOL_STAT_TX_BUSY] =
		"tx_busy",
	[UM_VEC2_ETHTOOL_STAT_TX_DROPPED] =
		"tx_dropped",
	[UM_VEC2_ETHTOOL_STAT_TX_TRANSIENT_ERRORS] =
		"tx_transient_errors",
	[UM_VEC2_ETHTOOL_STAT_TX_FATAL_ERRORS] =
		"tx_fatal_errors",
	[UM_VEC2_ETHTOOL_STAT_RX_ALLOC_ERRORS] =
		"rx_alloc_errors",
	[UM_VEC2_ETHTOOL_STAT_RX_PROTO_DROPS] =
		"rx_proto_drops",
	[UM_VEC2_ETHTOOL_STAT_RX_FATAL_ERRORS] =
		"rx_fatal_errors",
	[UM_VEC2_ETHTOOL_STAT_BACKEND_DEAD] =
		"backend_dead",
	[UM_VEC2_ETHTOOL_STAT_TX_RING_DEPTH] =
		"tx_ring_depth",
	[UM_VEC2_ETHTOOL_STAT_TX_RING_USED] =
		"tx_ring_used",
	[UM_VEC2_ETHTOOL_STAT_TX_RING_MAX_USED] =
		"tx_ring_max_used",
	[UM_VEC2_ETHTOOL_STAT_TX_RING_ENQUEUED] =
		"tx_ring_enqueued",
	[UM_VEC2_ETHTOOL_STAT_TX_RING_COMPLETED] =
		"tx_ring_completed",
	[UM_VEC2_ETHTOOL_STAT_TX_RING_RELEASED] =
		"tx_ring_released",
	[UM_VEC2_ETHTOOL_STAT_RX_BATCH_DEPTH] =
		"rx_batch_depth",
	[UM_VEC2_ETHTOOL_STAT_RX_BATCH_FILLED] =
		"rx_batch_filled",
	[UM_VEC2_ETHTOOL_STAT_RX_BATCH_PREPARED_TOTAL] =
		"rx_batch_prepared_total",
	[UM_VEC2_ETHTOOL_STAT_RX_BATCH_RECEIVED_TOTAL] =
		"rx_batch_received_total",
	[UM_VEC2_ETHTOOL_STAT_RX_BATCH_CONSUMED_TOTAL] =
		"rx_batch_consumed_total",
	[UM_VEC2_ETHTOOL_STAT_RX_BATCH_RELEASED_TOTAL] =
		"rx_batch_released_total",
};

static const char * const um_vec2_ethtool_queue_stat_names
		[UM_VEC2_ETHTOOL_QUEUE_STAT_MAX] = {
	[UM_VEC2_ETHTOOL_QUEUE_TX_RING_DEPTH] =
		"tx_ring_depth",
	[UM_VEC2_ETHTOOL_QUEUE_TX_RING_USED] =
		"tx_ring_used",
	[UM_VEC2_ETHTOOL_QUEUE_TX_RING_MAX_USED] =
		"tx_ring_max_used",
	[UM_VEC2_ETHTOOL_QUEUE_TX_RING_ENQUEUED] =
		"tx_ring_enqueued",
	[UM_VEC2_ETHTOOL_QUEUE_TX_RING_COMPLETED] =
		"tx_ring_completed",
	[UM_VEC2_ETHTOOL_QUEUE_TX_RING_RELEASED] =
		"tx_ring_released",
	[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_DEPTH] =
		"rx_batch_depth",
	[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_FILLED] =
		"rx_batch_filled",
	[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_PREPARED_TOTAL] =
		"rx_batch_prepared_total",
	[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_RECEIVED_TOTAL] =
		"rx_batch_received_total",
	[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_CONSUMED_TOTAL] =
		"rx_batch_consumed_total",
		[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_RELEASED_TOTAL] =
			"rx_batch_released_total",
	};

struct um_vec2_ethtool_counter_stat {
	enum um_vec2_ethtool_stat stat;
	enum um_vec2_stat_counter counter;
};

static const struct um_vec2_ethtool_counter_stat um_vec2_counter_stats[] = {
	{ UM_VEC2_ETHTOOL_STAT_OPEN_ATTEMPTS, UM_VEC2_STAT_OPEN_ATTEMPTS },
	{ UM_VEC2_ETHTOOL_STAT_OPEN_FAILURES, UM_VEC2_STAT_OPEN_FAILURES },
	{ UM_VEC2_ETHTOOL_STAT_CLOSES, UM_VEC2_STAT_CLOSES },
	{ UM_VEC2_ETHTOOL_STAT_NAPI_POLLS, UM_VEC2_STAT_NAPI_POLLS },
	{ UM_VEC2_ETHTOOL_STAT_RX_IRQS, UM_VEC2_STAT_RX_IRQS },
	{ UM_VEC2_ETHTOOL_STAT_TX_RETRY_WAKEUPS,
	  UM_VEC2_STAT_TX_RETRY_WAKEUPS },
	{ UM_VEC2_ETHTOOL_STAT_TX_XMIT_CALLS, UM_VEC2_STAT_TX_XMIT_CALLS },
	{ UM_VEC2_ETHTOOL_STAT_TX_BUSY, UM_VEC2_STAT_TX_BUSY },
	{ UM_VEC2_ETHTOOL_STAT_TX_DROPPED, UM_VEC2_STAT_TX_DROPPED },
	{ UM_VEC2_ETHTOOL_STAT_TX_TRANSIENT_ERRORS,
	  UM_VEC2_STAT_TX_TRANSIENT_ERRORS },
	{ UM_VEC2_ETHTOOL_STAT_TX_FATAL_ERRORS,
	  UM_VEC2_STAT_TX_FATAL_ERRORS },
	{ UM_VEC2_ETHTOOL_STAT_RX_ALLOC_ERRORS, UM_VEC2_STAT_RX_ALLOC_ERRORS },
	{ UM_VEC2_ETHTOOL_STAT_RX_PROTO_DROPS, UM_VEC2_STAT_RX_PROTO_DROPS },
	{ UM_VEC2_ETHTOOL_STAT_RX_FATAL_ERRORS, UM_VEC2_STAT_RX_FATAL_ERRORS },
	{ UM_VEC2_ETHTOOL_STAT_BACKEND_DEAD, UM_VEC2_STAT_BACKEND_DEAD },
};

static unsigned int um_vec2_ethtool_queue_count(struct um_vec2_dev *vdev)
{
	unsigned int queues = max(vdev->cfg.queues, vdev->registered_queues);

	return max(queues, vdev->num_channels);
}

static void um_vec2_get_drvinfo(struct net_device *dev,
				struct ethtool_drvinfo *info)
{
	strscpy(info->driver, UM_VEC2_DRIVER_NAME, sizeof(info->driver));
	strscpy(info->version, "2", sizeof(info->version));
	strscpy(info->bus_info, "uml", sizeof(info->bus_info));
}

static int um_vec2_get_sset_count(struct net_device *dev, int sset)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);
	unsigned int queues;

	switch (sset) {
	case ETH_SS_STATS:
		mutex_lock(&vdev->lock);
		queues = um_vec2_ethtool_queue_count(vdev);
		mutex_unlock(&vdev->lock);
		return UM_VEC2_ETHTOOL_STAT_MAX +
		       queues * UM_VEC2_ETHTOOL_QUEUE_STAT_MAX;
	default:
		return -EOPNOTSUPP;
	}
}

static void um_vec2_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);
	unsigned int queues;
	unsigned int q;
	unsigned int s;

	if (stringset != ETH_SS_STATS)
		return;

	memcpy(data, um_vec2_ethtool_stat_names,
	       sizeof(um_vec2_ethtool_stat_names));

	mutex_lock(&vdev->lock);
	queues = um_vec2_ethtool_queue_count(vdev);
	mutex_unlock(&vdev->lock);

	data += sizeof(um_vec2_ethtool_stat_names);
	for (q = 0; q < queues; q++) {
		for (s = 0; s < UM_VEC2_ETHTOOL_QUEUE_STAT_MAX; s++) {
			snprintf(data, ETH_GSTRING_LEN, "queue%u_%s", q,
				 um_vec2_ethtool_queue_stat_names[s]);
			data += ETH_GSTRING_LEN;
		}
	}
}

static void um_vec2_get_ringparam(struct net_device *dev,
				  struct ethtool_ringparam *ring,
				  struct kernel_ethtool_ringparam *kernel_ring,
				  struct netlink_ext_ack *extack)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);

	mutex_lock(&vdev->lock);
	ring->rx_max_pending = UM_VEC2_MAX_DEPTH;
	ring->tx_max_pending = UM_VEC2_MAX_DEPTH;
	ring->rx_pending = vdev->cfg.depth;
	ring->tx_pending = vdev->cfg.depth;
	mutex_unlock(&vdev->lock);
}

static int um_vec2_set_ringparam(struct net_device *dev,
				 struct ethtool_ringparam *ring,
				 struct kernel_ethtool_ringparam *kernel_ring,
				 struct netlink_ext_ack *extack)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);
	int ret = 0;

	if (ring->rx_mini_pending || ring->rx_jumbo_pending)
		return -EINVAL;
	if (!ring->rx_pending || !ring->tx_pending)
		return -EINVAL;
	if (ring->rx_pending != ring->tx_pending)
		return -EINVAL;
	if (ring->rx_pending > UM_VEC2_MAX_DEPTH)
		return -EINVAL;

	mutex_lock(&vdev->lock);
	if (vdev->life.state != UM_VEC2_DEV_REGISTERED &&
	    vdev->life.state != UM_VEC2_DEV_CONFIGURED) {
		ret = -EBUSY;
		goto out;
	}
	if (!vdev->cfg.batching && ring->rx_pending != 1) {
		ret = -EINVAL;
		goto out;
	}

	vdev->cfg.depth = ring->rx_pending;

out:
	mutex_unlock(&vdev->lock);
	return ret;
}

static int um_vec2_get_coalesce(struct net_device *dev,
				struct ethtool_coalesce *coal,
				struct kernel_ethtool_coalesce *kernel_coal,
				struct netlink_ext_ack *extack)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);

	mutex_lock(&vdev->lock);
	coal->rx_coalesce_usecs = vdev->cfg.coalesce_usecs;
	mutex_unlock(&vdev->lock);
	return 0;
}

static void um_vec2_clear_ethtool_stats(u64 *data, unsigned int queues)
{
	memset(data, 0,
	       sizeof(u64) * (UM_VEC2_ETHTOOL_STAT_MAX +
			      queues * UM_VEC2_ETHTOOL_QUEUE_STAT_MAX));
}

static void um_vec2_sample_config_stats(const struct um_vec2_dev *vdev,
					u64 *data)
{
	data[UM_VEC2_ETHTOOL_STAT_LIFECYCLE_STATE] = vdev->life.state;
	data[UM_VEC2_ETHTOOL_STAT_TRANSPORT] = vdev->cfg.transport;
	data[UM_VEC2_ETHTOOL_STAT_HOST_MODE] = vdev->cfg.mode;
	data[UM_VEC2_ETHTOOL_STAT_CONFIGURED_DEPTH] = vdev->cfg.depth;
	data[UM_VEC2_ETHTOOL_STAT_CONFIGURED_QUEUES] = vdev->cfg.queues;
	data[UM_VEC2_ETHTOOL_STAT_CONFIGURED_MTU] = vdev->cfg.mtu;
	data[UM_VEC2_ETHTOOL_STAT_CONFIGURED_COALESCE_USECS] =
		vdev->cfg.coalesce_usecs;
	data[UM_VEC2_ETHTOOL_STAT_VNET_HDR] = 0;
	if (vdev->cfg.transport == UM_VEC2_TRANSPORT_TAP)
		data[UM_VEC2_ETHTOOL_STAT_VNET_HDR] = 1;
}

static void um_vec2_sample_counter_stats(const struct um_vec2_dev *vdev,
					 u64 *data)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(um_vec2_counter_stats); i++)
		data[um_vec2_counter_stats[i].stat] =
			um_vec2_stat_read(vdev, um_vec2_counter_stats[i].counter);
}

static u64 *um_vec2_queue_stat_data(u64 *data, unsigned int q)
{
	return data + UM_VEC2_ETHTOOL_STAT_MAX +
	       q * UM_VEC2_ETHTOOL_QUEUE_STAT_MAX;
}

static void um_vec2_sample_tx_stats(struct um_vec2_queue_pair *queue,
				    u64 *data, u64 *qdata)
{
	spin_lock_bh(&queue->tx_lock);
	data[UM_VEC2_ETHTOOL_STAT_TX_RING_DEPTH] += queue->tx.depth;
	data[UM_VEC2_ETHTOOL_STAT_TX_RING_USED] += queue->tx.count;
	data[UM_VEC2_ETHTOOL_STAT_TX_RING_MAX_USED] += queue->tx.max_count;
	data[UM_VEC2_ETHTOOL_STAT_TX_RING_ENQUEUED] += queue->tx.enqueued;
	data[UM_VEC2_ETHTOOL_STAT_TX_RING_COMPLETED] += queue->tx.completed;
	data[UM_VEC2_ETHTOOL_STAT_TX_RING_RELEASED] += queue->tx.released;

	if (qdata) {
		qdata[UM_VEC2_ETHTOOL_QUEUE_TX_RING_DEPTH] = queue->tx.depth;
		qdata[UM_VEC2_ETHTOOL_QUEUE_TX_RING_USED] = queue->tx.count;
		qdata[UM_VEC2_ETHTOOL_QUEUE_TX_RING_MAX_USED] =
			queue->tx.max_count;
		qdata[UM_VEC2_ETHTOOL_QUEUE_TX_RING_ENQUEUED] =
			queue->tx.enqueued;
		qdata[UM_VEC2_ETHTOOL_QUEUE_TX_RING_COMPLETED] =
			queue->tx.completed;
		qdata[UM_VEC2_ETHTOOL_QUEUE_TX_RING_RELEASED] =
			queue->tx.released;
	}
	spin_unlock_bh(&queue->tx_lock);
}

static void um_vec2_sample_rx_stats(struct um_vec2_queue_pair *queue,
				    u64 *data, u64 *qdata)
{
	spin_lock_bh(&queue->rx_lock);
	data[UM_VEC2_ETHTOOL_STAT_RX_BATCH_DEPTH] += queue->rx.depth;
	data[UM_VEC2_ETHTOOL_STAT_RX_BATCH_FILLED] += queue->rx.filled;
	data[UM_VEC2_ETHTOOL_STAT_RX_BATCH_PREPARED_TOTAL] +=
		queue->rx.prepared_total;
	data[UM_VEC2_ETHTOOL_STAT_RX_BATCH_RECEIVED_TOTAL] +=
		queue->rx.received_total;
	data[UM_VEC2_ETHTOOL_STAT_RX_BATCH_CONSUMED_TOTAL] +=
		queue->rx.consumed_total;
	data[UM_VEC2_ETHTOOL_STAT_RX_BATCH_RELEASED_TOTAL] +=
		queue->rx.released_total;

	if (qdata) {
		qdata[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_DEPTH] = queue->rx.depth;
		qdata[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_FILLED] = queue->rx.filled;
		qdata[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_PREPARED_TOTAL] =
			queue->rx.prepared_total;
		qdata[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_RECEIVED_TOTAL] =
			queue->rx.received_total;
		qdata[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_CONSUMED_TOTAL] =
			queue->rx.consumed_total;
		qdata[UM_VEC2_ETHTOOL_QUEUE_RX_BATCH_RELEASED_TOTAL] =
			queue->rx.released_total;
	}
	spin_unlock_bh(&queue->rx_lock);
}

static void um_vec2_sample_channel_stats(struct um_vec2_channel *channel,
					 u64 *data, unsigned int queues)
{
	struct um_vec2_queue_pair *queue = channel->queue;
	u64 *qdata = NULL;

	if (channel->vnet_hdr)
		data[UM_VEC2_ETHTOOL_STAT_VNET_HDR] = 1;

	if (!queue)
		return;
	if (channel->index < queues)
		qdata = um_vec2_queue_stat_data(data, channel->index);

	um_vec2_sample_tx_stats(queue, data, qdata);
	um_vec2_sample_rx_stats(queue, data, qdata);
}

static void um_vec2_sample_queue_stats(struct um_vec2_dev *vdev, u64 *data,
				       unsigned int queues)
{
	struct um_vec2_channel *channel;

	for (channel = vdev->channels;
	     channel && channel < vdev->channels + vdev->num_channels;
	     channel++)
		um_vec2_sample_channel_stats(channel, data, queues);
}

static void um_vec2_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	struct um_vec2_dev *vdev = um_vec2_dev_from_netdev(dev);
	unsigned int queues;

	mutex_lock(&vdev->lock);
	queues = um_vec2_ethtool_queue_count(vdev);
	um_vec2_clear_ethtool_stats(data, queues);
	um_vec2_sample_config_stats(vdev, data);
	um_vec2_sample_counter_stats(vdev, data);
	um_vec2_sample_queue_stats(vdev, data, queues);

	mutex_unlock(&vdev->lock);
}

static const struct ethtool_ops um_vec2_ethtool_ops = {
	.supported_coalesce_params = ETHTOOL_COALESCE_RX_USECS,
	.get_drvinfo	= um_vec2_get_drvinfo,
	.get_link	= ethtool_op_get_link,
	.get_sset_count = um_vec2_get_sset_count,
	.get_strings	= um_vec2_get_strings,
	.get_ethtool_stats = um_vec2_get_ethtool_stats,
	.get_ringparam	= um_vec2_get_ringparam,
	.set_ringparam	= um_vec2_set_ringparam,
	.get_coalesce	= um_vec2_get_coalesce,
	.get_ts_info	= ethtool_op_get_ts_info,
};

void um_vec2_ethtool_attach(struct net_device *dev)
{
	dev->ethtool_ops = &um_vec2_ethtool_ops;
}
