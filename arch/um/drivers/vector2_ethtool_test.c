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
vector2_ethtool_test_alloc_netdev(struct kunit *test, struct um_vec2_dev *vdev)
{
	struct net_device *dev;

	dev = alloc_etherdev_mqs(sizeof(struct um_vec2_netdev_priv), 1, 1);
	KUNIT_ASSERT_NOT_NULL(test, dev);

	um_vec2_netdev_init(vdev, dev);
	vdev->netdev = dev;
	return dev;
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

static void vector2_ethtool_multiqueue_stats_aggregate_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_ethtool_test_alloc_vdev(test, 3);
	struct net_device *dev = vector2_ethtool_test_alloc_netdev(test, vdev);
	struct um_vec2_channel *channels;
	struct um_vec2_queue_pair *queues;
	struct um_vec2_tx_desc *tx_desc;
	struct um_vec2_rx_slot *rx_slot;
	unsigned int count;
	int tx_depth;
	int rx_depth;
	int q0_tx_depth;
	int q1_tx_depth;
	int q0_rx_depth;
	int q1_rx_depth;
	u64 *data;
	unsigned int i;

	channels = kunit_kcalloc(test, 2, sizeof(*channels), GFP_KERNEL);
	queues = kunit_kcalloc(test, 2, sizeof(*queues), GFP_KERNEL);
	tx_desc = kunit_kcalloc(test, 4, sizeof(*tx_desc), GFP_KERNEL);
	rx_slot = kunit_kcalloc(test, 4, sizeof(*rx_slot), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, channels);
	KUNIT_ASSERT_NOT_NULL(test, queues);
	KUNIT_ASSERT_NOT_NULL(test, tx_desc);
	KUNIT_ASSERT_NOT_NULL(test, rx_slot);

	for (i = 0; i < 2; i++) {
		channels[i].vdev = vdev;
		channels[i].index = i;
		channels[i].queue = &queues[i];
		spin_lock_init(&queues[i].tx_lock);
		spin_lock_init(&queues[i].rx_lock);
		KUNIT_ASSERT_EQ(test,
				um_vec2_tx_ring_init(&queues[i].tx,
						     &tx_desc[i * 2], 2), 0);
		KUNIT_ASSERT_EQ(test,
				um_vec2_rx_batch_init(&queues[i].rx,
						      &rx_slot[i * 2], 2), 0);
	}
	vdev->channels = channels;
	vdev->num_channels = 2;

	data = vector2_ethtool_test_stats(test, dev, &count);
	tx_depth = vector2_ethtool_find_stat(test, dev, "tx_ring_depth");
	rx_depth = vector2_ethtool_find_stat(test, dev, "rx_batch_depth");
	q0_tx_depth = vector2_ethtool_find_stat(test, dev,
						"queue0_tx_ring_depth");
	q1_tx_depth = vector2_ethtool_find_stat(test, dev,
						"queue1_tx_ring_depth");
	q0_rx_depth = vector2_ethtool_find_stat(test, dev,
						"queue0_rx_batch_depth");
	q1_rx_depth = vector2_ethtool_find_stat(test, dev,
						"queue1_rx_batch_depth");

	KUNIT_ASSERT_GE(test, tx_depth, 0);
	KUNIT_ASSERT_GE(test, rx_depth, 0);
	KUNIT_ASSERT_GE(test, q0_tx_depth, 0);
	KUNIT_ASSERT_GE(test, q1_tx_depth, 0);
	KUNIT_ASSERT_GE(test, q0_rx_depth, 0);
	KUNIT_ASSERT_GE(test, q1_rx_depth, 0);
	KUNIT_ASSERT_LT(test, tx_depth, (int)count);
	KUNIT_ASSERT_LT(test, rx_depth, (int)count);
	KUNIT_ASSERT_LT(test, q0_tx_depth, (int)count);
	KUNIT_ASSERT_LT(test, q1_tx_depth, (int)count);
	KUNIT_ASSERT_LT(test, q0_rx_depth, (int)count);
	KUNIT_ASSERT_LT(test, q1_rx_depth, (int)count);
	KUNIT_EXPECT_EQ(test, data[tx_depth], 4ULL);
	KUNIT_EXPECT_EQ(test, data[rx_depth], 4ULL);
	KUNIT_EXPECT_EQ(test, data[q0_tx_depth], 2ULL);
	KUNIT_EXPECT_EQ(test, data[q1_tx_depth], 2ULL);
	KUNIT_EXPECT_EQ(test, data[q0_rx_depth], 2ULL);
	KUNIT_EXPECT_EQ(test, data[q1_rx_depth], 2ULL);

	vdev->channels = NULL;
	vdev->num_channels = 0;
	vdev->netdev = NULL;
	free_netdev(dev);
}

/*
 * P3.2 — exercise um_vec2_get_ethtool_stats' per-queue
 * spin_lock_bh(tx_lock) + spin_lock_bh(rx_lock) paths against a live
 * fake-host-backed device.  The previous coverage built channels by
 * hand and never opened the netdev, so the locks were taken with no
 * concurrent path.  KUnit cannot easily run parallel threads in UML;
 * instead this test interleaves enqueue, partial drain via the NAPI
 * poll, ethtool stats read, and a second drain — proving the
 * spin_lock_bh path does not deadlock against an actively-served
 * queue and that the per-queue counters reflect the traffic round.
 * See audit P3.2.
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

static void vector2_ethtool_stats_during_traffic_test(struct kunit *test)
{
	struct vector2_ethtool_traffic_ctx *ctx;
	struct um_vec2_dev *vdev;
	struct net_device *dev;
	const struct ethtool_ops *ops;
	struct sk_buff *skbs[2];
	unsigned int count;
	int q0_tx_used;
	int q1_tx_used;
	int q0_rx_recv;
	int q1_rx_recv;
	int tx_enq;
	int rx_recv;
	u64 *data;
	unsigned int i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->fds[0] = -1;
	ctx->fds[1] = -1;

	vdev = vector2_ethtool_test_alloc_vdev(test, 4);
	vdev->cfg.queues = 2;
	vdev->cfg.transport = UM_VEC2_TRANSPORT_FD;
	dev = alloc_etherdev_mqs(sizeof(struct um_vec2_netdev_priv), 2, 2);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	um_vec2_netdev_init(vdev, dev);
	vdev->netdev = dev;
	KUNIT_ASSERT_EQ(test, netif_set_real_num_tx_queues(dev, 2), 0);
	KUNIT_ASSERT_EQ(test, netif_set_real_num_rx_queues(dev, 2), 0);

	KUNIT_ASSERT_EQ(test, os_pipe(ctx->fds, 1, 1), 0);
	KUNIT_ASSERT_EQ(test, ctx->fds[1], ctx->fds[0] + 1);
	vdev->cfg.fd = ctx->fds[0];
	vdev->cfg.has_fd = true;

	KUNIT_ASSERT_EQ(test, um_vec2_netdev_open(dev), 0);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels);
	KUNIT_ASSERT_EQ(test, vdev->num_channels, 2U);

	ctx->vdev = vdev;
	ctx->dev = dev;

	for (i = 0; i < 2; i++) {
		um_vec2_fake_host_init(&ctx->ch[i].fake);
		ctx->ch[i].saved_host = vdev->channels[i].host;
		vdev->channels[i].host =
			um_vec2_fake_host_base(&ctx->ch[i].fake);
	}

	/*
	 * Enqueue one TX per channel and push one RX per channel.
	 * Read ethtool stats BEFORE the poll runs — the tx_lock /
	 * rx_lock paths see populated queues and must not deadlock.
	 */
	for (i = 0; i < 2; i++) {
		struct um_vec2_channel *channel = &vdev->channels[i];

		skbs[i] = alloc_skb(64, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, skbs[i]);
		skb_put(skbs[i], 64);
		KUNIT_ASSERT_EQ(test,
				um_vec2_tx_ring_enqueue(&channel->queue->tx,
							skbs[i], skbs[i]->len),
				0);
		KUNIT_ASSERT_EQ(test,
				um_vec2_fake_host_push_rx(&ctx->ch[i].fake, 64),
				0);
	}

	ops = dev->ethtool_ops;
	data = vector2_ethtool_test_stats(test, dev, &count);
	q0_tx_used = vector2_ethtool_find_stat(test, dev, "queue0_tx_ring_used");
	q1_tx_used = vector2_ethtool_find_stat(test, dev, "queue1_tx_ring_used");
	q0_rx_recv = vector2_ethtool_find_stat(test, dev,
					       "queue0_rx_batch_received_total");
	q1_rx_recv = vector2_ethtool_find_stat(test, dev,
					       "queue1_rx_batch_received_total");
	tx_enq = vector2_ethtool_find_stat(test, dev, "tx_ring_enqueued");
	rx_recv = vector2_ethtool_find_stat(test, dev,
					    "rx_batch_received_total");
	KUNIT_ASSERT_GE(test, q0_tx_used, 0);
	KUNIT_ASSERT_GE(test, q1_tx_used, 0);
	KUNIT_ASSERT_GE(test, q0_rx_recv, 0);
	KUNIT_ASSERT_GE(test, q1_rx_recv, 0);
	KUNIT_ASSERT_GE(test, tx_enq, 0);
	KUNIT_ASSERT_GE(test, rx_recv, 0);

	/* Pre-poll: TX rings populated, RX not yet drained. */
	KUNIT_EXPECT_EQ(test, data[q0_tx_used], 1ULL);
	KUNIT_EXPECT_EQ(test, data[q1_tx_used], 1ULL);
	KUNIT_EXPECT_GE(test, data[tx_enq], 2ULL);

	/* Drain channel 0 only, then re-read stats. */
	KUNIT_EXPECT_EQ(test, vector2_ethtool_traffic_invoke_poll(ctx, 0, 4), 1);

	data = vector2_ethtool_test_stats(test, dev, &count);
	KUNIT_EXPECT_EQ(test, data[q0_tx_used], 0ULL);
	KUNIT_EXPECT_EQ(test, data[q1_tx_used], 1ULL);
	KUNIT_EXPECT_EQ(test, data[q0_rx_recv], 1ULL);
	KUNIT_EXPECT_EQ(test, data[q1_rx_recv], 0ULL);

	/* Drain channel 1 and confirm final stats. */
	KUNIT_EXPECT_EQ(test, vector2_ethtool_traffic_invoke_poll(ctx, 1, 4), 1);

	data = vector2_ethtool_test_stats(test, dev, &count);
	KUNIT_EXPECT_EQ(test, data[q0_tx_used], 0ULL);
	KUNIT_EXPECT_EQ(test, data[q1_tx_used], 0ULL);
	KUNIT_EXPECT_EQ(test, data[q0_rx_recv], 1ULL);
	KUNIT_EXPECT_EQ(test, data[q1_rx_recv], 1ULL);
	KUNIT_EXPECT_GE(test, data[rx_recv], 2ULL);

	/* Restore real hosts for the close path's release callbacks. */
	for (i = 0; i < 2; i++)
		vdev->channels[i].host = ctx->ch[i].saved_host;

	um_vec2_netdev_stop(dev);
	if (ctx->fds[0] >= 0)
		os_close_file(ctx->fds[0]);
	if (ctx->fds[1] >= 0)
		os_close_file(ctx->fds[1]);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static struct kunit_case vector2_ethtool_test_cases[] = {
	KUNIT_CASE(vector2_ethtool_stats_stopped_test),
	KUNIT_CASE(vector2_ethtool_ring_policy_test),
	KUNIT_CASE(vector2_ethtool_xmit_drop_stats_test),
	KUNIT_CASE(vector2_ethtool_multiqueue_stats_aggregate_test),
	KUNIT_CASE(vector2_ethtool_stats_during_traffic_test),
	{}
};

static struct kunit_suite vector2_ethtool_test_suite = {
	.name = "um_vector2_ethtool",
	.test_cases = vector2_ethtool_test_cases,
};

kunit_test_suite(vector2_ethtool_test_suite);
