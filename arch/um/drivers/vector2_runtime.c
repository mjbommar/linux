// SPDX-License-Identifier: GPL-2.0
/*
 * Shared runtime helpers for UML vector networking v2.
 */

#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/virtio_net.h>

#include <os.h>

#include "vector2_internal.h"

#define UM_VEC2_TX_IOV_MAX	(MAX_SKB_FRAGS + 2)

unsigned int um_vec2_runtime_frame_len(const struct net_device *dev,
				       bool vnet_hdr)
{
	unsigned int len = dev->mtu + ETH_HLEN + VLAN_HLEN;

	if (vnet_hdr)
		len += sizeof(struct virtio_net_hdr);

	return len;
}

static int um_vec2_tx_iov_append(struct iovec *iov, int iovcnt, void *base,
				 size_t len)
{
	if (!len)
		return iovcnt;

	iov[iovcnt].iov_base = base;
	iov[iovcnt].iov_len = len;
	return iovcnt + 1;
}

static int um_vec2_tx_iov_from_skb(struct sk_buff *skb, bool vnet_hdr,
				   struct virtio_net_hdr *hdr,
				   struct iovec *iov)
{
	int iovcnt = 0;
	int nr_frags;
	int frag;
	int ret;

	if (vnet_hdr) {
		ret = virtio_net_hdr_from_skb(skb, hdr, true, false, 0);
		if (ret)
			return ret;
		iovcnt = um_vec2_tx_iov_append(iov, iovcnt, hdr, sizeof(*hdr));
	}

	iovcnt = um_vec2_tx_iov_append(iov, iovcnt, skb->data,
				       skb_headlen(skb));
	nr_frags = skb_shinfo(skb)->nr_frags;
	for (frag = 0; frag < nr_frags; frag++) {
		skb_frag_t *skb_frag = &skb_shinfo(skb)->frags[frag];
		void *addr = skb_frag_address_safe(skb_frag);

		if (!addr)
			return -EFAULT;
		iovcnt = um_vec2_tx_iov_append(iov, iovcnt, addr,
					       skb_frag_size(skb_frag));
	}

	return iovcnt;
}

int um_vec2_write_skb(int fd, struct sk_buff *skb, bool vnet_hdr)
{
	struct iovec iov[UM_VEC2_TX_IOV_MAX];
	struct virtio_net_hdr hdr;
	size_t wire_len = skb->len;
	ssize_t written;
	int iovcnt;

	iovcnt = um_vec2_tx_iov_from_skb(skb, vnet_hdr, &hdr, iov);
	if (iovcnt < 0)
		return iovcnt;
	if (!iovcnt)
		return -EINVAL;

	if (vnet_hdr)
		wire_len += sizeof(hdr);

	written = os_writev(fd, iov, iovcnt);
	if (written <= 0)
		return written;
	if (written != (ssize_t)wire_len)
		return -EIO;

	return skb->len;
}

static void um_vec2_tx_drop_skb(void *owner, unsigned int len, void *cookie)
{
	struct net_device *dev = cookie;
	struct sk_buff *skb = owner;

	if (dev)
		dev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
}

static void um_vec2_rx_release_skb(void *owner, unsigned int len, void *cookie)
{
	struct sk_buff *skb = owner;

	dev_kfree_skb_any(skb);
}

int um_vec2_queue_pair_alloc(struct um_vec2_channel *channel,
			     unsigned int depth)
{
	struct um_vec2_queue_pair *queue;
	int ret;

	queue = kzalloc_obj(*queue);
	if (!queue)
		return -ENOMEM;

	queue->tx_desc = kcalloc(depth, sizeof(*queue->tx_desc), GFP_KERNEL);
	if (!queue->tx_desc) {
		ret = -ENOMEM;
		goto out_free_queue;
	}

	queue->rx_slot = kcalloc(depth, sizeof(*queue->rx_slot), GFP_KERNEL);
	if (!queue->rx_slot) {
		ret = -ENOMEM;
		goto out_free_tx;
	}

	spin_lock_init(&queue->tx_lock);
	spin_lock_init(&queue->rx_lock);

	ret = um_vec2_tx_ring_init(&queue->tx, queue->tx_desc, depth);
	if (ret)
		goto out_free_rx;

	ret = um_vec2_rx_batch_init(&queue->rx, queue->rx_slot, depth);
	if (ret)
		goto out_free_rx;

	channel->queue = queue;
	return 0;

out_free_rx:
	kfree(queue->rx_slot);
out_free_tx:
	kfree(queue->tx_desc);
out_free_queue:
	kfree(queue);
	return ret;
}

void um_vec2_queue_pair_free(struct um_vec2_channel *channel,
			     struct net_device *dev)
{
	struct um_vec2_queue_pair *queue = channel->queue;

	if (!queue)
		return;

	um_vec2_tx_ring_reset(&queue->tx, um_vec2_tx_drop_skb, dev);
	um_vec2_rx_batch_reset(&queue->rx, um_vec2_rx_release_skb, dev);
	kfree(queue->rx_slot);
	kfree(queue->tx_desc);
	kfree(queue);
	channel->queue = NULL;
}
