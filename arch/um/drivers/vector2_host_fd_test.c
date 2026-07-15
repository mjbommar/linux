// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the UML vector networking v2 direct-fd host backend.
 */

#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/mm.h>
#include <linux/netdevice.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/string.h>

#include <os.h>

#include "vector2_internal.h"
#include "vector2_test.h"

#define VECTOR2_FD_OPEN_STOP_STRESS_ITERS	1000U
#define VECTOR2_FD_FAILURE_STRESS_ITERS		10000U

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
	unsigned int queues = um_vec2_netdev_queue_count(vdev);

	dev = alloc_etherdev_mqs(sizeof(struct um_vec2_netdev_priv), queues,
				 queues);
	KUNIT_ASSERT_NOT_NULL(test, dev);

	um_vec2_netdev_init(vdev, dev);
	vdev->netdev = dev;
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
	struct net_device *dev = vector2_fd_test_alloc_netdev(test, vdev);
	int fds[2] = { -1, -1 };

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);
	vdev->cfg.fd = fds[0];
	vdev->cfg.has_fd = true;

	KUNIT_EXPECT_EQ(test, um_vec2_fd_open(vdev), 0);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels[0].host);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels[0].queue);
	KUNIT_EXPECT_STREQ(test, vdev->channels[0].host->ops->name, "fd");
	KUNIT_EXPECT_EQ(test, vdev->channels[0].life.state,
			UM_VEC2_CHAN_FD_ATTACHED);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 1U);

	um_vec2_fd_close(vdev);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);

	vector2_fd_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_fd_bad_fd_fails_closed_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 1);
	struct net_device *dev = vector2_fd_test_alloc_netdev(test, vdev);

	vdev->cfg.fd = INT_MAX;
	vdev->cfg.has_fd = true;

	KUNIT_EXPECT_EQ(test, um_vec2_fd_open(vdev), -EBADF);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);

	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_fd_wrong_type_fails_closed_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 5);
	struct net_device *dev = vector2_fd_test_alloc_netdev(test, vdev);
	int fd;

	fd = os_open_file(".", of_read(OPENFLAGS()), 0);
	KUNIT_ASSERT_GE(test, fd, 0);
	vdev->cfg.fd = fd;
	vdev->cfg.has_fd = true;

	KUNIT_EXPECT_EQ(test, um_vec2_fd_open(vdev), -EINVAL);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);

	os_close_file(fd);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_fd_multiqueue_open_close_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 6);
	struct net_device *dev;
	int fds[2] = { -1, -1 };

	vdev->cfg.queues = 2;
	dev = vector2_fd_test_alloc_netdev(test, vdev);

	KUNIT_ASSERT_EQ(test, um_vec2_test_adjacent_pipe(fds), 0);
	vdev->cfg.fd = fds[0];
	vdev->cfg.has_fd = true;

	KUNIT_EXPECT_EQ(test, um_vec2_fd_open(vdev), 0);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels[0].host);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels[1].host);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 2U);
	KUNIT_EXPECT_EQ(test, vdev->channels[0].index, 0U);
	KUNIT_EXPECT_EQ(test, vdev->channels[1].index, 1U);
	KUNIT_EXPECT_NE(test, vdev->channels[0].rx_fd,
			vdev->channels[1].rx_fd);

	um_vec2_fd_close(vdev);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);

	vector2_fd_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_fd_multiqueue_missing_second_fd_unwinds_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 7);
	struct net_device *dev;
	int fds[2] = { -1, -1 };

	vdev->cfg.queues = 2;
	dev = vector2_fd_test_alloc_netdev(test, vdev);

	KUNIT_ASSERT_EQ(test, um_vec2_test_adjacent_pipe(fds), 0);
	os_close_file(fds[1]);
	fds[1] = -1;
	vdev->cfg.fd = fds[0];
	vdev->cfg.has_fd = true;

	KUNIT_EXPECT_EQ(test, um_vec2_fd_open(vdev), -EBADF);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);

	vector2_fd_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

/*
 * Partial-open unwind must leave vdev->channels = NULL.
 *
 * Validation passes for both fds in the multi-queue range, then the
 * fault injector trips um_vec2_fd_channel_open() at index 1. The
 * unwind path closes channel 0 and frees the channels array. A subsequent
 * um_vec2_fd_close() must be a no-op rather than walking freed memory.
 */
static void vector2_fd_multiqueue_partial_open_unwind_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 16);
	struct net_device *dev;
	int fds[2] = { -1, -1 };

	vdev->cfg.queues = 2;
	dev = vector2_fd_test_alloc_netdev(test, vdev);

	KUNIT_ASSERT_EQ(test, um_vec2_test_adjacent_pipe(fds), 0);
	vdev->cfg.fd = fds[0];
	vdev->cfg.has_fd = true;

	um_vec2_fd_fault_index = 1;
	KUNIT_EXPECT_EQ(test, um_vec2_fd_open(vdev), -EIO);
	um_vec2_fd_fault_index = -1;

	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);

	/* Closing a freshly unwound vdev must not walk freed memory. */
	um_vec2_fd_close(vdev);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);

	vector2_fd_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
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
	KUNIT_EXPECT_TRUE(test, netif_carrier_ok(dev));

	KUNIT_EXPECT_EQ(test, um_vec2_netdev_stop(dev), 0);
	KUNIT_EXPECT_EQ(test, vdev->life.state, UM_VEC2_DEV_REGISTERED);
	KUNIT_EXPECT_NULL(test, vdev->channels);

	vdev->netdev = NULL;
	free_netdev(dev);
	vector2_fd_test_close_pipe(fds);
}

static struct net_device *
vector2_fd_test_alloc_pipe_netdev(struct kunit *test, struct um_vec2_dev *vdev,
				  int fds[2])
{
	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);
	vdev->cfg.fd = fds[0];
	vdev->cfg.has_fd = true;
	return vector2_fd_test_alloc_netdev(test, vdev);
}

static bool vector2_fd_repeat_open(struct kunit *test, struct um_vec2_dev *vdev,
				   struct net_device *dev, unsigned int iter)
{
	int ret;

	ret = um_vec2_netdev_open(dev);
	if (ret) {
		KUNIT_FAIL(test, "iter %u open ret=%d", iter, ret);
		return false;
	}
	if (vdev->life.state != UM_VEC2_DEV_RUNNING) {
		KUNIT_FAIL(test, "iter %u state after open=%d", iter,
			   vdev->life.state);
		return false;
	}
	if (!vdev->channels || !netif_carrier_ok(dev)) {
		KUNIT_FAIL(test, "iter %u open did not attach channel", iter);
		return false;
	}

	return true;
}

static bool vector2_fd_repeat_stop(struct kunit *test, struct um_vec2_dev *vdev,
				   struct net_device *dev, unsigned int iter)
{
	int ret;

	ret = um_vec2_netdev_stop(dev);
	if (ret) {
		KUNIT_FAIL(test, "iter %u stop ret=%d", iter, ret);
		return false;
	}
	if (vdev->life.state != UM_VEC2_DEV_REGISTERED ||
	    vdev->channels || netif_carrier_ok(dev)) {
		KUNIT_FAIL(test, "iter %u stop did not return closed", iter);
		return false;
	}

	return true;
}

static void vector2_fd_expect_repeat_stats(struct kunit *test,
					   struct um_vec2_dev *vdev)
{
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(vdev, UM_VEC2_STAT_OPEN_ATTEMPTS),
			(u64)VECTOR2_FD_OPEN_STOP_STRESS_ITERS);
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(vdev, UM_VEC2_STAT_OPEN_FAILURES),
			0ULL);
	KUNIT_EXPECT_EQ(test, um_vec2_stat_read(vdev, UM_VEC2_STAT_CLOSES),
			(u64)VECTOR2_FD_OPEN_STOP_STRESS_ITERS);
}

static void vector2_fd_netdev_open_stop_repeats_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 8);
	struct net_device *dev;
	unsigned int i;
	int fds[2] = { -1, -1 };

	dev = vector2_fd_test_alloc_pipe_netdev(test, vdev, fds);

	for (i = 0; i < VECTOR2_FD_OPEN_STOP_STRESS_ITERS; i++) {
		if (!vector2_fd_repeat_open(test, vdev, dev, i))
			break;
		if (!vector2_fd_repeat_stop(test, vdev, dev, i))
			break;

		if (!(i & 0x3f))
			cond_resched();
	}

	KUNIT_EXPECT_EQ(test, i, VECTOR2_FD_OPEN_STOP_STRESS_ITERS);
	vector2_fd_expect_repeat_stats(test, vdev);

	if (vdev->life.state != UM_VEC2_DEV_REGISTERED)
		um_vec2_netdev_stop(dev);
	vdev->netdev = NULL;
	free_netdev(dev);
	vector2_fd_test_close_pipe(fds);
}

static void vector2_fd_netdev_bad_fd_unwinds_closed_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 9);
	struct net_device *dev = vector2_fd_test_alloc_netdev(test, vdev);
	int ret;

	vdev->cfg.fd = INT_MAX;
	vdev->cfg.has_fd = true;

	ret = um_vec2_netdev_open(dev);
	KUNIT_EXPECT_EQ(test, ret, -EBADF);
	KUNIT_EXPECT_EQ(test, vdev->life.state, UM_VEC2_DEV_REGISTERED);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);
	KUNIT_EXPECT_FALSE(test, netif_carrier_ok(dev));
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(vdev, UM_VEC2_STAT_OPEN_ATTEMPTS),
			1ULL);
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(vdev, UM_VEC2_STAT_OPEN_FAILURES),
			1ULL);
	KUNIT_EXPECT_EQ(test, um_vec2_stat_read(vdev, UM_VEC2_STAT_CLOSES),
			0ULL);
	KUNIT_EXPECT_EQ(test, um_vec2_netdev_stop(dev), 0);

	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_fd_injected_open_failure_stays_closed_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 10);
	struct net_device *dev = vector2_fd_test_alloc_netdev(test, vdev);
	int ret;

	vdev->cfg.fail_open_after = 1;

	ret = um_vec2_netdev_open(dev);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, vdev->life.state, UM_VEC2_DEV_REGISTERED);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);
	KUNIT_EXPECT_FALSE(test, netif_carrier_ok(dev));
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(vdev, UM_VEC2_STAT_OPEN_ATTEMPTS),
			1ULL);
	KUNIT_EXPECT_EQ(test,
			um_vec2_stat_read(vdev, UM_VEC2_STAT_OPEN_FAILURES),
			1ULL);
	KUNIT_EXPECT_EQ(test, um_vec2_stat_read(vdev, UM_VEC2_STAT_CLOSES),
			0ULL);

	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_fd_missing_config_repeats_closed_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 11);
	unsigned int i;

	vdev->cfg.has_fd = false;

	for (i = 0; i < VECTOR2_FD_FAILURE_STRESS_ITERS; i++) {
		int ret;

		ret = um_vec2_fd_open(vdev);
		if (ret != -EINVAL) {
			KUNIT_FAIL(test, "iter %u fd open ret=%d", i, ret);
			break;
		}
		if (vdev->channels || vdev->num_channels) {
			KUNIT_FAIL(test, "iter %u missing-fd left channels", i);
			break;
		}

		if (!(i & 0x1ff))
			cond_resched();
	}

	KUNIT_EXPECT_EQ(test, i, VECTOR2_FD_FAILURE_STRESS_ITERS);
}

struct vector2_fd_tx_trace {
	struct kunit *test;
	unsigned int packets;
	unsigned int bytes;
	unsigned int expected_frags;
};

static void vector2_fd_tx_complete(void *owner, unsigned int len, void *cookie)
{
	struct vector2_fd_tx_trace *trace = cookie;
	struct sk_buff *skb = owner;

	trace->packets++;
	trace->bytes += len;
	if (trace->test)
		KUNIT_EXPECT_EQ(trace->test, skb_shinfo(skb)->nr_frags,
				trace->expected_frags);
	dev_consume_skb_any(skb);
}

static struct sk_buff *vector2_fd_test_skb(struct kunit *test,
					   struct net_device *dev,
					   const u8 *payload,
					   unsigned int len)
{
	struct sk_buff *skb;

	skb = alloc_skb(len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);

	skb_put_data(skb, payload, len);
	skb->dev = dev;
	skb->ip_summed = CHECKSUM_NONE;
	return skb;
}

static struct sk_buff *vector2_fd_test_frag_skb(struct kunit *test,
						struct net_device *dev,
						const u8 *linear,
						unsigned int linear_len,
						const u8 *frag,
						unsigned int frag_len)
{
	struct sk_buff *skb;
	struct page *page;
	void *addr;

	skb = vector2_fd_test_skb(test, dev, linear, linear_len);
	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);

	addr = page_address(page);
	KUNIT_ASSERT_NOT_NULL(test, addr);
	memcpy(addr, frag, frag_len);
	skb_add_rx_frag(skb, 0, page, 0, frag_len, frag_len);
	return skb;
}

static void *vector2_fd_rx_alloc(unsigned int slot, void *cookie)
{
	struct net_device *dev = cookie;
	struct sk_buff *skb;

	skb = alloc_skb(dev->mtu + ETH_HLEN, GFP_KERNEL);
	if (!skb)
		return NULL;
	skb->dev = dev;
	return skb;
}

static void vector2_fd_rx_release(void *owner, unsigned int len, void *cookie)
{
	struct sk_buff *skb = owner;

	dev_kfree_skb_any(skb);
}

struct vector2_fd_rx_trace {
	struct kunit *test;
	unsigned int packets;
	unsigned int bytes;
	u8 first_payload_byte;
};

static void vector2_fd_rx_consume(void *owner, unsigned int len, void *cookie)
{
	struct vector2_fd_rx_trace *trace = cookie;
	struct sk_buff *skb = owner;

	trace->packets++;
	trace->bytes += len;
	KUNIT_EXPECT_EQ(trace->test, skb->len, len);
	KUNIT_EXPECT_EQ(trace->test, skb->data[ETH_HLEN],
			trace->first_payload_byte);
	dev_kfree_skb_any(skb);
}

static void vector2_fd_tx_batch_writes_frame_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 3);
	struct net_device *dev = vector2_fd_test_alloc_netdev(test, vdev);
	struct um_vec2_channel *channel;
	struct vector2_fd_tx_trace trace = {};
	u8 payload[ETH_HLEN + 8] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x08, 0x00, 0x45, 0x00, 0x00, 0x08,
		0x00, 0x00,
	};
	unsigned char frame[sizeof(payload)];
	struct sk_buff *skb;
	int fds[2] = { -1, -1 };
	int ret;

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 0, 1), 0);
	vdev->cfg.fd = fds[1];
	vdev->cfg.has_fd = true;
	KUNIT_ASSERT_EQ(test, um_vec2_fd_open(vdev), 0);
	channel = &vdev->channels[0];

	skb = vector2_fd_test_skb(test, dev, payload, sizeof(payload));
	KUNIT_ASSERT_EQ(test,
			um_vec2_tx_ring_enqueue(&channel->queue->tx, skb,
						skb->len), 0);

	ret = channel->host->ops->tx_batch(channel->host, &channel->queue->tx,
					   1, vector2_fd_tx_complete,
					   &trace);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_empty(&channel->queue->tx));
	KUNIT_EXPECT_EQ(test, trace.packets, 1U);
	KUNIT_EXPECT_EQ(test, trace.bytes, (unsigned int)sizeof(payload));

	ret = os_read_file(fds[0], frame, sizeof(frame));
	KUNIT_EXPECT_EQ(test, ret, (int)sizeof(frame));
	KUNIT_EXPECT_MEMEQ(test, frame, payload, sizeof(payload));

	um_vec2_fd_close(vdev);
	vector2_fd_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_fd_tx_batch_preserves_frags_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 13);
	struct net_device *dev = vector2_fd_test_alloc_netdev(test, vdev);
	struct um_vec2_channel *channel;
	struct vector2_fd_tx_trace trace = {
		.test = test,
		.expected_frags = 1,
	};
	u8 linear[ETH_HLEN] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x08, 0x00,
	};
	u8 frag[8] = { 0x45, 0x00, 0x00, 0x08, 0x00, 0x00, 0x12, 0x34 };
	unsigned char frame[sizeof(linear) + sizeof(frag)];
	struct sk_buff *skb;
	int fds[2] = { -1, -1 };
	int ret;

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 0, 1), 0);
	vdev->cfg.fd = fds[1];
	vdev->cfg.has_fd = true;
	KUNIT_ASSERT_EQ(test, um_vec2_fd_open(vdev), 0);
	channel = &vdev->channels[0];

	skb = vector2_fd_test_frag_skb(test, dev, linear, sizeof(linear),
				       frag, sizeof(frag));
	KUNIT_ASSERT_EQ(test,
			um_vec2_tx_ring_enqueue(&channel->queue->tx, skb,
						skb->len), 0);

	ret = channel->host->ops->tx_batch(channel->host, &channel->queue->tx,
					   1, vector2_fd_tx_complete,
					   &trace);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_empty(&channel->queue->tx));
	KUNIT_EXPECT_EQ(test, trace.packets, 1U);
	KUNIT_EXPECT_EQ(test, trace.bytes,
			(unsigned int)(sizeof(linear) + sizeof(frag)));

	ret = os_read_file(fds[0], frame, sizeof(frame));
	KUNIT_EXPECT_EQ(test, ret, (int)sizeof(frame));
	KUNIT_EXPECT_MEMEQ(test, frame, linear, sizeof(linear));
	KUNIT_EXPECT_MEMEQ(test, frame + sizeof(linear), frag, sizeof(frag));

	um_vec2_fd_close(vdev);
	vector2_fd_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_fd_rx_batch_reads_frame_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_fd_test_alloc_vdev(test, 4);
	struct net_device *dev = vector2_fd_test_alloc_netdev(test, vdev);
	struct um_vec2_channel *channel;
	struct vector2_fd_rx_trace trace = {
		.test = test,
		.first_payload_byte = 0x45,
	};
	unsigned char frame[ETH_HLEN + 8] = {};
	int fds[2] = { -1, -1 };
	int ret;

	memset(frame, 0xff, ETH_ALEN);
	frame[ETH_ALEN] = 0x02;
	frame[2 * ETH_ALEN] = 0x08;
	frame[2 * ETH_ALEN + 1] = 0x00;
	frame[ETH_HLEN] = 0x45;

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 0, 1), 0);
	vdev->cfg.fd = fds[0];
	vdev->cfg.has_fd = true;
	KUNIT_ASSERT_EQ(test, um_vec2_fd_open(vdev), 0);
	channel = &vdev->channels[0];

	KUNIT_ASSERT_EQ(test, os_write_file(fds[1], frame, sizeof(frame)),
			(int)sizeof(frame));

	ret = channel->host->ops->rx_batch(channel->host, &channel->queue->rx,
					   4, vector2_fd_rx_alloc,
					   vector2_fd_rx_release, dev);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test, channel->queue->rx.filled, 1U);
	KUNIT_EXPECT_EQ(test, channel->queue->rx.prepared_total, 2ULL);
	KUNIT_EXPECT_EQ(test, channel->queue->rx.released_total, 1ULL);
	KUNIT_EXPECT_EQ(test,
			um_vec2_rx_batch_consume(&channel->queue->rx, 1,
						 vector2_fd_rx_consume,
						 &trace), 0);
	KUNIT_EXPECT_EQ(test, trace.packets, 1U);
	KUNIT_EXPECT_EQ(test, trace.bytes, (unsigned int)sizeof(frame));

	um_vec2_fd_close(vdev);
	vector2_fd_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static struct kunit_case vector2_fd_test_cases[] = {
	KUNIT_CASE(vector2_fd_open_close_test),
	KUNIT_CASE(vector2_fd_bad_fd_fails_closed_test),
	KUNIT_CASE(vector2_fd_wrong_type_fails_closed_test),
	KUNIT_CASE(vector2_fd_multiqueue_open_close_test),
	KUNIT_CASE(vector2_fd_multiqueue_missing_second_fd_unwinds_test),
	KUNIT_CASE(vector2_fd_multiqueue_partial_open_unwind_test),
	KUNIT_CASE(vector2_fd_netdev_open_stop_test),
	KUNIT_CASE(vector2_fd_netdev_open_stop_repeats_test),
	KUNIT_CASE(vector2_fd_netdev_bad_fd_unwinds_closed_test),
	KUNIT_CASE(vector2_fd_injected_open_failure_stays_closed_test),
	KUNIT_CASE(vector2_fd_missing_config_repeats_closed_test),
	KUNIT_CASE(vector2_fd_tx_batch_writes_frame_test),
	KUNIT_CASE(vector2_fd_tx_batch_preserves_frags_test),
	KUNIT_CASE(vector2_fd_rx_batch_reads_frame_test),
	{}
};

static struct kunit_suite vector2_fd_test_suite = {
	.name = "um_vector2_host_fd",
	.test_cases = vector2_fd_test_cases,
};

kunit_test_suite(vector2_fd_test_suite);
