/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Queue ownership helpers for UML vector networking v2.
 *
 * These helpers are intentionally independent of struct net_device and
 * struct sk_buff.  The runtime driver will pass sk_buff pointers as opaque
 * owners, while KUnit and formal models can use small fake tokens.
 */
#ifndef __UM_VECTOR2_QUEUE_H
#define __UM_VECTOR2_QUEUE_H

#include <linux/types.h>

#define UM_VEC2_QUEUE_MAX_DEPTH		4096U

enum um_vec2_tx_desc_state {
	UM_VEC2_TX_DESC_FREE,
	UM_VEC2_TX_DESC_DRIVER,
};

enum um_vec2_rx_slot_state {
	UM_VEC2_RX_SLOT_FREE,
	UM_VEC2_RX_SLOT_PREPARED,
	UM_VEC2_RX_SLOT_FILLED,
};

struct um_vec2_tx_desc {
	void *owner;
	unsigned int len;
	enum um_vec2_tx_desc_state state;
};

struct um_vec2_rx_slot {
	void *owner;
	unsigned int len;
	enum um_vec2_rx_slot_state state;
};

struct um_vec2_tx_ring {
	struct um_vec2_tx_desc *desc;
	unsigned int depth;
	unsigned int head;
	unsigned int tail;
	unsigned int count;
	unsigned int max_count;
	u64 enqueued;
	u64 completed;
	u64 released;
};

struct um_vec2_rx_batch {
	struct um_vec2_rx_slot *slot;
	unsigned int depth;
	unsigned int prepared;
	unsigned int filled;
	u64 prepared_total;
	u64 received_total;
	u64 released_total;
	u64 consumed_total;
};

typedef void (*um_vec2_queue_release_fn)(void *owner, unsigned int len,
					 void *cookie);
typedef void *(*um_vec2_rx_alloc_fn)(unsigned int slot, void *cookie);

int um_vec2_tx_ring_init(struct um_vec2_tx_ring *ring,
			 struct um_vec2_tx_desc *desc, unsigned int depth);
void um_vec2_tx_ring_reset(struct um_vec2_tx_ring *ring,
			   um_vec2_queue_release_fn release, void *cookie);

bool um_vec2_tx_ring_empty(const struct um_vec2_tx_ring *ring);
bool um_vec2_tx_ring_full(const struct um_vec2_tx_ring *ring);
unsigned int um_vec2_tx_ring_space(const struct um_vec2_tx_ring *ring);
unsigned int um_vec2_tx_ring_contig_count(const struct um_vec2_tx_ring *ring);
const struct um_vec2_tx_desc *
um_vec2_tx_ring_peek(const struct um_vec2_tx_ring *ring, unsigned int offset);
int um_vec2_tx_ring_enqueue(struct um_vec2_tx_ring *ring, void *owner,
			    unsigned int len);
int um_vec2_tx_ring_complete(struct um_vec2_tx_ring *ring, unsigned int count,
			     um_vec2_queue_release_fn release, void *cookie);

int um_vec2_rx_batch_init(struct um_vec2_rx_batch *batch,
			  struct um_vec2_rx_slot *slot, unsigned int depth);
void um_vec2_rx_batch_reset(struct um_vec2_rx_batch *batch,
			    um_vec2_queue_release_fn release, void *cookie);

bool um_vec2_rx_batch_idle(const struct um_vec2_rx_batch *batch);
const struct um_vec2_rx_slot *
um_vec2_rx_batch_slot(const struct um_vec2_rx_batch *batch,
		      unsigned int offset);
int um_vec2_rx_batch_prepare(struct um_vec2_rx_batch *batch,
			     unsigned int budget, um_vec2_rx_alloc_fn alloc,
			     um_vec2_queue_release_fn release, void *cookie);
int um_vec2_rx_batch_prepare_next(struct um_vec2_rx_batch *batch,
				  um_vec2_rx_alloc_fn alloc, void *cookie);
int um_vec2_rx_batch_complete(struct um_vec2_rx_batch *batch,
			      unsigned int received, const unsigned int *lens,
			      um_vec2_queue_release_fn release, void *cookie);
int um_vec2_rx_batch_consume(struct um_vec2_rx_batch *batch,
			     unsigned int count,
			     um_vec2_queue_release_fn consume, void *cookie);

#endif /* __UM_VECTOR2_QUEUE_H */
