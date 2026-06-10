// SPDX-License-Identifier: GPL-2.0
/*
 * Trusted direct-fd host backend for UML vector networking v2.
 */

#define pr_fmt(fmt) "uml-vector2-fd: " fmt

#include <linux/container_of.h>
#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/if_tun.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/virtio_net.h>
#include <uapi/linux/if.h>

#include <os.h>

#include "vector2_internal.h"

#if IS_ENABLED(CONFIG_UML_NET_VECTOR_V2_HOST_FD_KUNIT)
int um_vec2_fd_fault_index = -1;
EXPORT_SYMBOL_GPL(um_vec2_fd_fault_index);
#endif

struct um_vec2_fd_host {
	struct um_vec2_host host;
	struct net_device *dev;
	unsigned int frame_len;
	int rx_fd;
	int tx_fd;
	bool vnet_hdr;	/* inherited fd has IFF_VNET_HDR set */
};

static struct um_vec2_fd_host *um_vec2_host_to_fd(struct um_vec2_host *host)
{
	return container_of(host, struct um_vec2_fd_host, host);
}

/*
 * Send one skb to the host TAP fd.  When @vnet_hdr is true the
 * tap was opened with IFF_VNET_HDR; prepend a virtio_net_hdr
 * so the host kernel can offload large TCP segments (TSO) instead
 * of forcing the guest to MTU-segment.  Mirrors vector2_host_tap.c.
 */
static int um_vec2_fd_write_skb(int fd, struct sk_buff *skb, bool vnet_hdr)
{
	if (vnet_hdr) {
		struct virtio_net_hdr hdr;
		unsigned int original_len = skb->len;
		int ret;

		ret = skb_cow_head(skb, sizeof(hdr));
		if (ret)
			return ret;
		ret = virtio_net_hdr_from_skb(skb, &hdr, true, false, 0);
		if (ret)
			return ret;
		skb_push(skb, sizeof(hdr));
		skb_copy_to_linear_data(skb, &hdr, sizeof(hdr));
		ret = os_write_file(fd, skb->data, skb->len);
		skb_pull(skb, sizeof(hdr));
		if (ret == -EAGAIN || ret == -ENOBUFS)
			return ret;
		if (ret < 0)
			return ret;
		if (ret != original_len + (int)sizeof(hdr))
			return -EIO;
		return original_len;
	}

	return os_write_file(fd, skb->data, skb->len);
}

static int um_vec2_fd_tx_batch(struct um_vec2_host *host,
			       struct um_vec2_tx_ring *ring,
			       unsigned int budget,
			       um_vec2_queue_release_fn complete,
			       void *cookie)
{
	struct um_vec2_fd_host *fdhost = um_vec2_host_to_fd(host);
	unsigned int sent = 0;
	int ret = 0;

	while (sent < budget && !um_vec2_tx_ring_empty(ring)) {
		const struct um_vec2_tx_desc *desc;
		struct sk_buff *skb;

		desc = um_vec2_tx_ring_peek(ring, 0);
		if (!desc)
			break;

		skb = desc->owner;
		ret = skb_linearize(skb);
		if (ret)
			return sent ? (int)sent : ret;

		ret = um_vec2_fd_write_skb(fdhost->tx_fd, skb, fdhost->vnet_hdr);
		if (ret == -EAGAIN || ret == -ENOBUFS)
			break;
		if (ret < 0)
			return sent ? (int)sent : ret;
		if (ret != skb->len)
			return sent ? (int)sent : -EIO;

		ret = um_vec2_tx_ring_complete(ring, 1, complete, cookie);
		if (ret)
			return sent ? (int)sent : ret;
		sent++;
	}

	return sent;
}

static int um_vec2_fd_parse_vnet_skb(struct sk_buff *skb, int len)
{
	struct virtio_net_hdr hdr;

	if (len <= (int)sizeof(hdr))
		return -EPROTO;

	skb_trim(skb, len);
	memcpy(&hdr, skb->data, sizeof(hdr));
	skb_pull(skb, sizeof(hdr));
	if (virtio_net_hdr_to_skb(skb, &hdr, true))
		return -EPROTO;

	return skb->len;
}

static int um_vec2_fd_parse_raw_skb(struct sk_buff *skb, int len)
{
	if (len < ETH_HLEN)
		return -EPROTO;

	skb_trim(skb, len);
	return skb->len;
}

static int um_vec2_fd_read_skb(struct um_vec2_fd_host *fdhost,
			       struct sk_buff *skb)
{
	unsigned char *data;
	int ret;

	data = skb_put(skb, fdhost->frame_len);
	ret = os_read_file(fdhost->rx_fd, data, fdhost->frame_len);
	if (ret < 0)
		return ret;

	if (fdhost->vnet_hdr)
		ret = um_vec2_fd_parse_vnet_skb(skb, ret);
	else
		ret = um_vec2_fd_parse_raw_skb(skb, ret);
	if (ret < 0)
		return ret;

	skb->dev = fdhost->dev;
	return ret;
}

static int um_vec2_fd_rx_batch(struct um_vec2_host *host,
			       struct um_vec2_rx_batch *batch,
			       unsigned int budget, um_vec2_rx_alloc_fn alloc,
			       um_vec2_queue_release_fn release, void *cookie)
{
	struct um_vec2_fd_host *fdhost = um_vec2_host_to_fd(host);
	unsigned int lens[64];
	unsigned int received = 0;
	unsigned int i;
	int ret;

	if (!budget)
		return 0;
	if (budget > batch->depth)
		return -EINVAL;
	if (budget > ARRAY_SIZE(lens))
		budget = ARRAY_SIZE(lens);

	ret = um_vec2_rx_batch_prepare(batch, budget, alloc, release, cookie);
	if (ret)
		return ret;

	for (i = 0; i < budget; i++) {
		struct sk_buff *skb = batch->slot[i].owner;

		ret = um_vec2_fd_read_skb(fdhost, skb);
		if (ret == -EAGAIN)
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

static const struct um_vec2_host_ops um_vec2_fd_host_ops = {
	.name		= "fd",
	.tx_batch	= um_vec2_fd_tx_batch,
	.rx_batch	= um_vec2_fd_rx_batch,
};

static const char *um_vec2_fd_mode_name(unsigned int mode)
{
	switch (mode & S_IFMT) {
	case S_IFCHR:
		return "char";
	case S_IFIFO:
		return "fifo";
	case S_IFSOCK:
		return "socket";
	case S_IFREG:
		return "regular";
	case S_IFDIR:
		return "directory";
	case S_IFBLK:
		return "block";
	case S_IFLNK:
		return "symlink";
	default:
		return "unknown";
	}
}

static int um_vec2_fd_validate_source(struct um_vec2_dev *vdev,
				      unsigned int source_fd)
{
	struct uml_stat st;
	int ret;

	ret = os_stat_fd(source_fd, &st);
	if (ret) {
		pr_err("vec2.%u inherited fd %u is not usable: fstat failed ret=%d\n",
		       vdev->unit, source_fd, ret);
		return ret;
	}

	if (S_ISCHR(st.ust_mode) || S_ISFIFO(st.ust_mode) ||
	    S_ISSOCK(st.ust_mode))
		return 0;

	pr_err("vec2.%u inherited fd %u has unsupported type %s mode=0%o; "
	       "expected char, fifo, or socket\n",
	       vdev->unit, source_fd, um_vec2_fd_mode_name(st.ust_mode),
	       st.ust_mode);
	return -EINVAL;
}

static void um_vec2_fd_host_close(struct um_vec2_fd_host *fdhost)
{
	if (!fdhost)
		return;

	if (fdhost->rx_fd >= 0)
		os_close_file(fdhost->rx_fd);
	if (fdhost->tx_fd >= 0 && fdhost->tx_fd != fdhost->rx_fd)
		os_close_file(fdhost->tx_fd);

	fdhost->rx_fd = -1;
	fdhost->tx_fd = -1;
}

static void um_vec2_fd_channel_init(struct um_vec2_dev *vdev,
				    struct um_vec2_channel *channel,
				    unsigned int index)
{
	um_vec2_chan_lifecycle_init(&channel->life);
	channel->vdev = vdev;
	channel->index = index;
	channel->rx_fd = UM_VEC2_NO_FD;
	channel->tx_fd = UM_VEC2_NO_FD;
	channel->rx_irq = UM_VEC2_NO_IRQ;
	channel->tx_irq = UM_VEC2_NO_IRQ;
}

static int um_vec2_fd_dup_source(unsigned int source_fd)
{
	int fd;
	int ret;

	fd = os_dup_file(source_fd);
	if (fd < 0)
		return fd;

	ret = os_set_fd_block(fd, 0);
	if (ret) {
		os_close_file(fd);
		return ret;
	}

	return fd;
}

static bool um_vec2_fd_has_vnet_hdr(int fd)
{
	struct ifreq ifr = {};
	int ret;

	/*
	 * Probe the inherited tap fd for IFF_VNET_HDR. When set, the tap was
	 * opened with virtio_net_hdr-prefixed framing. Non-TAP fds and probe
	 * failures use raw Ethernet frames.
	 */
	ret = os_ioctl_generic(fd, TUNGETIFF, (unsigned long)&ifr);
	return ret == 0 && (ifr.ifr_flags & IFF_VNET_HDR) != 0;
}

static void um_vec2_fd_attach_channel(struct um_vec2_fd_host *fdhost,
				      struct um_vec2_channel *channel,
				      struct net_device *dev, int fd)
{
	fdhost->host.ops = &um_vec2_fd_host_ops;
	fdhost->dev = dev;
	fdhost->vnet_hdr = um_vec2_fd_has_vnet_hdr(fd);
	fdhost->frame_len = um_vec2_runtime_frame_len(dev, fdhost->vnet_hdr);
	fdhost->rx_fd = fd;
	fdhost->tx_fd = fd;

	channel->host = &fdhost->host;
	channel->rx_fd = fd;
	channel->tx_fd = fd;
}

static void um_vec2_fd_detach_channel(struct um_vec2_fd_host *fdhost,
				      struct um_vec2_channel *channel)
{
	um_vec2_fd_host_close(fdhost);
	channel->host = NULL;
	channel->rx_fd = UM_VEC2_NO_FD;
	channel->tx_fd = UM_VEC2_NO_FD;
}

static void um_vec2_fd_channel_mark_closed(struct um_vec2_channel *channel)
{
	if (um_vec2_chan_can_transition(channel->life.state,
					UM_VEC2_CHAN_QUIESCING))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_QUIESCING);
	if (um_vec2_chan_can_transition(channel->life.state, UM_VEC2_CHAN_CLOSED))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_CLOSED);
}

static struct um_vec2_fd_host *um_vec2_fd_host_alloc(void)
{
	struct um_vec2_fd_host *fdhost;

	fdhost = kzalloc_obj(*fdhost);
	if (!fdhost)
		return NULL;

	fdhost->rx_fd = UM_VEC2_NO_FD;
	fdhost->tx_fd = UM_VEC2_NO_FD;
	return fdhost;
}

static int um_vec2_fd_channel_prepare(struct um_vec2_dev *vdev,
				      struct um_vec2_channel *channel,
				      unsigned int index)
{
	int ret;

	um_vec2_fd_channel_init(vdev, channel, index);
	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_ALLOCATED);
	if (ret)
		return ret;

	return um_vec2_queue_pair_alloc(channel, vdev->cfg.depth);
}

static int um_vec2_fd_channel_open(struct um_vec2_dev *vdev,
				   struct um_vec2_channel *channel,
				   unsigned int index,
				   unsigned int source_fd)
{
	struct um_vec2_fd_host *fdhost;
	struct net_device *dev = vdev->netdev;
	int fd;
	int ret;

#if IS_ENABLED(CONFIG_UML_NET_VECTOR_V2_HOST_FD_KUNIT)
	if (um_vec2_fd_fault_index >= 0 &&
		    (unsigned int)um_vec2_fd_fault_index == index)
		return -EIO;
	#endif

	fdhost = um_vec2_fd_host_alloc();
	if (!fdhost)
		return -ENOMEM;

	ret = um_vec2_fd_channel_prepare(vdev, channel, index);
	if (ret)
		goto out_free_host;

	fd = um_vec2_fd_dup_source(source_fd);
	if (fd < 0) {
		ret = fd;
		goto out_free_queue;
	}

	um_vec2_fd_attach_channel(fdhost, channel, dev, fd);
	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_FD_ATTACHED);
	if (ret)
		goto out_close_fd;

	return 0;

out_close_fd:
	um_vec2_fd_detach_channel(fdhost, channel);
out_free_queue:
	um_vec2_queue_pair_free(channel, dev);
	um_vec2_fd_channel_mark_closed(channel);
out_free_host:
	kfree(fdhost);
	return ret;
}

static void um_vec2_fd_channel_close(struct um_vec2_channel *channel,
				     struct net_device *dev)
{
	struct um_vec2_fd_host *fdhost;

	if (!channel)
		return;

	fdhost = channel->host ? um_vec2_host_to_fd(channel->host) : NULL;
	if (um_vec2_chan_can_transition(channel->life.state,
					UM_VEC2_CHAN_QUIESCING))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_QUIESCING);
	um_vec2_fd_detach_channel(fdhost, channel);
	if (um_vec2_chan_can_transition(channel->life.state, UM_VEC2_CHAN_CLOSED))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_CLOSED);

	um_vec2_queue_pair_free(channel, dev);
	kfree(fdhost);
}

static void um_vec2_fd_clear_channels(struct um_vec2_dev *vdev)
{
	vdev->channels = NULL;
	vdev->num_channels = 0;
}

static int um_vec2_fd_validate_range(struct um_vec2_dev *vdev,
				     unsigned int queues)
{
	unsigned int i;
	int ret;

	if (vdev->cfg.fd > INT_MAX - (queues - 1)) {
		pr_err("vec2.%u inherited fd range fd=%u queues=%u exceeds INT_MAX\n",
		       vdev->unit, vdev->cfg.fd, queues);
		return -EINVAL;
	}

	for (i = 0; i < queues; i++) {
		ret = um_vec2_fd_validate_source(vdev, vdev->cfg.fd + i);
		if (ret)
			return ret;
	}

	return 0;
}

static int um_vec2_fd_prepare_open(struct um_vec2_dev *vdev,
				   unsigned int *queues)
{
	if (vdev->cfg.transport != UM_VEC2_TRANSPORT_FD || !vdev->cfg.has_fd)
		return -EINVAL;
	if (!vdev->netdev)
		return -ENODEV;
	if (vdev->channels)
		return -EBUSY;

	*queues = um_vec2_netdev_queue_count(vdev);
	return um_vec2_fd_validate_range(vdev, *queues);
}

static int um_vec2_fd_open_channels(struct um_vec2_dev *vdev,
				    struct um_vec2_channel *channels,
				    unsigned int queues,
				    unsigned int *opened)
{
	int ret;

	*opened = 0;
	while (*opened < queues) {
		ret = um_vec2_fd_channel_open(vdev, &channels[*opened], *opened,
					      vdev->cfg.fd + *opened);
		if (ret)
			return ret;
		(*opened)++;
	}

	return 0;
}

static void um_vec2_fd_close_channels(struct um_vec2_dev *vdev,
				      struct um_vec2_channel *channels,
				      unsigned int count)
{
	while (count--)
		um_vec2_fd_channel_close(&channels[count], vdev->netdev);
}

int um_vec2_fd_open(struct um_vec2_dev *vdev)
{
	struct um_vec2_channel *channels;
	unsigned int queues;
	unsigned int opened;
	int ret;

	ret = um_vec2_fd_prepare_open(vdev, &queues);
	if (ret)
		return ret;

	channels = kcalloc(queues, sizeof(*channels), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	ret = um_vec2_fd_open_channels(vdev, channels, queues, &opened);
	if (ret)
		goto out_close_channels;

	vdev->channels = channels;
	vdev->num_channels = queues;
	return 0;

out_close_channels:
	um_vec2_fd_close_channels(vdev, channels, opened);
	kfree(channels);
	um_vec2_fd_clear_channels(vdev);
	return ret;
}

void um_vec2_fd_close(struct um_vec2_dev *vdev)
{
	struct um_vec2_channel *channels = vdev->channels;
	unsigned int i;

	if (!channels)
		return;

	for (i = 0; i < vdev->num_channels; i++)
		um_vec2_fd_channel_close(&channels[i], vdev->netdev);
	kfree(channels);
	um_vec2_fd_clear_channels(vdev);
}
