// SPDX-License-Identifier: GPL-2.0
/*
 * Trusted TAP host backend for UML vector networking v2.
 *
 * This R4 backend only owns TAP fd open/close and channel lifecycle wiring.
 * Packet movement, interrupts, NAPI, and carrier enablement are left to the
 * later datapath phases.
 */

#define pr_fmt(fmt) "uml-vector2-tap: " fmt

#include <linux/container_of.h>
#include <linux/errno.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/slab.h>

#include <os.h>

#include "vector2_internal.h"

#define UM_VEC2_TUN_PATH	"/dev/net/tun"

struct um_vec2_tap_host {
	struct um_vec2_host host;
	int fd;
};

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
	return -EOPNOTSUPP;
}

static int um_vec2_tap_rx_batch(struct um_vec2_host *host,
				struct um_vec2_rx_batch *batch,
				unsigned int budget, um_vec2_rx_alloc_fn alloc,
				um_vec2_queue_release_fn release, void *cookie)
{
	return -EOPNOTSUPP;
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

static int um_vec2_tap_create_fd(const char *ifname)
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
	strscpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

	ret = os_ioctl_generic(fd, TUNSETIFF, (unsigned long)&ifr);
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

/*
 * Attach an already-open TAP-like fd to @vdev.  The fd is consumed only after
 * this helper returns 0; callers remain responsible for closing it on failure.
 */
int um_vec2_tap_attach_fd(struct um_vec2_dev *vdev, int fd)
{
	struct um_vec2_tap_host *taphost;
	struct um_vec2_channel *channel;
	int ret;

	if (vdev->cfg.transport != UM_VEC2_TRANSPORT_TAP)
		return -EINVAL;
	if (fd < 0)
		return -EBADF;
	if (vdev->channels)
		return -EBUSY;

	channel = kzalloc_obj(*channel);
	if (!channel)
		return -ENOMEM;

	taphost = kzalloc_obj(*taphost);
	if (!taphost) {
		ret = -ENOMEM;
		goto out_free_channel;
	}

	um_vec2_chan_lifecycle_init(&channel->life);
	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_ALLOCATED);
	if (ret)
		goto out_free_host;

	taphost->host.ops = &um_vec2_tap_host_ops;
	taphost->fd = fd;
	channel->host = &taphost->host;

	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_FD_ATTACHED);
	if (ret)
		goto out_free_host;

	vdev->channels = channel;
	vdev->num_channels = 1;
	return 0;

out_free_host:
	kfree(taphost);
out_free_channel:
	kfree(channel);
	return ret;
}

int um_vec2_tap_open(struct um_vec2_dev *vdev)
{
	int fd;
	int ret;

	if (vdev->cfg.transport != UM_VEC2_TRANSPORT_TAP)
		return -EINVAL;
	if (!IS_ENABLED(CONFIG_UML_NET_VECTOR_V2_INPROC))
		return -EACCES;
	if (vdev->cfg.mode != UM_VEC2_HOST_AUTO &&
	    vdev->cfg.mode != UM_VEC2_HOST_INPROC)
		return -EOPNOTSUPP;
	if (!vdev->cfg.ifname[0])
		return -EINVAL;

	fd = um_vec2_tap_create_fd(vdev->cfg.ifname);
	if (fd < 0)
		return fd;

	ret = um_vec2_tap_attach_fd(vdev, fd);
	if (ret)
		os_close_file(fd);
	return ret;
}

void um_vec2_tap_close(struct um_vec2_dev *vdev)
{
	struct um_vec2_channel *channel = vdev->channels;
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

	kfree(taphost);
	kfree(channel);
	vdev->channels = NULL;
	vdev->num_channels = 0;
}
