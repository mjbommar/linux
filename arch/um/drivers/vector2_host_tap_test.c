// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the UML vector networking v2 TAP host backend.
 */

#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/if.h>
#include <linux/netdevice.h>

#include <os.h>

#include "vector2_internal.h"

static struct um_vec2_dev *vector2_tap_test_alloc_vdev(struct kunit *test,
						       unsigned int unit)
{
	struct um_vec2_dev *vdev;

	vdev = kunit_kzalloc(test, sizeof(*vdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, vdev);

	INIT_LIST_HEAD(&vdev->list);
	mutex_init(&vdev->lock);
	vdev->unit = unit;
	um_vec2_config_init(&vdev->cfg);
	vdev->cfg.transport = UM_VEC2_TRANSPORT_TAP;
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
vector2_tap_test_alloc_netdev(struct kunit *test, struct um_vec2_dev *vdev)
{
	struct net_device *dev;

	dev = alloc_etherdev_mqs(sizeof(struct um_vec2_netdev_priv), 1, 1);
	KUNIT_ASSERT_NOT_NULL(test, dev);

	um_vec2_netdev_init(vdev, dev);
	return dev;
}

static void vector2_tap_test_close_pipe(int *fds)
{
	if (fds[0] >= 0)
		os_close_file(fds[0]);
	if (fds[1] >= 0)
		os_close_file(fds[1]);
	fds[0] = -1;
	fds[1] = -1;
}

static void vector2_tap_attach_close_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 0);
	int fds[2] = { -1, -1 };

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);

	KUNIT_EXPECT_EQ(test, um_vec2_tap_attach_fd(vdev, fds[0]), 0);
	fds[0] = -1;
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels[0].host);
	KUNIT_EXPECT_STREQ(test, vdev->channels[0].host->ops->name, "tap");
	KUNIT_EXPECT_EQ(test, vdev->channels[0].life.state,
			UM_VEC2_CHAN_FD_ATTACHED);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 1U);

	um_vec2_tap_close(vdev);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);

	vector2_tap_test_close_pipe(fds);
}

static void vector2_tap_attach_rejects_busy_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 1);
	int first[2] = { -1, -1 };
	int second[2] = { -1, -1 };

	KUNIT_ASSERT_EQ(test, os_pipe(first, 1, 1), 0);
	KUNIT_ASSERT_EQ(test, os_pipe(second, 1, 1), 0);

	KUNIT_EXPECT_EQ(test, um_vec2_tap_attach_fd(vdev, first[0]), 0);
	first[0] = -1;
	KUNIT_EXPECT_EQ(test, um_vec2_tap_attach_fd(vdev, second[0]), -EBUSY);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 1U);

	um_vec2_tap_close(vdev);
	vector2_tap_test_close_pipe(first);
	vector2_tap_test_close_pipe(second);
}

static void vector2_tap_sandbox_open_fails_closed_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 2);

	if (IS_ENABLED(CONFIG_UML_NET_VECTOR_V2_INPROC))
		kunit_skip(test, "trusted in-process build may open host TAP");

	strscpy(vdev->cfg.ifname, "kunit-tap0", sizeof(vdev->cfg.ifname));

	KUNIT_EXPECT_EQ(test, um_vec2_tap_open(vdev), -EACCES);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);
}

static void vector2_tap_netdev_open_unwinds_sandbox_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 3);
	struct net_device *dev;

	if (IS_ENABLED(CONFIG_UML_NET_VECTOR_V2_INPROC))
		kunit_skip(test, "trusted in-process build may open host TAP");

	strscpy(vdev->cfg.ifname, "kunit-tap0", sizeof(vdev->cfg.ifname));
	dev = vector2_tap_test_alloc_netdev(test, vdev);

	KUNIT_EXPECT_EQ(test, um_vec2_netdev_open(dev), -EACCES);
	KUNIT_EXPECT_EQ(test, vdev->life.state, UM_VEC2_DEV_REGISTERED);
	KUNIT_EXPECT_NULL(test, vdev->channels);

	free_netdev(dev);
}

static struct kunit_case vector2_tap_test_cases[] = {
	KUNIT_CASE(vector2_tap_attach_close_test),
	KUNIT_CASE(vector2_tap_attach_rejects_busy_test),
	KUNIT_CASE(vector2_tap_sandbox_open_fails_closed_test),
	KUNIT_CASE(vector2_tap_netdev_open_unwinds_sandbox_test),
	{}
};

static struct kunit_suite vector2_tap_test_suite = {
	.name = "um_vector2_host_tap",
	.test_cases = vector2_tap_test_cases,
};

kunit_test_suite(vector2_tap_test_suite);
