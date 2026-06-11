// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the UML vector networking v2 TAP host backend.
 */

#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if.h>
#include <linux/mm.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/virtio_net.h>

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
	vdev->netdev = dev;
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

struct vector2_tap_tx_trace {
	struct kunit *test;
	unsigned int packets;
	unsigned int bytes;
	unsigned int expected_frags;
};

static void vector2_tap_tx_complete(void *owner, unsigned int len, void *cookie)
{
	struct vector2_tap_tx_trace *trace = cookie;
	struct sk_buff *skb = owner;

	trace->packets++;
	trace->bytes += len;
	if (trace->test)
		KUNIT_EXPECT_EQ(trace->test, skb_shinfo(skb)->nr_frags,
				trace->expected_frags);
	dev_consume_skb_any(skb);
}

static struct sk_buff *vector2_tap_test_skb(struct kunit *test,
					    struct net_device *dev,
					    const u8 *payload,
					    unsigned int len)
{
	struct sk_buff *skb;

	skb = alloc_skb(sizeof(struct virtio_net_hdr) + len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);

	skb_reserve(skb, sizeof(struct virtio_net_hdr));
	skb_put_data(skb, payload, len);
	skb->dev = dev;
	skb->ip_summed = CHECKSUM_NONE;
	return skb;
}

static struct sk_buff *vector2_tap_test_frag_skb(struct kunit *test,
						 struct net_device *dev,
						 const u8 *linear,
						 unsigned int linear_len,
						 const u8 *frag,
						 unsigned int frag_len)
{
	struct sk_buff *skb;
	struct page *page;
	void *addr;

	skb = vector2_tap_test_skb(test, dev, linear, linear_len);
	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);

	addr = page_address(page);
	KUNIT_ASSERT_NOT_NULL(test, addr);
	memcpy(addr, frag, frag_len);
	skb_add_rx_frag(skb, 0, page, 0, frag_len, frag_len);
	return skb;
}

static void *vector2_tap_rx_alloc(unsigned int slot, void *cookie)
{
	struct net_device *dev = cookie;
	struct sk_buff *skb;

	skb = alloc_skb(sizeof(struct virtio_net_hdr) + dev->mtu + ETH_HLEN,
			GFP_KERNEL);
	if (!skb)
		return NULL;
	skb->dev = dev;
	return skb;
}

static void vector2_tap_rx_release(void *owner, unsigned int len, void *cookie)
{
	struct sk_buff *skb = owner;

	dev_kfree_skb_any(skb);
}

struct vector2_tap_rx_trace {
	struct kunit *test;
	unsigned int packets;
	unsigned int bytes;
	u8 first_payload_byte;
	bool check_ip_summed;
	u8 expected_ip_summed;
};

static void vector2_tap_rx_consume(void *owner, unsigned int len, void *cookie)
{
	struct vector2_tap_rx_trace *trace = cookie;
	struct sk_buff *skb = owner;

	trace->packets++;
	trace->bytes += len;
	KUNIT_EXPECT_EQ(trace->test, skb->len, len);
	KUNIT_EXPECT_EQ(trace->test, skb->data[ETH_HLEN],
			trace->first_payload_byte);
	if (trace->check_ip_summed)
		KUNIT_EXPECT_EQ(trace->test, (u8)skb->ip_summed,
				trace->expected_ip_summed);
	dev_kfree_skb_any(skb);
}

static void vector2_tap_attach_close_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 0);
	struct net_device *dev = vector2_tap_test_alloc_netdev(test, vdev);
	int fds[2] = { -1, -1 };

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);

	KUNIT_EXPECT_EQ(test, um_vec2_tap_attach_fd(vdev, fds[0]), 0);
	fds[0] = -1;
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels);
	KUNIT_ASSERT_NOT_NULL(test, vdev->channels[0].host);
	KUNIT_EXPECT_STREQ(test, vdev->channels[0].host->ops->name, "tap");
	KUNIT_EXPECT_EQ(test, vdev->channels[0].index, 0U);
	KUNIT_EXPECT_EQ(test, vdev->channels[0].life.state,
			UM_VEC2_CHAN_FD_ATTACHED);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 1U);

	um_vec2_tap_close(vdev);
	KUNIT_EXPECT_NULL(test, vdev->channels);
	KUNIT_EXPECT_EQ(test, vdev->num_channels, 0U);

	vector2_tap_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_tap_attach_rejects_busy_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 1);
	struct net_device *dev = vector2_tap_test_alloc_netdev(test, vdev);
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
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_tap_tx_batch_writes_frame_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 4);
	struct net_device *dev = vector2_tap_test_alloc_netdev(test, vdev);
	struct um_vec2_channel *channel;
	struct vector2_tap_tx_trace trace = {};
	u8 payload[ETH_HLEN + 8] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x08, 0x00, 0x45, 0x00, 0x00, 0x08,
		0x00, 0x00,
	};
	unsigned char frame[sizeof(struct virtio_net_hdr) + sizeof(payload)];
	struct sk_buff *skb;
	int fds[2] = { -1, -1 };
	int ret;

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tap_attach_fd(vdev, fds[1]), 0);
	fds[1] = -1;
	channel = &vdev->channels[0];

	skb = vector2_tap_test_skb(test, dev, payload, sizeof(payload));
	KUNIT_ASSERT_EQ(test,
			um_vec2_tx_ring_enqueue(&channel->queue->tx, skb,
						skb->len), 0);

	ret = channel->host->ops->tx_batch(channel->host, &channel->queue->tx,
					   1, vector2_tap_tx_complete,
					   &trace);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_empty(&channel->queue->tx));
	KUNIT_EXPECT_EQ(test, trace.packets, 1U);
	KUNIT_EXPECT_EQ(test, trace.bytes, (unsigned int)sizeof(payload));

	ret = os_read_file(fds[0], frame, sizeof(frame));
	KUNIT_EXPECT_EQ(test, ret, (int)sizeof(frame));
	KUNIT_EXPECT_MEMEQ(test, frame + sizeof(struct virtio_net_hdr),
			   payload, sizeof(payload));

	um_vec2_tap_close(vdev);
	vector2_tap_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_tap_tx_batch_preserves_frags_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 14);
	struct net_device *dev = vector2_tap_test_alloc_netdev(test, vdev);
	struct um_vec2_channel *channel;
	struct vector2_tap_tx_trace trace = {
		.test = test,
		.expected_frags = 1,
	};
	u8 linear[ETH_HLEN] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x08, 0x00,
	};
	u8 frag[8] = { 0x45, 0x00, 0x00, 0x08, 0x00, 0x00, 0x12, 0x34 };
	unsigned char frame[sizeof(struct virtio_net_hdr) + sizeof(linear) +
			    sizeof(frag)];
	struct sk_buff *skb;
	int fds[2] = { -1, -1 };
	int ret;

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tap_attach_fd(vdev, fds[1]), 0);
	fds[1] = -1;
	channel = &vdev->channels[0];

	skb = vector2_tap_test_frag_skb(test, dev, linear, sizeof(linear),
					frag, sizeof(frag));
	KUNIT_ASSERT_EQ(test,
			um_vec2_tx_ring_enqueue(&channel->queue->tx, skb,
						skb->len), 0);

	ret = channel->host->ops->tx_batch(channel->host, &channel->queue->tx,
					   1, vector2_tap_tx_complete,
					   &trace);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_empty(&channel->queue->tx));
	KUNIT_EXPECT_EQ(test, trace.packets, 1U);
	KUNIT_EXPECT_EQ(test, trace.bytes,
			(unsigned int)(sizeof(linear) + sizeof(frag)));

	ret = os_read_file(fds[0], frame, sizeof(frame));
	KUNIT_EXPECT_EQ(test, ret, (int)sizeof(frame));
	KUNIT_EXPECT_MEMEQ(test, frame + sizeof(struct virtio_net_hdr),
			   linear, sizeof(linear));
	KUNIT_EXPECT_MEMEQ(test, frame + sizeof(struct virtio_net_hdr) +
			   sizeof(linear), frag, sizeof(frag));

	um_vec2_tap_close(vdev);
	vector2_tap_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_tap_rx_batch_reads_frame_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 5);
	struct net_device *dev = vector2_tap_test_alloc_netdev(test, vdev);
	struct um_vec2_channel *channel;
	struct vector2_tap_rx_trace trace = {
		.test = test,
		.first_payload_byte = 0x45,
	};
	unsigned char frame[sizeof(struct virtio_net_hdr) + ETH_HLEN + 8] = {
	};
	int fds[2] = { -1, -1 };
	int ret;

	memset(frame + sizeof(struct virtio_net_hdr), 0xff, ETH_ALEN);
	frame[sizeof(struct virtio_net_hdr) + ETH_ALEN] = 0x02;
	frame[sizeof(struct virtio_net_hdr) + 2 * ETH_ALEN] = 0x08;
	frame[sizeof(struct virtio_net_hdr) + 2 * ETH_ALEN + 1] = 0x00;
	frame[sizeof(struct virtio_net_hdr) + ETH_HLEN] = 0x45;

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tap_attach_fd(vdev, fds[0]), 0);
	fds[0] = -1;
	channel = &vdev->channels[0];

	KUNIT_ASSERT_EQ(test, os_write_file(fds[1], frame, sizeof(frame)),
			(int)sizeof(frame));

	ret = channel->host->ops->rx_batch(channel->host, &channel->queue->rx,
					   4, vector2_tap_rx_alloc,
					   vector2_tap_rx_release, dev);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test, channel->queue->rx.filled, 1U);
	KUNIT_EXPECT_EQ(test, channel->queue->rx.prepared_total, 2ULL);
	KUNIT_EXPECT_EQ(test, channel->queue->rx.released_total, 1ULL);
	KUNIT_EXPECT_EQ(test,
			um_vec2_rx_batch_consume(&channel->queue->rx, 1,
						 vector2_tap_rx_consume,
						 &trace), 0);
	KUNIT_EXPECT_EQ(test, trace.packets, 1U);
	KUNIT_EXPECT_EQ(test, trace.bytes, (unsigned int)(ETH_HLEN + 8));

	um_vec2_tap_close(vdev);
	vector2_tap_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

static void vector2_tap_rx_data_valid_sets_checksum_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 17);
	struct net_device *dev = vector2_tap_test_alloc_netdev(test, vdev);
	struct um_vec2_channel *channel;
	struct vector2_tap_rx_trace trace = {
		.test = test,
		.first_payload_byte = 0x45,
		.check_ip_summed = true,
		.expected_ip_summed = CHECKSUM_UNNECESSARY,
	};
	unsigned char frame[sizeof(struct virtio_net_hdr) + ETH_HLEN + 8] = {
	};
	struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)frame;
	int fds[2] = { -1, -1 };
	int ret;

	hdr->flags = VIRTIO_NET_HDR_F_DATA_VALID;
	memset(frame + sizeof(struct virtio_net_hdr), 0xff, ETH_ALEN);
	frame[sizeof(struct virtio_net_hdr) + ETH_ALEN] = 0x02;
	frame[sizeof(struct virtio_net_hdr) + 2 * ETH_ALEN] = 0x08;
	frame[sizeof(struct virtio_net_hdr) + 2 * ETH_ALEN + 1] = 0x00;
	frame[sizeof(struct virtio_net_hdr) + ETH_HLEN] = 0x45;

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tap_attach_fd(vdev, fds[0]), 0);
	fds[0] = -1;
	channel = &vdev->channels[0];

	KUNIT_ASSERT_EQ(test, os_write_file(fds[1], frame, sizeof(frame)),
			(int)sizeof(frame));

	ret = channel->host->ops->rx_batch(channel->host, &channel->queue->rx,
					   4, vector2_tap_rx_alloc,
					   vector2_tap_rx_release, dev);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test,
			um_vec2_rx_batch_consume(&channel->queue->rx, 1,
						 vector2_tap_rx_consume,
						 &trace), 0);
	KUNIT_EXPECT_EQ(test, trace.packets, 1U);
	KUNIT_EXPECT_EQ(test, trace.bytes, (unsigned int)(ETH_HLEN + 8));

	um_vec2_tap_close(vdev);
	vector2_tap_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
}

/*
 * Short frames must propagate -EPROTO from rx_batch.
 *
 * um_vec2_tap_read_skb() returns -EPROTO when the host-read frame is
 * too short to contain a virtio_net_hdr. The RX loop must preserve
 * that error so NAPI sees -EPROTO and UM_VEC2_STAT_RX_PROTO_DROPS is
 * updated for TAP.
 */
static void vector2_tap_rx_batch_short_frame_returns_eproto_test(struct kunit *test)
{
	struct um_vec2_dev *vdev = vector2_tap_test_alloc_vdev(test, 16);
	struct net_device *dev = vector2_tap_test_alloc_netdev(test, vdev);
	struct um_vec2_channel *channel;
	/*
	 * 4 bytes is well below sizeof(struct virtio_net_hdr).  read_skb
	 * sees ret <= sizeof(hdr) and returns -EPROTO.
	 */
	unsigned char short_frame[4] = { 0 };
	int fds[2] = { -1, -1 };
	int ret;

	KUNIT_ASSERT_EQ(test, os_pipe(fds, 1, 1), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tap_attach_fd(vdev, fds[0]), 0);
	fds[0] = -1;
	channel = &vdev->channels[0];

	KUNIT_ASSERT_EQ(test,
			os_write_file(fds[1], short_frame, sizeof(short_frame)),
			(int)sizeof(short_frame));

	ret = channel->host->ops->rx_batch(channel->host, &channel->queue->rx,
					   1, vector2_tap_rx_alloc,
					   vector2_tap_rx_release, dev);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);

	um_vec2_tap_close(vdev);
	vector2_tap_test_close_pipe(fds);
	vdev->netdev = NULL;
	free_netdev(dev);
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

	vdev->netdev = NULL;
	free_netdev(dev);
}

static struct kunit_case vector2_tap_test_cases[] = {
	KUNIT_CASE(vector2_tap_attach_close_test),
	KUNIT_CASE(vector2_tap_attach_rejects_busy_test),
	KUNIT_CASE(vector2_tap_tx_batch_writes_frame_test),
	KUNIT_CASE(vector2_tap_tx_batch_preserves_frags_test),
	KUNIT_CASE(vector2_tap_rx_batch_reads_frame_test),
	KUNIT_CASE(vector2_tap_rx_data_valid_sets_checksum_test),
	KUNIT_CASE(vector2_tap_rx_batch_short_frame_returns_eproto_test),
	KUNIT_CASE(vector2_tap_sandbox_open_fails_closed_test),
	KUNIT_CASE(vector2_tap_netdev_open_unwinds_sandbox_test),
	{}
};

static struct kunit_suite vector2_tap_test_suite = {
	.name = "um_vector2_host_tap",
	.test_cases = vector2_tap_test_cases,
};

kunit_test_suite(vector2_tap_test_suite);
