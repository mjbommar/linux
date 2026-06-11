// SPDX-License-Identifier: GPL-2.0
/*
 * Trusted TAP host backend for UML vector networking v2.
 */

#define pr_fmt(fmt) "uml-vector2-tap: " fmt

#include <linux/container_of.h>
#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/virtio_net.h>

#include <os.h>

#include "vector2_internal.h"

#define UM_VEC2_TUN_PATH	"/dev/net/tun"
#define UM_VEC2_TAP_MAX_BATCH	64U

struct um_vec2_tap_host {
	struct um_vec2_host host;
	struct net_device *dev;
	unsigned int frame_len;
	int fd;
};

static int um_vec2_tap_write_skb(struct um_vec2_tap_host *taphost,
				 struct sk_buff *skb);
static int um_vec2_tap_read_skb(struct um_vec2_tap_host *taphost,
				struct sk_buff *skb);

static struct um_vec2_tap_host *um_vec2_host_to_tap(struct um_vec2_host *host)
{
	return container_of(host, struct um_vec2_tap_host, host);
}

static int um_vec2_tap_tx_batch(struct um_vec2_host *host,
				struct um_vec2_tx_ring *ring,
				unsigned int budget,
				um_vec2_queue_release_fn complete,
				void *cookie)
{
	struct um_vec2_tap_host *taphost = um_vec2_host_to_tap(host);
	unsigned int sent = 0;
	int ret = 0;

	while (sent < budget && !um_vec2_tx_ring_empty(ring)) {
		const struct um_vec2_tx_desc *desc;
		struct sk_buff *skb;

		desc = um_vec2_tx_ring_peek(ring, 0);
		if (!desc)
			break;

		skb = desc->owner;
		ret = um_vec2_tap_write_skb(taphost, skb);
		if (!ret)
			break;
		if (ret < 0)
			return sent ? (int)sent : ret;

		ret = um_vec2_tx_ring_complete(ring, 1, complete, cookie);
		if (ret)
			return sent ? (int)sent : ret;
		sent++;
	}

	return sent;
}

static int um_vec2_tap_rx_batch(struct um_vec2_host *host,
				struct um_vec2_rx_batch *batch,
				unsigned int budget, um_vec2_rx_alloc_fn alloc,
				um_vec2_queue_release_fn release, void *cookie)
{
	struct um_vec2_tap_host *taphost = um_vec2_host_to_tap(host);
	unsigned int lens[UM_VEC2_TAP_MAX_BATCH];
	unsigned int received = 0;
	unsigned int i;
	int ret;

	if (!budget)
		return 0;
	if (budget > batch->depth)
		return -EINVAL;
	if (budget > UM_VEC2_TAP_MAX_BATCH)
		budget = UM_VEC2_TAP_MAX_BATCH;

	ret = um_vec2_rx_batch_prepare(batch, budget, alloc, release, cookie);
	if (ret)
		return ret;

	for (i = 0; i < budget; i++) {
		struct sk_buff *skb = batch->slot[i].owner;

		ret = um_vec2_tap_read_skb(taphost, skb);
		if (!ret || ret == -EAGAIN)
			break;
		if (ret < 0)
			goto complete;
		lens[received++] = ret;
	}

	ret = 0;

complete:
	if (um_vec2_rx_batch_complete(batch, received, lens, release, cookie))
		return -EIO;
	if (received)
		return received;
	return ret == -EAGAIN ? 0 : ret;
}

static const struct um_vec2_host_ops um_vec2_tap_host_ops = {
	.name		= "tap",
	.tx_batch	= um_vec2_tap_tx_batch,
	.rx_batch	= um_vec2_tap_rx_batch,
};

static void um_vec2_tap_host_close(struct um_vec2_tap_host *taphost)
{
	if (!taphost)
		return;

	if (taphost->fd >= 0)
		os_close_file(taphost->fd);
	taphost->fd = -1;
}

static int um_vec2_tap_write_skb(struct um_vec2_tap_host *taphost,
				 struct sk_buff *skb)
{
	int ret = um_vec2_write_skb(taphost->fd, skb, true);

	if (ret == -EAGAIN || ret == -ENOBUFS)
		return 0;
	return ret;
}

static int um_vec2_tap_read_skb(struct um_vec2_tap_host *taphost,
				struct sk_buff *skb)
{
	struct virtio_net_hdr hdr;
	unsigned char *data;
	int ret;

	data = skb_put(skb, taphost->frame_len);
	ret = os_read_file(taphost->fd, data, taphost->frame_len);
	if (ret == -EAGAIN)
		return 0;
	if (ret < 0)
		return ret;
	if (ret <= sizeof(hdr))
		return -EPROTO;

	skb_trim(skb, ret);
	memcpy(&hdr, skb->data, sizeof(hdr));
	skb_pull(skb, sizeof(hdr));

	ret = virtio_net_hdr_to_skb(skb, &hdr, true);
	if (ret)
		return -EPROTO;

	skb->dev = taphost->dev;
	return skb->len;
}

static int um_vec2_tap_create_fd(const char *ifname, bool multi_queue)
{
	struct ifreq ifr = {};
	int offload = TUN_F_CSUM | TUN_F_TSO4 | TUN_F_TSO6;
	int fd;
	int ret;

	fd = os_open_file(UM_VEC2_TUN_PATH, of_cloexec(of_rdwr(OPENFLAGS())),
			  0);
	if (fd < 0)
		return fd;

	ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR;
	if (multi_queue)
		ifr.ifr_flags |= IFF_MULTI_QUEUE;
	strscpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

	ret = os_ioctl_generic(fd, TUNSETIFF, (unsigned long)&ifr);
	if (ret)
		goto out_close;

	ret = os_set_fd_block(fd, 0);
	if (ret)
		goto out_close;

	ret = os_ioctl_generic(fd, TUNSETOFFLOAD, (unsigned long)offload);
	if (ret)
		pr_info("TUNSETOFFLOAD failed for %s: %d\n", ifname, ret);

	return fd;

out_close:
	os_close_file(fd);
	return ret;
}

static struct um_vec2_tap_host *um_vec2_tap_host_alloc(void)
{
	struct um_vec2_tap_host *taphost;

	taphost = kzalloc_obj(*taphost);
	if (!taphost)
		return NULL;

	taphost->fd = UM_VEC2_NO_FD;
	return taphost;
}

static void um_vec2_tap_channel_init(struct um_vec2_dev *vdev,
				     struct um_vec2_channel *channel,
				     unsigned int index, int fd)
{
	um_vec2_chan_lifecycle_init(&channel->life);
	channel->vdev = vdev;
	channel->index = index;
	channel->rx_fd = fd;
	channel->tx_fd = fd;
	channel->rx_irq = UM_VEC2_NO_IRQ;
	channel->tx_irq = UM_VEC2_NO_IRQ;
}

static int um_vec2_tap_channel_prepare(struct um_vec2_dev *vdev,
				       struct um_vec2_channel *channel,
				       unsigned int index, int fd)
{
	int ret;

	um_vec2_tap_channel_init(vdev, channel, index, fd);
	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_ALLOCATED);
	if (ret)
		return ret;

	return um_vec2_queue_pair_alloc(channel, vdev->cfg.depth);
}

static void um_vec2_tap_attach_host(struct um_vec2_tap_host *taphost,
				    struct um_vec2_channel *channel,
				    struct net_device *dev, int fd)
{
	taphost->host.ops = &um_vec2_tap_host_ops;
	taphost->dev = dev;
	taphost->frame_len = um_vec2_runtime_frame_len(dev, true);
	taphost->fd = fd;
	channel->host = &taphost->host;
}

static void um_vec2_tap_channel_mark_closed(struct um_vec2_channel *channel)
{
	if (um_vec2_chan_can_transition(channel->life.state,
					UM_VEC2_CHAN_QUIESCING))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_QUIESCING);
	if (um_vec2_chan_can_transition(channel->life.state, UM_VEC2_CHAN_CLOSED))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_CLOSED);
}

static int um_vec2_tap_channel_attach_fd(struct um_vec2_dev *vdev,
					 struct um_vec2_channel *channel,
					 unsigned int index, int fd)
{
	struct um_vec2_tap_host *taphost;
	struct net_device *dev = vdev->netdev;
	int ret;

	taphost = um_vec2_tap_host_alloc();
	if (!taphost)
		return -ENOMEM;

	ret = um_vec2_tap_channel_prepare(vdev, channel, index, fd);
	if (ret)
		goto out_free_host;

	um_vec2_tap_attach_host(taphost, channel, dev, fd);
	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_FD_ATTACHED);
	if (ret)
		goto out_free_queue;

	return 0;

out_free_queue:
	channel->host = NULL;
	um_vec2_queue_pair_free(channel, dev);
	um_vec2_tap_channel_mark_closed(channel);
out_free_host:
	kfree(taphost);
	return ret;
}

static void um_vec2_tap_channel_close(struct um_vec2_channel *channel,
				      struct net_device *dev)
{
	struct um_vec2_tap_host *taphost;

	if (!channel)
		return;

	taphost = channel->host ? um_vec2_host_to_tap(channel->host) : NULL;
	if (um_vec2_chan_can_transition(channel->life.state,
					UM_VEC2_CHAN_QUIESCING))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_QUIESCING);
	um_vec2_tap_host_close(taphost);
	if (um_vec2_chan_can_transition(channel->life.state, UM_VEC2_CHAN_CLOSED))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_CLOSED);

	um_vec2_queue_pair_free(channel, dev);
	channel->rx_fd = UM_VEC2_NO_FD;
	channel->tx_fd = UM_VEC2_NO_FD;
	kfree(taphost);
}

static void um_vec2_tap_clear_channels(struct um_vec2_dev *vdev)
{
	vdev->channels = NULL;
	vdev->num_channels = 0;
}

static int um_vec2_tap_prepare_open(struct um_vec2_dev *vdev,
				    unsigned int *queues)
{
	if (vdev->cfg.transport != UM_VEC2_TRANSPORT_TAP)
		return -EINVAL;
	if (!IS_ENABLED(CONFIG_UML_NET_VECTOR_V2_INPROC))
		return -EACCES;
	if (vdev->cfg.mode != UM_VEC2_HOST_AUTO &&
	    vdev->cfg.mode != UM_VEC2_HOST_INPROC)
		return -EOPNOTSUPP;
	if (!vdev->cfg.ifname[0])
		return -EINVAL;
	if (!vdev->netdev)
		return -ENODEV;
	if (vdev->channels)
		return -EBUSY;

	*queues = um_vec2_netdev_queue_count(vdev);
	return 0;
}

static int um_vec2_tap_open_one_channel(struct um_vec2_dev *vdev,
					struct um_vec2_channel *channel,
					unsigned int index,
					unsigned int queues)
{
	int fd;
	int ret;

	fd = um_vec2_tap_create_fd(vdev->cfg.ifname, queues > 1);
	if (fd < 0)
		return fd;

	ret = um_vec2_tap_channel_attach_fd(vdev, channel, index, fd);
	if (ret)
		os_close_file(fd);
	return ret;
}

static int um_vec2_tap_open_channels(struct um_vec2_dev *vdev,
				     struct um_vec2_channel *channels,
				     unsigned int queues,
				     unsigned int *opened)
{
	int ret;

	*opened = 0;
	while (*opened < queues) {
		ret = um_vec2_tap_open_one_channel(vdev, &channels[*opened],
						   *opened, queues);
		if (ret)
			return ret;
		(*opened)++;
	}

	return 0;
}

static void um_vec2_tap_close_channels(struct um_vec2_dev *vdev,
				       struct um_vec2_channel *channels,
				       unsigned int count)
{
	while (count--)
		um_vec2_tap_channel_close(&channels[count], vdev->netdev);
}

/*
 * Attach an already-open TAP-like fd to @vdev.  The fd is consumed only after
 * this helper returns 0; callers remain responsible for closing it on failure.
 */
int um_vec2_tap_attach_fd(struct um_vec2_dev *vdev, int fd)
{
	struct um_vec2_channel *channel;
	struct net_device *dev = vdev->netdev;
	int ret;

	if (vdev->cfg.transport != UM_VEC2_TRANSPORT_TAP)
		return -EINVAL;
	if (fd < 0)
		return -EBADF;
	if (!dev)
		return -ENODEV;
	if (vdev->channels)
		return -EBUSY;

	channel = kzalloc_obj(*channel);
	if (!channel)
		return -ENOMEM;

	ret = um_vec2_tap_channel_attach_fd(vdev, channel, 0, fd);
	if (ret)
		goto out_free_channel;

	vdev->channels = channel;
	vdev->num_channels = 1;
	return 0;

out_free_channel:
	kfree(channel);
	return ret;
}

int um_vec2_tap_open(struct um_vec2_dev *vdev)
{
	struct um_vec2_channel *channels;
	unsigned int queues;
	unsigned int opened;
	int ret;

	ret = um_vec2_tap_prepare_open(vdev, &queues);
	if (ret)
		return ret;

	channels = kcalloc(queues, sizeof(*channels), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	ret = um_vec2_tap_open_channels(vdev, channels, queues, &opened);
	if (ret)
		goto out_close_channels;

	vdev->channels = channels;
	vdev->num_channels = queues;
	return 0;

out_close_channels:
	um_vec2_tap_close_channels(vdev, channels, opened);
	kfree(channels);
	return ret;
}

void um_vec2_tap_close(struct um_vec2_dev *vdev)
{
	struct um_vec2_channel *channels = vdev->channels;

	if (!channels)
		return;

	um_vec2_tap_close_channels(vdev, channels, vdev->num_channels);
	kfree(channels);
	um_vec2_tap_clear_channels(vdev);
}

/*
 * Re-bind this vec2 netdev to a different host TAP interface name,
 * closing the inherited channels and opening fresh ones via TUNSETIFF
 * on the new name.
 *
 * Used when a forked pool member needs its own TAP. The master's TAP
 * fd is shared via CoW post-fork, so all pool members would otherwise
 * contend on the same host interface.
 *
 * The new TAP is created via the standard um_vec2_tap_open() path
 * which opens /dev/net/tun + TUNSETIFF.  Caller must hold a
 * reference to @dev (the netdev is alive throughout the call).
 *
 * Returns 0 on success, -EINVAL for a non-TAP vec2 interface or
 * empty new_ifname, -errno on tap_open failure.  On failure the
 * old channels are already torn down; caller must accept that
 * the netdev is now without a working backend; recovery requires
 * either retrying with a valid name or calling um_vec2_tap_close
 * + a fresh open with the original name.
 */
int um_vec2_tap_reopen_for_pool_member(struct net_device *dev,
				       const char *new_ifname)
{
	struct um_vec2_dev *vdev;

	if (!dev || !new_ifname || !*new_ifname)
		return -EINVAL;

	vdev = um_vec2_dev_from_netdev(dev);
	if (!vdev)
		return -EINVAL;
	if (vdev->cfg.transport != UM_VEC2_TRANSPORT_TAP)
		return -EINVAL;
	if (strnlen(new_ifname, sizeof(vdev->cfg.ifname)) >=
	    sizeof(vdev->cfg.ifname))
		return -ENAMETOOLONG;

	um_vec2_tap_close(vdev);
	strscpy(vdev->cfg.ifname, new_ifname, sizeof(vdev->cfg.ifname));
	return um_vec2_tap_open(vdev);
}
EXPORT_SYMBOL_GPL(um_vec2_tap_reopen_for_pool_member);
