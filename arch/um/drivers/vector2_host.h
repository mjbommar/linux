/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host operation boundary for UML vector networking v2.
 */
#ifndef __UM_VECTOR2_HOST_H
#define __UM_VECTOR2_HOST_H

#include <linux/types.h>

#include "vector2_queue.h"

struct um_vec2_host;

/**
 * struct um_vec2_host_ops - host data-plane operations.
 * @name: Stable backend name for logs and trace output.
 * @tx_batch: Complete up to @budget descriptors from a TX ring.
 * @rx_batch: Prepare up to @budget RX buffers and complete received slots.
 *
 * Implementations must not allocate long-lived driver state in these calls.
 * Transient errors such as -EAGAIN and -ENOBUFS must preserve descriptor
 * ownership. Fatal fd death returns -ENODEV and leaves cleanup to the channel
 * lifecycle owner.
 */
struct um_vec2_host_ops {
	const char *name;
	int (*tx_batch)(struct um_vec2_host *host,
			struct um_vec2_tx_ring *ring, unsigned int budget,
			um_vec2_queue_release_fn complete, void *cookie);
	int (*rx_batch)(struct um_vec2_host *host,
			struct um_vec2_rx_batch *batch, unsigned int budget,
			um_vec2_rx_alloc_fn alloc,
			um_vec2_queue_release_fn release, void *cookie);
};

struct um_vec2_host {
	const struct um_vec2_host_ops *ops;
};

#endif /* __UM_VECTOR2_HOST_H */
