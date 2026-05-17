// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the UML vector networking v2 direct-fd host backend.
 */

#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>

#include <os.h>

#include "vector2_internal.h"

static struct um_vec2_dev *vector2_fd_test_alloc_vdev(struct kunit *test,
						      unsigned int unit)
{
	struct um_vec2_dev *vdev;

	vdev = kunit_kzalloc(test, sizeof(*vdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, vdev);

	INIT_LIST_HEAD(&vdev->list);
	mutex_init(&vdev->lock);
	vdev->unit = unit;
	um_vec2_config_init(&vdev->cfg);
	vdev->cfg.transport = UM_VEC2_TRANSPORT_FD;
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
vector2_fd_test_alloc_netdev(struct kunit *test, struct um_vec2_dev *vdev)
{
	struct net_device *dev;

	dev = alloc_etherdev_mqs(sizeof(struct um_vec2_netdev_priv), 1, 1);
	KUNIT_ASSERT_NOT_NULL(test, dev);

	um_vec2_netdev_init(vdev, dev);
	return dev;
}

static void vector2_fd_test_close_pipe(int *fds)
{
	if (fds[0] >= 0)
		os_close_file(fds[0]);
	if (fds[1] >= 0)
		os_close_file(fds[1]);
	fds[0] = -1;
	fds[1] = -1;
}

static void vector2_fd_open_close_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 0);
	int fds[2] = { -1, -1 };

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);
	vdev->cfg.fd = fds[0];
	vdev->cfg.has_fd = true;

	KUNIT_EXPECT_EQ(test, um_vec2_fd_open(vdev), 0);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels[0].host);
	KUNIT_EXPECT_STREQ(test, vdev->channels[0].host->ops->name, "fd");
	KUNIT_EXPECT_EQ(test, vdev->channels[0].life.state,
			UM_VEC2_CHAN_FD_ATTACHED);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 1U);

	um_vec2_fd_close(vdev);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);

	vector2_fd_test_close_pipe(fds);
}

static void vector2_fd_bad_fd_fails_closed_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 1);

	vdev->cfg.fd = INT_MAX;
	vdev->cfg.has_fd = true;

	KUNIT_EXPECT_EQ(test, um_vec2_fd_open(vdev), -EBADF);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);
}

static void vector2_fd_netdev_open_stop_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 2);
	struct net_device *dev;
	int fds[2] = { -1, -1 };

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);
	vdev->cfg.fd = fds[0];
	vdev->cfg.has_fd = true;
	dev = vector2_fd_test_alloc_netdev(test, vdev);

	KUNIT_EXPECT_EQ(test, um_vec2_netdev_open(dev), 0);
	KUNIT_EXPECT_EQ(test, vdev->life.state, UM_VEC2_DEV_RUNNING);
	KUNIT_EXPECT_NOT_NULL(test, vdev->channels);
	KUNIT_EXPECT_FALSE(test, netif_carrier_ok(dev));

	KUNIT_EXPECT_EQ(test, um_vec2_netdev_stop(dev), 0);
	KUNIT_EXPECT_EQ(test, vdev->life.state, UM_VEC2_DEV_REGISTERED);
	KUNIT_EXPECT_NULL(test, vdev->channels);

	free_netdev(dev);
	vector2_fd_test_close_pipe(fds);
}

static struct kunit_case vector2_fd_test_cases[] = {
	KUNIT_CASE(vector2_fd_open_close_test),
	KUNIT_CASE(vector2_fd_bad_fd_fails_closed_test),
	KUNIT_CASE(vector2_fd_netdev_open_stop_test),
	{}
};

static struct kunit_suite vector2_fd_test_suite = {
	.name = "um_vector2_host_fd",
	.test_cases = vector2_fd_test_cases,
};

kunit_test_suite(vector2_fd_test_suite);
