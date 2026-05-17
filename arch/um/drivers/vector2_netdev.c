// SPDX-License-Identifier: GPL-2.0
/*
 * Netdev registration skeleton for UML vector networking v2.
 */

#define pr_fmt(fmt) "uml-vector2: " fmt

#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/virtio_net.h>

#include <irq_kern.h>
#include <irq_user.h>

#include "vector2_internal.h"

#define UM_VEC2_NAPI_MAX_WEIGHT	64U

static const struct net_device_ops um_vec2_netdev_ops = {
	.ndo_open		= um_vec2_netdev_open,
	.ndo_stop		= um_vec2_netdev_stop,
	.ndo_start_xmit		= um_vec2_netdev_start_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static struct um_vec2_dev *um_vec2_from_netdev(struct net_device *dev)
{
	struct um_vec2_netdev_priv *priv = netdev_priv(dev);

	return priv->vdev;
}

static int um_vec2_open_backend(struct um_vec2_dev *vdev)
{
	switch (vdev->cfg.transport) {
	case UM_VEC2_TRANSPORT_TAP:
		return um_vec2_tap_open(vdev);
	case UM_VEC2_TRANSPORT_FD:
		return um_vec2_fd_open(vdev);
	default:
		return -EOPNOTSUPP;
	}
}

static void um_vec2_close_backend(struct um_vec2_dev *vdev)
{
	switch (vdev->cfg.transport) {
	case UM_VEC2_TRANSPORT_TAP:
		um_vec2_tap_close(vdev);
		break;
	case UM_VEC2_TRANSPORT_FD:
		um_vec2_fd_close(vdev);
		break;
	default:
		break;
	}
}

static struct um_vec2_channel *um_vec2_first_channel(struct um_vec2_dev *vdev)
{
	if (!vdev->channels || !vdev->num_channels)
		return NULL;
	return &vdev->channels[0];
}

static void um_vec2_tx_complete_skb(void *owner, unsigned int len,
				    void *cookie)
{
	struct net_device *dev = cookie;
	struct sk_buff *skb = owner;

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;
	dev_consume_skb_any(skb);
}

static void um_vec2_rx_free_skb(void *owner, unsigned int len, void *cookie)
{
	struct sk_buff *skb = owner;

	dev_kfree_skb_any(skb);
}

struct um_vec2_rx_poll_ctx {
	struct napi_struct *napi;
	struct net_device *dev;
	unsigned int frame_len;
};

static void *um_vec2_rx_alloc_skb(unsigned int slot, void *cookie)
{
	struct um_vec2_rx_poll_ctx *ctx = cookie;
	struct sk_buff *skb;

	skb = napi_alloc_skb(ctx->napi, ctx->frame_len);
	if (!skb)
		return NULL;
	skb->dev = ctx->dev;
	return skb;
}

static void um_vec2_rx_deliver_skb(void *owner, unsigned int len, void *cookie)
{
	struct um_vec2_rx_poll_ctx *ctx = cookie;
	struct sk_buff *skb = owner;

	skb->protocol = eth_type_trans(skb, ctx->dev);
	ctx->dev->stats.rx_packets++;
	ctx->dev->stats.rx_bytes += skb->len;
	napi_gro_receive(ctx->napi, skb);
}

static unsigned int um_vec2_napi_weight(const struct um_vec2_dev *vdev)
{
	return min_t(unsigned int, vdev->cfg.depth, UM_VEC2_NAPI_MAX_WEIGHT);
}

static int um_vec2_netdev_poll(struct napi_struct *napi, int budget)
{
	struct um_vec2_channel *channel =
		container_of(napi, struct um_vec2_channel, napi);
	struct um_vec2_dev *vdev = channel->vdev;
	struct net_device *dev = vdev->netdev;
	struct um_vec2_queue_pair *queue = channel->queue;
	struct um_vec2_rx_poll_ctx rx_ctx;
	bool tx_more = false;
	int tx_done = 0;
	int rx_done = 0;

	if (!channel->host || !queue)
		goto complete;

	spin_lock(&queue->tx_lock);
	if (!um_vec2_tx_ring_empty(&queue->tx)) {
		tx_done = channel->host->ops->tx_batch(channel->host,
						       &queue->tx,
						       queue->tx.depth,
						       um_vec2_tx_complete_skb,
						       dev);
		if (tx_done > 0) {
			netif_wake_queue(dev);
			netif_trans_update(dev);
		}
		tx_more = !um_vec2_tx_ring_empty(&queue->tx);
	}
	spin_unlock(&queue->tx_lock);

	if (tx_done == -ENODEV)
		goto backend_dead;

	rx_ctx.napi = napi;
	rx_ctx.dev = dev;
	rx_ctx.frame_len = dev->mtu + ETH_HLEN + VLAN_HLEN +
			   sizeof(struct virtio_net_hdr);

	spin_lock(&queue->rx_lock);
	rx_done = channel->host->ops->rx_batch(channel->host, &queue->rx,
					       budget, um_vec2_rx_alloc_skb,
					       um_vec2_rx_free_skb, &rx_ctx);
	if (rx_done > 0)
		um_vec2_rx_batch_consume(&queue->rx, rx_done,
					 um_vec2_rx_deliver_skb, &rx_ctx);
	else if (rx_done == -EPROTO)
		dev->stats.rx_dropped++;
	spin_unlock(&queue->rx_lock);

	if (rx_done == -ENODEV)
		goto backend_dead;
	if (rx_done < 0)
		rx_done = 0;

complete:
	if (rx_done < budget)
		napi_complete_done(napi, rx_done);
	if (tx_more && tx_done > 0)
		napi_schedule(napi);
	return rx_done;

backend_dead:
	netif_stop_queue(dev);
	netif_carrier_off(dev);
	if (rx_done < 0)
		rx_done = 0;
	if (rx_done < budget)
		napi_complete_done(napi, rx_done);
	return rx_done;
}

static irqreturn_t um_vec2_rx_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct um_vec2_dev *vdev = um_vec2_from_netdev(dev);
	struct um_vec2_channel *channel = um_vec2_first_channel(vdev);

	if (!channel || !netif_running(dev))
		return IRQ_NONE;
	napi_schedule(&channel->napi);
	return IRQ_HANDLED;
}

static int um_vec2_start_datapath(struct net_device *dev,
				  struct um_vec2_dev *vdev)
{
	struct um_vec2_channel *channel = um_vec2_first_channel(vdev);
	int fd;
	int ret;

	if (vdev->cfg.transport != UM_VEC2_TRANSPORT_TAP)
		return 0;
	if (!channel || !channel->host || !channel->queue)
		return -EINVAL;

	netif_napi_add_weight(dev, &channel->napi, um_vec2_netdev_poll,
			      um_vec2_napi_weight(vdev));
	channel->napi_added = true;

	fd = um_vec2_tap_fd(channel);
	if (fd < 0) {
		ret = fd;
		goto out_del_napi;
	}

	ret = um_request_irq(UM_IRQ_ALLOC, fd,
			     IRQ_READ, um_vec2_rx_interrupt, IRQF_SHARED,
			     dev->name, dev);
	if (ret < 0)
		goto out_del_napi;

	channel->rx_irq = ret;
	dev->irq = ret;

	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_IRQ_ATTACHED);
	if (ret)
		goto out_free_irq;

	napi_enable(&channel->napi);
	channel->napi_enabled = true;

	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_NAPI_ENABLED);
	if (ret)
		goto out_disable_napi;

	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_ACTIVE);
	if (ret)
		goto out_disable_napi;

	return 0;

out_disable_napi:
	if (channel->napi_enabled) {
		napi_disable(&channel->napi);
		channel->napi_enabled = false;
	}
out_free_irq:
	um_free_irq(channel->rx_irq, dev);
	channel->rx_irq = UM_VEC2_NO_IRQ;
out_del_napi:
	netif_napi_del(&channel->napi);
	channel->napi_added = false;
	return ret;
}

static void um_vec2_stop_datapath(struct net_device *dev,
				  struct um_vec2_dev *vdev)
{
	struct um_vec2_channel *channel = um_vec2_first_channel(vdev);

	if (!channel)
		return;

	if (channel->napi_enabled) {
		napi_disable(&channel->napi);
		channel->napi_enabled = false;
	}
	if (channel->rx_irq != UM_VEC2_NO_IRQ) {
		um_free_irq(channel->rx_irq, dev);
		channel->rx_irq = UM_VEC2_NO_IRQ;
		dev->irq = 0;
	}
	if (channel->napi_added) {
		netif_napi_del(&channel->napi);
		channel->napi_added = false;
	}
}

static int um_vec2_unwind_open(struct um_vec2_dev *vdev)
{
	int ret;

	um_vec2_close_backend(vdev);

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_QUIESCING);
	if (ret)
		return ret;

	return um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_REGISTERED);
}

int um_vec2_netdev_open(struct net_device *dev)
{
	struct um_vec2_dev *vdev = um_vec2_from_netdev(dev);
	int ret;

	mutex_lock(&vdev->lock);
	if (!um_vec2_dev_can_open(&vdev->life)) {
		ret = -EINVAL;
		goto out;
	}

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_OPENING);
	if (ret)
		goto out;

	netif_carrier_off(dev);
	netif_stop_queue(dev);

	ret = um_vec2_open_backend(vdev);
	if (ret) {
		netdev_info(dev, "vector v2 host backend is not available: %d\n",
			    ret);
		if (um_vec2_unwind_open(vdev))
			netdev_err(dev, "vector v2 open unwind failed\n");
		goto out;
	}

	ret = um_vec2_start_datapath(dev, vdev);
	if (ret) {
		netdev_err(dev, "vector v2 datapath start failed: %d\n", ret);
		if (um_vec2_unwind_open(vdev))
			netdev_err(dev, "vector v2 open unwind failed\n");
		goto out;
	}

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_RUNNING);
	if (ret) {
		um_vec2_stop_datapath(dev, vdev);
		if (um_vec2_unwind_open(vdev))
			netdev_err(dev, "vector v2 open unwind failed\n");
		goto out;
	}

	if (vdev->cfg.transport == UM_VEC2_TRANSPORT_TAP) {
		netif_carrier_on(dev);
		netif_start_queue(dev);
		napi_schedule(&um_vec2_first_channel(vdev)->napi);
	}

out:
	mutex_unlock(&vdev->lock);
	return ret;
}

int um_vec2_netdev_stop(struct net_device *dev)
{
	struct um_vec2_dev *vdev = um_vec2_from_netdev(dev);
	int ret = 0;

	mutex_lock(&vdev->lock);
	netif_stop_queue(dev);
	netif_carrier_off(dev);

	switch (vdev->life.state) {
	case UM_VEC2_DEV_REGISTERED:
		break;
	case UM_VEC2_DEV_RUNNING:
	case UM_VEC2_DEV_OPENING:
		ret = um_vec2_dev_transition(&vdev->life,
					     UM_VEC2_DEV_QUIESCING);
		if (ret)
			break;
		um_vec2_stop_datapath(dev, vdev);
		um_vec2_close_backend(vdev);
		ret = um_vec2_dev_transition(&vdev->life,
					     UM_VEC2_DEV_REGISTERED);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&vdev->lock);
	return ret;
}

netdev_tx_t um_vec2_netdev_start_xmit(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct um_vec2_dev *vdev = um_vec2_from_netdev(dev);
	struct um_vec2_channel *channel;
	struct um_vec2_queue_pair *queue;
	unsigned int len = skb->len;
	int ret;

	if (!um_vec2_dev_can_xmit(&vdev->life)) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	channel = um_vec2_first_channel(vdev);
	if (!channel || !channel->host || !channel->queue) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	queue = channel->queue;
	spin_lock(&queue->tx_lock);
	if (um_vec2_tx_ring_full(&queue->tx)) {
		netif_stop_queue(dev);
		spin_unlock(&queue->tx_lock);
		return NETDEV_TX_BUSY;
	}

	ret = um_vec2_tx_ring_enqueue(&queue->tx, skb, len);
	if (ret) {
		spin_unlock(&queue->tx_lock);
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (um_vec2_tx_ring_full(&queue->tx))
		netif_stop_queue(dev);
	spin_unlock(&queue->tx_lock);

	napi_schedule(&channel->napi);
	return NETDEV_TX_OK;
}

void um_vec2_netdev_init(struct um_vec2_dev *vdev, struct net_device *dev)
{
	struct um_vec2_netdev_priv *priv = netdev_priv(dev);

	priv->vdev = vdev;
	snprintf(dev->name, sizeof(dev->name), "%s.%u",
		 UM_VEC2_NAME_PREFIX, vdev->unit);

	dev->mtu = vdev->cfg.mtu;
	dev->min_mtu = UM_VEC2_MIN_MTU;
	dev->max_mtu = UM_VEC2_MAX_MTU;
	dev->netdev_ops = &um_vec2_netdev_ops;
	dev->watchdog_timeo = HZ;
	dev->irq = 0;

	if (vdev->cfg.has_mac)
		eth_hw_addr_set(dev, vdev->cfg.mac);
	else
		eth_hw_addr_random(dev);

	um_vec2_ethtool_attach(dev);
	netif_carrier_off(dev);
}

int um_vec2_netdev_register(struct um_vec2_dev *vdev)
{
	struct net_device *dev;
	int ret;

	if (vdev->life.state != UM_VEC2_DEV_CONFIGURED)
		return -EINVAL;

	dev = alloc_etherdev_mqs(sizeof(struct um_vec2_netdev_priv), 1, 1);
	if (!dev)
		return -ENOMEM;

	um_vec2_netdev_init(vdev, dev);

	rtnl_lock();
	ret = register_netdevice(dev);
	rtnl_unlock();
	if (ret)
		goto out_free_netdev;

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_REGISTERED);
	if (ret)
		goto out_unregister_netdev;

	vdev->netdev = dev;
	vdev->num_channels = 1;
	pr_info("registered netdev %s for vec2.%u\n", dev->name, vdev->unit);
	return 0;

out_unregister_netdev:
	unregister_netdev(dev);
out_free_netdev:
	free_netdev(dev);
	return ret;
}

void um_vec2_netdev_unregister(struct um_vec2_dev *vdev)
{
	struct net_device *dev = vdev->netdev;

	if (!dev)
		return;

	vdev->netdev = NULL;
	unregister_netdev(dev);
	free_netdev(dev);
}
