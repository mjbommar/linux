// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the UML vector networking v2 netdev layer.
 */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>

#include <os.h>

#include "vector2_fake_host.h"
#include "vector2_internal.h"

static struct um_vec2_dev *vector2_netdev_test_alloc_vdev(struct kunit *test,
							  unsigned int unit)
{
	struct um_vec2_dev *vdev;

	vdev = kunit_kzalloc(test, sizeof(*vdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, vdev);

	memset(vdev, 0, sizeof(*vdev));
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
vector2_netdev_test_alloc(struct kunit *test, struct um_vec2_dev *vdev)
{
	struct net_device *dev;
	unsigned int queues = um_vec2_netdev_queue_count(vdev);

	dev = alloc_etherdev_mqs(sizeof(struct um_vec2_netdev_priv), queues,
				 queues);
	KUNIT_ASSERT_NOT_NULL(test, dev);

	um_vec2_netdev_init(vdev, dev);
	return dev;
}

static void vector2_netdev_name_and_mac_test(struct kunit *test)
{
	static const u8 mac[ETH_ALEN] = { 0x02, 0, 0, 0, 0, 0x42 };
	struct um_vec2_dev *vdev;
	struct net_device *dev;

	vdev = vector2_netdev_test_alloc_vdev(test, 42);
	ether_addr_copy(vdev->cfg.mac, mac);
	vdev->cfg.has_mac = true;
	dev = vector2_netdev_test_alloc(test, vdev);

	KUNIT_EXPECT_STREQ(test, dev->name, "vec2.42");
	KUNIT_EXPECT_TRUE(test, ether_addr_equal(dev->dev_addr, mac));
	KUNIT_EXPECT_EQ(test, dev->mtu, UM_VEC2_DEFAULT_MTU);

	free_netdev(dev);
}

static void vector2_netdev_open_unwinds_missing_backend_test(struct kunit *test)
{
	struct um_vec2_dev *vdev;
	struct net_device *dev;
	int ret;

	vdev = vector2_netdev_test_alloc_vdev(test, 0);
	dev = vector2_netdev_test_alloc(test, vdev);

	ret = um_vec2_netdev_open(dev);

	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, vdev->life.state, UM_VEC2_DEV_REGISTERED);
	KUNIT_EXPECT_FALSE(test, netif_carrier_ok(dev));

	free_netdev(dev);
}

static void vector2_netdev_stop_registered_is_safe_test(struct kunit *test)
{
	struct um_vec2_dev *vdev;
	struct net_device *dev;
	int ret;

	vdev = vector2_netdev_test_alloc_vdev(test, 1);
	dev = vector2_netdev_test_alloc(test, vdev);

	ret = um_vec2_netdev_stop(dev);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, vdev->life.state, UM_VEC2_DEV_REGISTERED);
	KUNIT_EXPECT_FALSE(test, netif_carrier_ok(dev));

	free_netdev(dev);
}

static void vector2_netdev_xmit_drops_when_not_running_test(struct kunit *test)
{
	struct um_vec2_dev *vdev;
	struct net_device *dev;
	struct sk_buff *skb;
	unsigned long before;
	netdev_tx_t tx_ret;

	vdev = vector2_netdev_test_alloc_vdev(test, 2);
	dev = vector2_netdev_test_alloc(test, vdev);
	before = dev->stats.tx_dropped;
	skb = alloc_skb(64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);

	tx_ret = um_vec2_netdev_start_xmit(skb, dev);

	KUNIT_EXPECT_EQ(test, tx_ret, NETDEV_TX_OK);
	KUNIT_EXPECT_EQ(test, dev->stats.tx_dropped, before + 1);

	free_netdev(dev);
}

static void vector2_netdev_uses_configured_queue_count_test(struct kunit *test)
{
	struct um_vec2_dev *vdev;
	struct net_device *dev;

	vdev = vector2_netdev_test_alloc_vdev(test, 3);
	vdev->cfg.queues = 4;
	dev = vector2_netdev_test_alloc(test, vdev);

	KUNIT_EXPECT_EQ(test, um_vec2_netdev_queue_count(vdev), 4U);
	KUNIT_EXPECT_EQ(test, dev->num_tx_queues, 4U);
	KUNIT_EXPECT_EQ(test, dev->real_num_tx_queues, 4U);
	KUNIT_EXPECT_EQ(test, dev->num_rx_queues, 4U);
	KUNIT_EXPECT_EQ(test, dev->real_num_rx_queues, 4U);

	free_netdev(dev);
}

static void vector2_netdev_queue_cpu_policy_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  um_vec2_tx_queue_uses_cpu_ordinal(0, 0, 2, 4));
	KUNIT_EXPECT_TRUE(test,
			  um_vec2_tx_queue_uses_cpu_ordinal(0, 2, 2, 4));
	KUNIT_EXPECT_TRUE(test,
			  um_vec2_tx_queue_uses_cpu_ordinal(1, 1, 2, 4));
	KUNIT_EXPECT_TRUE(test,
			  um_vec2_tx_queue_uses_cpu_ordinal(1, 3, 2, 4));
	KUNIT_EXPECT_FALSE(test,
			   um_vec2_tx_queue_uses_cpu_ordinal(1, 0, 2, 4));

	KUNIT_EXPECT_TRUE(test,
			  um_vec2_tx_queue_uses_cpu_ordinal(0, 0, 4, 2));
	KUNIT_EXPECT_TRUE(test,
			  um_vec2_tx_queue_uses_cpu_ordinal(2, 0, 4, 2));
	KUNIT_EXPECT_TRUE(test,
			  um_vec2_tx_queue_uses_cpu_ordinal(1, 1, 4, 2));
	KUNIT_EXPECT_TRUE(test,
			  um_vec2_tx_queue_uses_cpu_ordinal(3, 1, 4, 2));
	KUNIT_EXPECT_FALSE(test,
			   um_vec2_tx_queue_uses_cpu_ordinal(3, 0, 4, 2));

	KUNIT_EXPECT_FALSE(test,
			   um_vec2_tx_queue_uses_cpu_ordinal(0, 0, 0, 2));
	KUNIT_EXPECT_FALSE(test,
			   um_vec2_tx_queue_uses_cpu_ordinal(0, 0, 2, 0));
	KUNIT_EXPECT_FALSE(test,
			   um_vec2_tx_queue_uses_cpu_ordinal(2, 0, 2, 4));
	KUNIT_EXPECT_FALSE(test,
			   um_vec2_tx_queue_uses_cpu_ordinal(0, 4, 2, 4));
}

/*
 * NAPI poll path coverage with the deterministic fake host.
 *
 * These tests open the FD transport against a pipe so start_datapath()
 * registers the real NAPI callback, then swap channel->host out for the
 * deterministic fake host. Direct poll invocation covers healthy traffic,
 * backend-dead paths, TX reschedule, RX allocation failure, and RX protocol
 * drops.
 */
struct vector2_netdev_poll_ctx {
	struct um_vec2_dev *vdev;
	struct net_device *dev;
	struct um_vec2_fake_host fake;
	struct um_vec2_host *saved_host;
	int fds[2];
};

/*
 * Open the netdev via the FD transport against a pipe, then swap the
 * channel's host backend out for the deterministic fake host.  This
 * gives the test a properly initialized NAPI (napi->poll =
 * um_vec2_netdev_poll because um_vec2_start_datapath wired it up) that
 * can be invoked directly, while still controlling tx/rx outcomes
 * byte-for-byte via the fake host primitives.
 */
static struct vector2_netdev_poll_ctx *
vector2_netdev_poll_ctx_open(struct kunit *test, unsigned int unit)
{
	struct vector2_netdev_poll_ctx *ctx;
	struct um_vec2_dev *vdev;
	struct net_device *dev;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->fds[0] = -1;
	ctx->fds[1] = -1;

	vdev = vector2_netdev_test_alloc_vdev(test, unit);
	vdev->cfg.transport = UM_VEC2_TRANSPORT_FD;
	dev = vector2_netdev_test_alloc(test, vdev);
	vdev->netdev = dev;

	KUNIT_ASSERT_EQ(test, os_pipe(ctx->fds, 1, 1), 0);
	vdev->cfg.fd = ctx->fds[0];
	vdev->cfg.has_fd = true;

	ret = um_vec2_netdev_open(dev);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels);
	KUNIT_ASSERT_EQ(test, vdev->num_channels, 1U);

	um_vec2_fake_host_init(&ctx->fake);
	ctx->saved_host = vdev->channels[0].host;
	vdev->channels[0].host = um_vec2_fake_host_base(&ctx->fake);

	ctx->vdev = vdev;
	ctx->dev = dev;
	return ctx;
}

static void vector2_netdev_poll_ctx_close(struct vector2_netdev_poll_ctx *ctx)
{
	struct um_vec2_dev *vdev = ctx->vdev;
	struct net_device *dev = ctx->dev;

	/*
	 * Restore the real fd-backed host so um_vec2_netdev_stop()'s
	 * close path frees the right backing object.  The fake host
	 * lives in ctx (kunit_kzalloc'd) and needs no explicit teardown.
	 */
	if (vdev->channels)
		vdev->channels[0].host = ctx->saved_host;

	um_vec2_netdev_stop(dev);
	if (ctx->fds[0] >= 0)
		os_close_file(ctx->fds[0]);
	if (ctx->fds[1] >= 0)
		os_close_file(ctx->fds[1]);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static struct um_vec2_channel *
vector2_netdev_poll_channel(struct vector2_netdev_poll_ctx *ctx)
{
	return &ctx->vdev->channels[0];
}

static int vector2_netdev_poll_invoke(struct vector2_netdev_poll_ctx *ctx,
				      int budget)
{
	struct um_vec2_channel *channel = vector2_netdev_poll_channel(ctx);
	struct napi_struct *napi = &channel->napi;
	int ret;

	/*
	 * Mark NAPI scheduled so in-poll napi_complete_done() sees the expected
	 * state. Clear MISSED first so a tx_more reschedule is distinguishable
	 * from this setup state.
	 */
	clear_bit(NAPI_STATE_MISSED, &napi->state);
	set_bit(NAPI_STATE_SCHED, &napi->state);
	ret = napi->poll(napi, budget);
	/*
	 * napi_complete_done() inside the poll cleared SCHED on the normal
	 * exit.  For the tx_more / backend_dead paths it may still be set;
	 * the per-test assertions handle both.
	 */
	return ret;
}

static void vector2_netdev_poll_healthy_round_test(struct kunit *test)
{
	struct vector2_netdev_poll_ctx *ctx =
		vector2_netdev_poll_ctx_open(test, 32);
	struct um_vec2_channel *channel = vector2_netdev_poll_channel(ctx);
	u64 before_napi_polls;
	struct sk_buff *skb;
	int rx_done;

	/*
	 * um_vec2_netdev_open() ends with napi_schedule(), which the softirq
	 * layer may or may not have drained by this point. Snapshot NAPI_POLLS
	 * after this direct invocation so the delta below is stable.
	 */
	skb = alloc_skb(64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_put(skb, 64);
	KUNIT_ASSERT_EQ(test,
			um_vec2_tx_ring_enqueue(&channel->queue->tx, skb,
						skb->len), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_fake_host_push_rx(&ctx->fake, 64), 0);

	before_napi_polls = um_vec2_stat_read(ctx->vdev,
					      UM_VEC2_STAT_NAPI_POLLS);
	rx_done = vector2_netdev_poll_invoke(ctx, 4);

	KUNIT_EXPECT_EQ(test, rx_done, 1);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_empty(&channel->queue->tx));
	KUNIT_EXPECT_GE(test,
			um_vec2_stat_read(ctx->vdev, UM_VEC2_STAT_NAPI_POLLS),
			before_napi_polls + 1ULL);
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(ctx->vdev,
					  UM_VEC2_STAT_BACKEND_DEAD), 0ULL);
	KUNIT_EXPECT_FALSE(test, test_bit(NAPI_STATE_SCHED,
					  &channel->napi.state));

	vector2_netdev_poll_ctx_close(ctx);
}

static void vector2_netdev_poll_backend_dead_tx_test(struct kunit *test)
{
	struct vector2_netdev_poll_ctx *ctx =
		vector2_netdev_poll_ctx_open(test, 33);
	struct um_vec2_channel *channel = vector2_netdev_poll_channel(ctx);
	struct sk_buff *skb;
	int rx_done;

	skb = alloc_skb(64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_put(skb, 64);
	KUNIT_ASSERT_EQ(test,
			um_vec2_tx_ring_enqueue(&channel->queue->tx, skb,
						skb->len), 0);
	um_vec2_fake_host_kill(&ctx->fake);
	KUNIT_ASSERT_TRUE(test, netif_carrier_ok(ctx->dev));

	rx_done = vector2_netdev_poll_invoke(ctx, 4);

	KUNIT_EXPECT_EQ(test, rx_done, 0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(ctx->vdev,
					  UM_VEC2_STAT_BACKEND_DEAD), 1ULL);
	KUNIT_EXPECT_FALSE(test, netif_carrier_ok(ctx->dev));

	/*
	 * The skb remained on the TX ring because tx_batch returned
	 * -ENODEV before completion.  um_vec2_queue_pair_free() inside
	 * the ctx_close path resets the ring with um_vec2_tx_drop_skb,
	 * which releases any remaining skb references.
	 */

	vector2_netdev_poll_ctx_close(ctx);
}

static void vector2_netdev_poll_backend_dead_rx_test(struct kunit *test)
{
	struct vector2_netdev_poll_ctx *ctx =
		vector2_netdev_poll_ctx_open(test, 34);
	int rx_done;

	/*
	 * Empty TX ring so tx_batch is skipped entirely and tx_done stays 0.
	 * rx_batch then sees fake.dead and returns -ENODEV, which the poll
	 * must route to the rx-side backend_dead branch.
	 */
	um_vec2_fake_host_kill(&ctx->fake);
	KUNIT_ASSERT_TRUE(test, netif_carrier_ok(ctx->dev));

	rx_done = vector2_netdev_poll_invoke(ctx, 4);

	KUNIT_EXPECT_EQ(test, rx_done, 0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(ctx->vdev,
					  UM_VEC2_STAT_BACKEND_DEAD), 1ULL);
	KUNIT_EXPECT_FALSE(test, netif_carrier_ok(ctx->dev));

	vector2_netdev_poll_ctx_close(ctx);
}

static void vector2_netdev_poll_tx_more_reschedules_test(struct kunit *test)
{
	struct vector2_netdev_poll_ctx *ctx =
		vector2_netdev_poll_ctx_open(test, 35);
	struct um_vec2_channel *channel = vector2_netdev_poll_channel(ctx);
	struct sk_buff *skb1;
	struct sk_buff *skb2;
	int rx_done;

	/*
	 * Two TX skbs enqueued, fake_host tx_limit = 1 so the first poll
	 * round only completes one packet.  tx_more is then true AND
	 * tx_done > 0, which is the napi_schedule() rescheduling branch.
	 */
	skb1 = alloc_skb(64, GFP_KERNEL);
	skb2 = alloc_skb(64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb1);
	KUNIT_ASSERT_NOT_NULL(test, skb2);
	skb_put(skb1, 64);
	skb_put(skb2, 64);
	KUNIT_ASSERT_EQ(test,
			um_vec2_tx_ring_enqueue(&channel->queue->tx, skb1,
						skb1->len), 0);
	KUNIT_ASSERT_EQ(test,
			um_vec2_tx_ring_enqueue(&channel->queue->tx, skb2,
						skb2->len), 0);
	um_vec2_fake_host_set_tx_limit(&ctx->fake, 1);

	rx_done = vector2_netdev_poll_invoke(ctx, 4);

	KUNIT_EXPECT_EQ(test, rx_done, 0);
	KUNIT_EXPECT_EQ(test, channel->queue->tx.count, 1U);
	KUNIT_EXPECT_TRUE(test, test_bit(NAPI_STATE_SCHED,
					 &channel->napi.state));

	/* Drain the rest so ctx_close has nothing pending. */
	um_vec2_fake_host_set_tx_limit(&ctx->fake, UINT_MAX);
	clear_bit(NAPI_STATE_SCHED, &channel->napi.state);
	rx_done = vector2_netdev_poll_invoke(ctx, 4);
	KUNIT_EXPECT_EQ(test, rx_done, 0);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_empty(&channel->queue->tx));

	vector2_netdev_poll_ctx_close(ctx);
}

static void vector2_netdev_poll_rx_alloc_error_test(struct kunit *test)
{
	struct vector2_netdev_poll_ctx *ctx =
		vector2_netdev_poll_ctx_open(test, 36);
	int rx_done;

	um_vec2_fake_host_set_rx_error(&ctx->fake, -ENOMEM);

	rx_done = vector2_netdev_poll_invoke(ctx, 4);

	KUNIT_EXPECT_EQ(test, rx_done, 0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(ctx->vdev,
					  UM_VEC2_STAT_RX_ALLOC_ERRORS), 1ULL);
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(ctx->vdev,
					  UM_VEC2_STAT_BACKEND_DEAD), 0ULL);

	vector2_netdev_poll_ctx_close(ctx);
}

static void vector2_netdev_poll_rx_eproto_test(struct kunit *test)
{
	struct vector2_netdev_poll_ctx *ctx =
		vector2_netdev_poll_ctx_open(test, 37);
	unsigned long before_dropped;
	int rx_done;

	before_dropped = ctx->dev->stats.rx_dropped;
	um_vec2_fake_host_set_rx_error(&ctx->fake, -EPROTO);

	rx_done = vector2_netdev_poll_invoke(ctx, 4);

	KUNIT_EXPECT_EQ(test, rx_done, 0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(ctx->vdev,
					  UM_VEC2_STAT_RX_PROTO_DROPS), 1ULL);
	KUNIT_EXPECT_EQ(test, ctx->dev->stats.rx_dropped, before_dropped + 1);
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(ctx->vdev,
					  UM_VEC2_STAT_BACKEND_DEAD), 0ULL);

	vector2_netdev_poll_ctx_close(ctx);
}

static struct kunit_case vector2_netdev_test_cases[] = {
	KUNIT_CASE(vector2_netdev_name_and_mac_test),
	KUNIT_CASE(vector2_netdev_open_unwinds_missing_backend_test),
	KUNIT_CASE(vector2_netdev_stop_registered_is_safe_test),
	KUNIT_CASE(vector2_netdev_xmit_drops_when_not_running_test),
	KUNIT_CASE(vector2_netdev_uses_configured_queue_count_test),
	KUNIT_CASE(vector2_netdev_queue_cpu_policy_test),
	KUNIT_CASE(vector2_netdev_poll_healthy_round_test),
	KUNIT_CASE(vector2_netdev_poll_backend_dead_tx_test),
	KUNIT_CASE(vector2_netdev_poll_backend_dead_rx_test),
	KUNIT_CASE(vector2_netdev_poll_tx_more_reschedules_test),
	KUNIT_CASE(vector2_netdev_poll_rx_alloc_error_test),
	KUNIT_CASE(vector2_netdev_poll_rx_eproto_test),
	{}
};

static struct kunit_suite vector2_netdev_test_suite = {
	.name = "um_vector2_netdev",
	.test_cases = vector2_netdev_test_cases,
};

kunit_test_suite(vector2_netdev_test_suite);
