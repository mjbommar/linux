// SPDX-License-Identifier: GPL-2.0
/*
 * Trusted direct-fd host backend for UML vector networking v2.
 */

#define pr_fmt(fmt) "uml-vector2-fd: " fmt

#include <linux/container_of.h>
#include <linux/errno.h>
#include <linux/slab.h>

#include <os.h>

#include "vector2_internal.h"

struct um_vec2_fd_host {
	struct um_vec2_host host;
	int rx_fd;
	int tx_fd;
};

static struct um_vec2_fd_host *um_vec2_host_to_fd(struct um_vec2_host *host)
{
	return container_of(host, struct um_vec2_fd_host, host);
}

static int um_vec2_fd_tx_batch(struct um_vec2_host *host,
			       struct um_vec2_tx_ring *ring,
			       unsigned int budget,
			       um_vec2_queue_release_fn complete,
			       void *cookie)
{
	return -EOPNOTSUPP;
}

static int um_vec2_fd_rx_batch(struct um_vec2_host *host,
			       struct um_vec2_rx_batch *batch,
			       unsigned int budget, um_vec2_rx_alloc_fn alloc,
			       um_vec2_queue_release_fn release, void *cookie)
{
	return -EOPNOTSUPP;
}

static const struct um_vec2_host_ops um_vec2_fd_host_ops = {
	.name		= "fd",
	.tx_batch	= um_vec2_fd_tx_batch,
	.rx_batch	= um_vec2_fd_rx_batch,
};

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

int um_vec2_fd_open(struct um_vec2_dev *vdev)
{
	struct um_vec2_fd_host *fdhost;
	struct um_vec2_channel *channel;
	int fd;
	int ret;

	if (vdev->cfg.transport != UM_VEC2_TRANSPORT_FD || !vdev->cfg.has_fd)
		return -EINVAL;
	if (vdev->channels)
		return -EBUSY;

	channel = kzalloc_obj(*channel);
	if (!channel)
		return -ENOMEM;

	fdhost = kzalloc_obj(*fdhost);
	if (!fdhost) {
		ret = -ENOMEM;
		goto out_free_channel;
	}

	um_vec2_chan_lifecycle_init(&channel->life);
	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_ALLOCATED);
	if (ret)
		goto out_free_host;

	fd = os_dup_file(vdev->cfg.fd);
	if (fd < 0) {
		ret = fd;
		goto out_close_channel;
	}

	fdhost->host.ops = &um_vec2_fd_host_ops;
	fdhost->rx_fd = fd;
	fdhost->tx_fd = fd;
	channel->host = &fdhost->host;

	ret = um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_FD_ATTACHED);
	if (ret)
		goto out_close_fd;

	vdev->channels = channel;
	vdev->num_channels = 1;
	return 0;

out_close_fd:
	um_vec2_fd_host_close(fdhost);
out_close_channel:
	if (um_vec2_chan_can_transition(channel->life.state,
					UM_VEC2_CHAN_QUIESCING))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_QUIESCING);
	if (um_vec2_chan_can_transition(channel->life.state, UM_VEC2_CHAN_CLOSED))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_CLOSED);
out_free_host:
	kfree(fdhost);
out_free_channel:
	kfree(channel);
	return ret;
}

void um_vec2_fd_close(struct um_vec2_dev *vdev)
{
	struct um_vec2_channel *channel = vdev->channels;
	struct um_vec2_fd_host *fdhost;

	if (!channel)
		return;

	fdhost = channel->host ? um_vec2_host_to_fd(channel->host) : NULL;
	if (um_vec2_chan_can_transition(channel->life.state,
					UM_VEC2_CHAN_QUIESCING))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_QUIESCING);
	um_vec2_fd_host_close(fdhost);
	if (um_vec2_chan_can_transition(channel->life.state, UM_VEC2_CHAN_CLOSED))
		um_vec2_chan_transition(&channel->life, UM_VEC2_CHAN_CLOSED);

	kfree(fdhost);
	kfree(channel);
	vdev->channels = NULL;
	vdev->num_channels = 0;
}
