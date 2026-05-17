// SPDX-License-Identifier: GPL-2.0
/*
 * Deterministic fake host backend for UML vector networking v2 tests.
 */

#include <linux/errno.h>
#include <linux/container_of.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include "vector2_fake_host.h"

static struct um_vec2_fake_host *
um_vec2_host_to_fake(struct um_vec2_host *host)
{
	return container_of(host, struct um_vec2_fake_host, host);
}

static int um_vec2_fake_tx_batch(struct um_vec2_host *host,
				 struct um_vec2_tx_ring *ring,
				 unsigned int budget,
				 um_vec2_queue_release_fn complete,
				 void *cookie)
{
	struct um_vec2_fake_host *fake = um_vec2_host_to_fake(host);
	unsigned int limit;
	unsigned int count;
	int err;

	fake->stats.tx_calls++;

	if (fake->dead) {
		fake->stats.fd_deaths++;
		return -ENODEV;
	}

	err = fake->tx_error;
	if (err) {
		fake->stats.tx_errors++;
		return err;
	}

	limit = min(budget, ring->count);
	limit = min(limit, fake->tx_limit);
	count = limit;
	if (!count)
		return 0;

	err = um_vec2_tx_ring_complete(ring, count, complete, cookie);
	if (err) {
		fake->stats.tx_errors++;
		return err;
	}

	fake->stats.tx_packets += count;
	return count;
}

static unsigned int um_vec2_fake_rx_pop(struct um_vec2_fake_host *fake,
					unsigned int *lens,
					unsigned int limit)
{
	unsigned int count = min(fake->rx_count, limit);
	unsigned int i;

	for (i = 0; i < count; i++) {
		lens[i] = fake->rx_len[fake->rx_head];
		fake->rx_head = (fake->rx_head + 1) % UM_VEC2_FAKE_HOST_MAX_RX;
	}

	fake->rx_count -= count;
	if (!fake->rx_count)
		fake->rx_head = 0;

	return count;
}

static int um_vec2_fake_rx_batch(struct um_vec2_host *host,
				 struct um_vec2_rx_batch *batch,
				 unsigned int budget, um_vec2_rx_alloc_fn alloc,
				 um_vec2_queue_release_fn release,
				 void *cookie)
{
	struct um_vec2_fake_host *fake = um_vec2_host_to_fake(host);
	unsigned int lens[UM_VEC2_FAKE_HOST_MAX_RX];
	unsigned int limit;
	unsigned int count;
	int err;

	fake->stats.rx_calls++;

	if (fake->dead) {
		fake->stats.fd_deaths++;
		return -ENODEV;
	}

	err = fake->rx_error;
	if (err) {
		fake->stats.rx_errors++;
		return err;
	}

	if (budget > UM_VEC2_FAKE_HOST_MAX_RX)
		return -EINVAL;

	err = um_vec2_rx_batch_prepare(batch, budget, alloc, release, cookie);
	if (err) {
		fake->stats.rx_errors++;
		return err;
	}

	limit = min(budget, fake->rx_limit);
	count = um_vec2_fake_rx_pop(fake, lens, limit);
	if (!count)
		fake->stats.rx_empty++;

	err = um_vec2_rx_batch_complete(batch, count, lens, release, cookie);
	if (err) {
		fake->stats.rx_errors++;
		return err;
	}

	fake->stats.rx_packets += count;
	return count;
}

static const struct um_vec2_host_ops um_vec2_fake_host_ops = {
	.name = "fake",
	.tx_batch = um_vec2_fake_tx_batch,
	.rx_batch = um_vec2_fake_rx_batch,
};

void um_vec2_fake_host_init(struct um_vec2_fake_host *fake)
{
	memset(fake, 0, sizeof(*fake));
	fake->host.ops = &um_vec2_fake_host_ops;
	fake->tx_limit = UINT_MAX;
	fake->rx_limit = UINT_MAX;
}

struct um_vec2_host *um_vec2_fake_host_base(struct um_vec2_fake_host *fake)
{
	return &fake->host;
}

void um_vec2_fake_host_set_tx_limit(struct um_vec2_fake_host *fake,
				    unsigned int limit)
{
	fake->tx_limit = limit;
}

void um_vec2_fake_host_set_rx_limit(struct um_vec2_fake_host *fake,
				    unsigned int limit)
{
	fake->rx_limit = limit;
}

void um_vec2_fake_host_set_tx_error(struct um_vec2_fake_host *fake, int error)
{
	fake->tx_error = error;
}

void um_vec2_fake_host_set_rx_error(struct um_vec2_fake_host *fake, int error)
{
	fake->rx_error = error;
}

void um_vec2_fake_host_kill(struct um_vec2_fake_host *fake)
{
	fake->dead = true;
}

int um_vec2_fake_host_push_rx(struct um_vec2_fake_host *fake, unsigned int len)
{
	unsigned int index;

	if (!len)
		return -EINVAL;
	if (fake->rx_count == UM_VEC2_FAKE_HOST_MAX_RX)
		return -ENOSPC;

	index = (fake->rx_head + fake->rx_count) % UM_VEC2_FAKE_HOST_MAX_RX;
	fake->rx_len[index] = len;
	fake->rx_count++;

	return 0;
}
