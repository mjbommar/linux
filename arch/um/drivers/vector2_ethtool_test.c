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

static struct kunit_case vector2_ethtool_test_cases[] = {
	KUNIT_CASE(vector2_ethtool_stats_stopped_test),
	KUNIT_CASE(vector2_ethtool_ring_policy_test),
	KUNIT_CASE(vector2_ethtool_xmit_drop_stats_test),
	{}
};

static struct kunit_suite vector2_ethtool_test_suite = {
	.name = "um_vector2_ethtool",
	.test_cases = vector2_ethtool_test_cases,
};

kunit_test_suite(vector2_ethtool_test_suite);
