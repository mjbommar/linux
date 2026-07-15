// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for UML vector networking v2 ethtool policy.
 */

#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/string.h>

#include <os.h>

#include "vector2_fake_host.h"
#include "vector2_internal.h"
#include "vector2_test.h"

static struct um_vec2_dev *vector2_ethtool_test_alloc_vdev(struct kunit *test,
							   unsigned int unit)
{
	struct um_vec2_dev *vdev;

	vdev = kunit_kzalloc(test, sizeof(*vdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, vdev);

	INIT_LIST_HEAD(&vdev->list);
	mutex_init(&vdev->lock);
	vdev->unit = unit;
	um_vec2_config_init(&vdev->cfg);
	um_vec2_dev_lifecycle_init(&vdev->life);
	KUNIT_ASSERT_EQ(test,
			um_vec2_dev_transition(&vdev->life,
					       UM_VEC2_DEV_CONFIGURED), 0);
	KUNIT_ASSERT_EQ(test,
			um_vec2_dev_transition(&vdev->life,
					       UM_VEC2_DEV_REGISTERED), 0);
	return vdev;
}

static struct net_device *
vector2_ethtool_test_alloc_netdev_mqs(struct kunit *test,
				       struct um_vec2_dev *vdev,
				       unsigned int queues)
{
	struct net_device *dev;

	dev = alloc_etherdev_mqs(sizeof(struct um_vec2_netdev_priv), queues,
				 queues);
	KUNIT_ASSERT_NOT_NULL(test, dev);

	um_vec2_netdev_init(vdev, dev);
	vdev->netdev = dev;
	return dev;
}

static struct net_device *
vector2_ethtool_test_alloc_netdev(struct kunit *test, struct um_vec2_dev *vdev)
{
	return vector2_ethtool_test_alloc_netdev_mqs(test, vdev, 1);
}

static int vector2_ethtool_find_stat(struct kunit *test,
				     struct net_device *dev,
				     const char *name)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	unsigned int count;
	u8 *strings;
	unsigned int i;

	count = ops->get_sset_count(dev, ETH_SS_STATS);
	strings = kunit_kzalloc(test, count * ETH_GSTRING_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, strings);

	ops->get_strings(dev, ETH_SS_STATS, strings);

	for (i = 0; i < count; i++) {
		const char *candidate = strings + i * ETH_GSTRING_LEN;

		if (!strncmp(candidate, name, ETH_GSTRING_LEN))
			return i;
	}

	KUNIT_FAIL(test, "missing ethtool stat '%s'\n", name);
	return -ENOENT;
}

static u64 *vector2_ethtool_test_stats(struct kunit *test,
				       struct net_device *dev,
				       unsigned int *count)
{
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct ethtool_stats stats = {};
	u64 *data;
	int ret;

	ret = ops->get_sset_count(dev, ETH_SS_STATS);
	KUNIT_ASSERT_GT(test, ret, 0);
	*count = ret;

	data = kunit_kcalloc(test, *count, sizeof(*data), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);

	ops->get_ethtool_stats(dev, &stats, data);
	return data;
}

static void vector2_ethtool_assert_stat_id(struct kunit *test, int stat,
					   unsigned int count)
{
	KUNIT_ASSERT_GE(test, stat, 0);
	KUNIT_ASSERT_LT(test, stat, (int)count);
}

static void vector2_ethtool_stats_stopped_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_ethtool_test_alloc_vdev(test, 0);
	struct net_device *dev = vector2_ethtool_test_alloc_netdev(test, vdev);
	const struct ethtool_ops *ops = dev->ethtool_ops;
	unsigned int count;
	int lifecycle;
	int depth;
	u64 *data;

	KUNIT_ASSERT_NOT_NULL(test, ops);
	KUNIT_ASSERT_NOT_NULL(test, ops->get_sset_count);
	KUNIT_ASSERT_NOT_NULL(test, ops->get_strings);
	KUNIT_ASSERT_NOT_NULL(test, ops->get_ethtool_stats);

	data = vector2_ethtool_test_stats(test, dev, &count);
	lifecycle = vector2_ethtool_find_stat(test, dev, "lifecycle_state");
	depth = vector2_ethtool_find_stat(test, dev, "configured_depth");

	KUNIT_ASSERT_GE(test, lifecycle, 0);
	KUNIT_ASSERT_GE(test, depth, 0);
	KUNIT_ASSERT_LT(test, lifecycle, (int)count);
	KUNIT_ASSERT_LT(test, depth, (int)count);
	KUNIT_EXPECT_EQ(test, data[lifecycle], (u64)UM_VEC2_DEV_REGISTERED);
	KUNIT_EXPECT_EQ(test, data[depth], (u64)UM_VEC2_DEFAULT_DEPTH);

	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_ethtool_ring_policy_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_ethtool_test_alloc_vdev(test, 1);
	struct net_device *dev = vector2_ethtool_test_alloc_netdev(test, vdev);
	const struct ethtool_ops *ops = dev->ethtool_ops;
	struct kernel_ethtool_ringparam kernel_ring = {};
	struct ethtool_ringparam ring = {};

	KUNIT_ASSERT_NOT_NULL(test, ops->get_ringparam);
	KUNIT_ASSERT_NOT_NULL(test, ops->set_ringparam);

	ops->get_ringparam(dev, &ring, &kernel_ring, NULL);
	KUNIT_EXPECT_EQ(test, ring.rx_max_pending, UM_VEC2_MAX_DEPTH);
	KUNIT_EXPECT_EQ(test, ring.tx_max_pending, UM_VEC2_MAX_DEPTH);
	KUNIT_EXPECT_EQ(test, ring.rx_pending, UM_VEC2_DEFAULT_DEPTH);
	KUNIT_EXPECT_EQ(test, ring.tx_pending, UM_VEC2_DEFAULT_DEPTH);

	memset(&ring, 0, sizeof(ring));
	ring.rx_pending = 128;
	ring.tx_pending = 128;
	KUNIT_EXPECT_EQ(test, ops->set_ringparam(dev, &ring, &kernel_ring,
						 NULL), 0);
	KUNIT_EXPECT_EQ(test, vdev->cfg.depth, 128U);

	ring.tx_pending = 64;
	KUNIT_EXPECT_EQ(test, ops->set_ringparam(dev, &ring, &kernel_ring,
						 NULL), -EINVAL);

	KUNIT_ASSERT_EQ(test,
			um_vec2_dev_transition(&vdev->life,
					       UM_VEC2_DEV_OPENING), 0);
	KUNIT_ASSERT_EQ(test,
			um_vec2_dev_transition(&vdev->life,
					       UM_VEC2_DEV_RUNNING), 0);
	ring.tx_pending = ring.rx_pending;
	KUNIT_EXPECT_EQ(test, ops->set_ringparam(dev, &ring, &kernel_ring,
						 NULL), -EBUSY);

	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_ethtool_xmit_drop_stats_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_ethtool_test_alloc_vdev(test, 2);
	struct net_device *dev = vector2_ethtool_test_alloc_netdev(test, vdev);
	struct sk_buff *skb;
	unsigned int count;
	int xmit_calls;
	int tx_dropped;
	u64 *data;

	skb = alloc_skb(64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_EXPECT_EQ(test, um_vec2_netdev_start_xmit(skb, dev),
			NETDEV_TX_OK);

	data = vector2_ethtool_test_stats(test, dev, &count);
	xmit_calls = vector2_ethtool_find_stat(test, dev, "tx_xmit_calls");
	tx_dropped = vector2_ethtool_find_stat(test, dev, "tx_dropped");

	KUNIT_ASSERT_GE(test, xmit_calls, 0);
	KUNIT_ASSERT_GE(test, tx_dropped, 0);
	KUNIT_ASSERT_LT(test, xmit_calls, (int)count);
	KUNIT_ASSERT_LT(test, tx_dropped, (int)count);
	KUNIT_EXPECT_EQ(test, data[xmit_calls], 1ULL);
	KUNIT_EXPECT_EQ(test, data[tx_dropped], 1ULL);

	vdev->netdev = NULL;
	free_netdev(dev);
}

struct vector2_ethtool_depth_stat_ids {
	int tx_depth;
	int rx_depth;
	int queue_tx_depth[2];
	int queue_rx_depth[2];
};

static void vector2_ethtool_attach_test_channels(struct kunit *test,
						 struct um_vec2_dev *vdev,
						 unsigned int count,
						 unsigned int depth)
{
	struct um_vec2_channel *channels;
	struct um_vec2_queue_pair *queues;
	struct um_vec2_tx_desc *tx_desc;
	struct um_vec2_rx_slot *rx_slot;
	unsigned int i;

	channels = kunit_kcalloc(test, count, sizeof(*channels), GFP_KERNEL);
	queues = kunit_kcalloc(test, count, sizeof(*queues), GFP_KERNEL);
	tx_desc = kunit_kcalloc(test, count * depth, sizeof(*tx_desc),
				GFP_KERNEL);
	rx_slot = kunit_kcalloc(test, count * depth, sizeof(*rx_slot),
				GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, channels);
	KUNIT_ASSERT_NOT_NULL(test, queues);
	KUNIT_ASSERT_NOT_NULL(test, tx_desc);
	KUNIT_ASSERT_NOT_NULL(test, rx_slot);

	for (i = 0; i < count; i++) {
		channels[i].vdev = vdev;
		channels[i].index = i;
		channels[i].queue = &queues[i];
		spin_lock_init(&queues[i].tx_lock);
		spin_lock_init(&queues[i].rx_lock);
		KUNIT_ASSERT_EQ(test,
				um_vec2_tx_ring_init(&queues[i].tx,
						     &tx_desc[i * depth],
						     depth),
				0);
		KUNIT_ASSERT_EQ(test,
				um_vec2_rx_batch_init(&queues[i].rx,
						      &rx_slot[i * depth],
						      depth),
				0);
	}
	vdev->channels = channels;
	vdev->num_channels = count;
}

static void
vector2_ethtool_find_depth_stats(struct kunit *test, struct net_device *dev,
				 unsigned int count,
				 struct vector2_ethtool_depth_stat_ids *ids)
{
	ids->tx_depth = vector2_ethtool_find_stat(test, dev, "tx_ring_depth");
	ids->rx_depth = vector2_ethtool_find_stat(test, dev, "rx_batch_depth");
	ids->queue_tx_depth[0] =
		vector2_ethtool_find_stat(test, dev, "queue0_tx_ring_depth");
	ids->queue_tx_depth[1] =
		vector2_ethtool_find_stat(test, dev, "queue1_tx_ring_depth");
	ids->queue_rx_depth[0] =
		vector2_ethtool_find_stat(test, dev, "queue0_rx_batch_depth");
	ids->queue_rx_depth[1] =
		vector2_ethtool_find_stat(test, dev, "queue1_rx_batch_depth");

	vector2_ethtool_assert_stat_id(test, ids->tx_depth, count);
	vector2_ethtool_assert_stat_id(test, ids->rx_depth, count);
	vector2_ethtool_assert_stat_id(test, ids->queue_tx_depth[0], count);
	vector2_ethtool_assert_stat_id(test, ids->queue_tx_depth[1], count);
	vector2_ethtool_assert_stat_id(test, ids->queue_rx_depth[0], count);
	vector2_ethtool_assert_stat_id(test, ids->queue_rx_depth[1], count);
}

static void
vector2_ethtool_expect_depth_stats(struct kunit *test, const u64 *data,
				   const struct vector2_ethtool_depth_stat_ids *ids)
{
	KUNIT_EXPECT_EQ(test, data[ids->tx_depth], 4ULL);
	KUNIT_EXPECT_EQ(test, data[ids->rx_depth], 4ULL);
	KUNIT_EXPECT_EQ(test, data[ids->queue_tx_depth[0]], 2ULL);
	KUNIT_EXPECT_EQ(test, data[ids->queue_tx_depth[1]], 2ULL);
	KUNIT_EXPECT_EQ(test, data[ids->queue_rx_depth[0]], 2ULL);
	KUNIT_EXPECT_EQ(test, data[ids->queue_rx_depth[1]], 2ULL);
}

static void vector2_ethtool_multiqueue_stats_aggregate_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_ethtool_test_alloc_vdev(test, 3);
	struct net_device *dev = vector2_ethtool_test_alloc_netdev(test, vdev);
	struct vector2_ethtool_depth_stat_ids ids;
	unsigned int count;
	u64 *data;

	vector2_ethtool_attach_test_channels(test, vdev, 2, 2);
	data = vector2_ethtool_test_stats(test, dev, &count);
	vector2_ethtool_find_depth_stats(test, dev, count, &ids);
	vector2_ethtool_expect_depth_stats(test, data, &ids);

	vdev->channels = NULL;
	vdev->num_channels = 0;
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_ethtool_vnet_hdr_runtime_fd_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_ethtool_test_alloc_vdev(test, 5);
	struct net_device *dev = vector2_ethtool_test_alloc_netdev(test, vdev);
	unsigned int count;
	int vnet_hdr;
	u64 *data;

	vdev->cfg.transport = UM_VEC2_TRANSPORT_FD;
	vector2_ethtool_attach_test_channels(test, vdev, 2, 2);
	vnet_hdr = vector2_ethtool_find_stat(test, dev, "vnet_hdr_enabled");
	KUNIT_ASSERT_GE(test, vnet_hdr, 0);

	data = vector2_ethtool_test_stats(test, dev, &count);
	vector2_ethtool_assert_stat_id(test, vnet_hdr, count);
	KUNIT_EXPECT_EQ(test, data[vnet_hdr], 0ULL);

	vdev->channels[1].vnet_hdr = true;
	data = vector2_ethtool_test_stats(test, dev, &count);
	KUNIT_EXPECT_EQ(test, data[vnet_hdr], 1ULL);

	vdev->channels = NULL;
	vdev->num_channels = 0;
	vdev->netdev = NULL;
	free_netdev(dev);
}

/*
 * Exercise um_vec2_get_ethtool_stats' per-queue spin_lock_bh(tx_lock)
 * and spin_lock_bh(rx_lock) paths against a live fake-host-backed
 * device. KUnit cannot easily run parallel threads in UML, so this
 * test interleaves enqueue, partial drain via the NAPI poll, ethtool
 * stats read, and a second drain. That proves the spin_lock_bh path
 * does not deadlock against an actively served queue and that the
 * per-queue counters reflect the traffic round.
 */
struct vector2_ethtool_traffic_channel {
	struct um_vec2_fake_host fake;
	struct um_vec2_host *saved_host;
};

struct vector2_ethtool_traffic_ctx {
	struct um_vec2_dev *vdev;
	struct net_device *dev;
	int fds[2];
	struct vector2_ethtool_traffic_channel ch[2];
};

struct vector2_ethtool_traffic_stat_ids {
	int tx_used[2];
	int rx_received[2];
	int tx_enqueued;
	int rx_received_total;
};

static void vector2_ethtool_traffic_cleanup(void *data)
{
	struct vector2_ethtool_traffic_ctx *ctx = data;
	unsigned int i;

	if (ctx->vdev && ctx->vdev->channels) {
		for (i = 0; i < ARRAY_SIZE(ctx->ch); i++) {
			if (ctx->ch[i].saved_host)
				ctx->vdev->channels[i].host =
					ctx->ch[i].saved_host;
		}
	}

	if (ctx->dev)
		um_vec2_netdev_stop(ctx->dev);

	for (i = 0; i < ARRAY_SIZE(ctx->fds); i++) {
		if (ctx->fds[i] >= 0) {
			os_close_file(ctx->fds[i]);
			ctx->fds[i] = -1;
		}
	}

	if (ctx->vdev)
		ctx->vdev->netdev = NULL;
	if (ctx->dev)
		free_netdev(ctx->dev);
}

static void
vector2_ethtool_traffic_attach_fake_hosts(struct vector2_ethtool_traffic_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ctx->ch); i++) {
		um_vec2_fake_host_init(&ctx->ch[i].fake);
		ctx->ch[i].saved_host = ctx->vdev->channels[i].host;
		ctx->vdev->channels[i].host =
			um_vec2_fake_host_base(&ctx->ch[i].fake);
	}
}

static struct vector2_ethtool_traffic_ctx *
vector2_ethtool_traffic_open(struct kunit *test)
{
	struct vector2_ethtool_traffic_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->fds[0] = -1;
	ctx->fds[1] = -1;
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  vector2_ethtool_traffic_cleanup,
						  ctx),
			0);

	ctx->vdev = vector2_ethtool_test_alloc_vdev(test, 4);
	ctx->vdev->cfg.queues = 2;
	ctx->vdev->cfg.transport = UM_VEC2_TRANSPORT_FD;

	ctx->dev = vector2_ethtool_test_alloc_netdev_mqs(test, ctx->vdev, 2);
	KUNIT_ASSERT_EQ(test, netif_set_real_num_tx_queues(ctx->dev, 2), 0);
	KUNIT_ASSERT_EQ(test, netif_set_real_num_rx_queues(ctx->dev, 2), 0);

	KUNIT_ASSERT_EQ(test, um_vec2_test_adjacent_pipe(ctx->fds), 0);
	ctx->vdev->cfg.fd = ctx->fds[0];
	ctx->vdev->cfg.has_fd = true;

	KUNIT_ASSERT_EQ(test, um_vec2_netdev_open(ctx->dev), 0);
	KUNIT_ASSERT_NOT_NULL(test, ctx->vdev->channels);
	KUNIT_ASSERT_EQ(test, ctx->vdev->num_channels, 2U);

	vector2_ethtool_traffic_attach_fake_hosts(ctx);
	return ctx;
}

static int
vector2_ethtool_traffic_invoke_poll(struct vector2_ethtool_traffic_ctx *ctx,
				    unsigned int index, int budget)
{
	struct napi_struct *napi = &ctx->vdev->channels[index].napi;
	int ret;

	clear_bit(NAPI_STATE_MISSED, &napi->state);
	set_bit(NAPI_STATE_SCHED, &napi->state);
	ret = napi->poll(napi, budget);
	clear_bit(NAPI_STATE_SCHED, &napi->state);
	return ret;
}

static void
vector2_ethtool_traffic_stage(struct kunit *test,
			      struct vector2_ethtool_traffic_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ctx->ch); i++) {
		struct um_vec2_channel *channel = &ctx->vdev->channels[i];
		struct sk_buff *skb;

		skb = alloc_skb(64, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, skb);
		skb_put(skb, 64);
		KUNIT_ASSERT_EQ(test,
				um_vec2_tx_ring_enqueue(&channel->queue->tx,
							skb, skb->len),
				0);
		KUNIT_ASSERT_EQ(test,
				um_vec2_fake_host_push_rx(&ctx->ch[i].fake, 64),
				0);
		WRITE_ONCE(channel->rx_pending, true);
	}
}

static void
vector2_ethtool_traffic_find_stats(struct kunit *test, struct net_device *dev,
				   unsigned int count,
				   struct vector2_ethtool_traffic_stat_ids *ids)
{
	ids->tx_used[0] = vector2_ethtool_find_stat(test, dev,
						    "queue0_tx_ring_used");
	ids->tx_used[1] = vector2_ethtool_find_stat(test, dev,
						    "queue1_tx_ring_used");
	ids->rx_received[0] =
		vector2_ethtool_find_stat(test, dev,
					  "queue0_rx_batch_received_total");
	ids->rx_received[1] =
		vector2_ethtool_find_stat(test, dev,
					  "queue1_rx_batch_received_total");
	ids->tx_enqueued = vector2_ethtool_find_stat(test, dev,
						     "tx_ring_enqueued");
	ids->rx_received_total =
		vector2_ethtool_find_stat(test, dev,
					  "rx_batch_received_total");

	vector2_ethtool_assert_stat_id(test, ids->tx_used[0], count);
	vector2_ethtool_assert_stat_id(test, ids->tx_used[1], count);
	vector2_ethtool_assert_stat_id(test, ids->rx_received[0], count);
	vector2_ethtool_assert_stat_id(test, ids->rx_received[1], count);
	vector2_ethtool_assert_stat_id(test, ids->tx_enqueued, count);
	vector2_ethtool_assert_stat_id(test, ids->rx_received_total, count);
}

static void
vector2_ethtool_expect_traffic_queued(struct kunit *test, const u64 *data,
				      const struct vector2_ethtool_traffic_stat_ids *ids)
{
	KUNIT_EXPECT_EQ(test, data[ids->tx_used[0]], 1ULL);
	KUNIT_EXPECT_EQ(test, data[ids->tx_used[1]], 1ULL);
	KUNIT_EXPECT_GE(test, data[ids->tx_enqueued], 2ULL);
}

static void
vector2_ethtool_expect_channel0_drained(struct kunit *test, const u64 *data,
					const struct vector2_ethtool_traffic_stat_ids *ids)
{
	KUNIT_EXPECT_EQ(test, data[ids->tx_used[0]], 0ULL);
	KUNIT_EXPECT_EQ(test, data[ids->tx_used[1]], 1ULL);
	KUNIT_EXPECT_EQ(test, data[ids->rx_received[0]], 1ULL);
	KUNIT_EXPECT_EQ(test, data[ids->rx_received[1]], 0ULL);
}

static void
vector2_ethtool_expect_all_drained(struct kunit *test, const u64 *data,
				   const struct vector2_ethtool_traffic_stat_ids *ids)
{
	KUNIT_EXPECT_EQ(test, data[ids->tx_used[0]], 0ULL);
	KUNIT_EXPECT_EQ(test, data[ids->tx_used[1]], 0ULL);
	KUNIT_EXPECT_EQ(test, data[ids->rx_received[0]], 1ULL);
	KUNIT_EXPECT_EQ(test, data[ids->rx_received[1]], 1ULL);
	KUNIT_EXPECT_GE(test, data[ids->rx_received_total], 2ULL);
}

static void vector2_ethtool_stats_during_traffic_test(struct kunit *test)
{
	struct vector2_ethtool_traffic_stat_ids ids;
	struct vector2_ethtool_traffic_ctx *ctx =
		vector2_ethtool_traffic_open(test);
	unsigned int count;
	u64 *data;

	/*
	 * Enqueue one TX per channel and push one RX per channel.
	 * Read ethtool stats BEFORE the poll runs; the tx_lock /
	 * rx_lock paths see populated queues and must not deadlock.
	 */
	vector2_ethtool_traffic_stage(test, ctx);
	data = vector2_ethtool_test_stats(test, ctx->dev, &count);
	vector2_ethtool_traffic_find_stats(test, ctx->dev, count, &ids);

	/* Before RX drain: TX rings are populated. */
	vector2_ethtool_expect_traffic_queued(test, data, &ids);

	/* Drain channel 0 only, then re-read stats. */
	KUNIT_EXPECT_EQ(test, vector2_ethtool_traffic_invoke_poll(ctx, 0, 4), 1);

	data = vector2_ethtool_test_stats(test, ctx->dev, &count);
	vector2_ethtool_expect_channel0_drained(test, data, &ids);

	/* Drain channel 1 and confirm final stats. */
	KUNIT_EXPECT_EQ(test, vector2_ethtool_traffic_invoke_poll(ctx, 1, 4), 1);

	data = vector2_ethtool_test_stats(test, ctx->dev, &count);
	vector2_ethtool_expect_all_drained(test, data, &ids);
}

static struct kunit_case vector2_ethtool_test_cases[] = {
	KUNIT_CASE(vector2_ethtool_stats_stopped_test),
	KUNIT_CASE(vector2_ethtool_ring_policy_test),
	KUNIT_CASE(vector2_ethtool_xmit_drop_stats_test),
	KUNIT_CASE(vector2_ethtool_multiqueue_stats_aggregate_test),
	KUNIT_CASE(vector2_ethtool_vnet_hdr_runtime_fd_test),
	KUNIT_CASE(vector2_ethtool_stats_during_traffic_test),
	{}
};

static struct kunit_suite vector2_ethtool_test_suite = {
	.name = "um_vector2_ethtool",
	.test_cases = vector2_ethtool_test_cases,
};

kunit_test_suite(vector2_ethtool_test_suite);
