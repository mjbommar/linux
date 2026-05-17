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
#include <linux/virtio_net.h>

#include "vector2_internal.h"

unsigned int um_vec2_runtime_frame_len(const struct net_device *dev,
				       bool vnet_hdr)
{
	unsigned int len = dev->mtu + ETH_HLEN + VLAN_HLEN;

	if (vnet_hdr)
		len += sizeof(struct virtio_net_hdr);

	return len;
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
