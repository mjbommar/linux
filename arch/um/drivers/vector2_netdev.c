// SPDX-License-Identifier: GPL-2.0
/*
 * Netdev registration skeleton for UML vector networking v2.
 */

#define pr_fmt(fmt) "uml-vector2: " fmt

#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>

#include "vector2_internal.h"

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
	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_QUIESCING);
	if (ret)
		goto out;

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_REGISTERED);
	if (ret)
		goto out;

	netdev_info(dev, "vector v2 host backend is not implemented yet\n");
	ret = -EOPNOTSUPP;

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

	if (!um_vec2_dev_can_xmit(&vdev->life)) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	dev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
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
