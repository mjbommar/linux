// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the UML vector networking v2 netdev skeleton.
 */

#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>

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

	dev = alloc_etherdev_mqs(sizeof(struct um_vec2_netdev_priv), 1, 1);
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

static struct kunit_case vector2_netdev_test_cases[] = {
	KUNIT_CASE(vector2_netdev_name_and_mac_test),
	KUNIT_CASE(vector2_netdev_open_unwinds_missing_backend_test),
	KUNIT_CASE(vector2_netdev_stop_registered_is_safe_test),
	KUNIT_CASE(vector2_netdev_xmit_drops_when_not_running_test),
	{}
};

static struct kunit_suite vector2_netdev_test_suite = {
	.name = "um_vector2_netdev",
	.test_cases = vector2_netdev_test_cases,
};

kunit_test_suite(vector2_netdev_test_suite);
