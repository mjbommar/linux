// SPDX-License-Identifier: GPL-2.0
/*
 * Queue ownership helpers for UML vector networking v2.
 */

#include <linux/errno.h>
#include <linux/string.h>

#include "vector2_queue.h"

static void um_vec2_tx_desc_clear(struct um_vec2_tx_desc *desc)
{
	desc->owner = NULL;
	desc->len = 0;
	desc->state = UM_VEC2_TX_DESC_FREE;
}

static void um_vec2_rx_slot_clear(struct um_vec2_rx_slot *slot)
{
	slot->owner = NULL;
	slot->len = 0;
	slot->state = UM_VEC2_RX_SLOT_FREE;
}

static void um_vec2_release_slot(void *owner, unsigned int len,
				 um_vec2_queue_release_fn release,
				 void *cookie)
{
	if (release)
		release(owner, len, cookie);
}

int um_vec2_tx_ring_init(struct um_vec2_tx_ring *ring,
			 struct um_vec2_tx_desc *desc, unsigned int depth)
{
	unsigned int i;

	if (!ring || !desc || !depth || depth > UM_VEC2_QUEUE_MAX_DEPTH)
		return -EINVAL;

	memset(ring, 0, sizeof(*ring));
	ring->desc = desc;
	ring->depth = depth;

	for (i = 0; i < depth; i++)
		um_vec2_tx_desc_clear(&ring->desc[i]);

	return 0;
}

bool um_vec2_tx_ring_empty(const struct um_vec2_tx_ring *ring)
{
	return ring->count == 0;
}

bool um_vec2_tx_ring_full(const struct um_vec2_tx_ring *ring)
{
	return ring->count == ring->depth;
}

unsigned int um_vec2_tx_ring_space(const struct um_vec2_tx_ring *ring)
{
	return ring->depth - ring->count;
}

unsigned int um_vec2_tx_ring_contig_count(const struct um_vec2_tx_ring *ring)
{
	if (!ring->count)
		return 0;
	if (ring->tail > ring->head)
		return ring->tail - ring->head;
	return ring->depth - ring->head;
}

const struct um_vec2_tx_desc *
um_vec2_tx_ring_peek(const struct um_vec2_tx_ring *ring, unsigned int offset)
{
	unsigned int index;

	if (offset >= ring->count)
		return NULL;

	index = (ring->head + offset) % ring->depth;
	return &ring->desc[index];
}

int um_vec2_tx_ring_enqueue(struct um_vec2_tx_ring *ring, void *owner,
			    unsigned int len)
{
	struct um_vec2_tx_desc *desc;

	if (!owner || !len)
		return -EINVAL;
	if (um_vec2_tx_ring_full(ring))
		return -ENOSPC;

	desc = &ring->desc[ring->tail];
	if (desc->state != UM_VEC2_TX_DESC_FREE)
		return -EIO;

	desc->owner = owner;
	desc->len = len;
	desc->state = UM_VEC2_TX_DESC_DRIVER;

	ring->tail = (ring->tail + 1) % ring->depth;
	ring->count++;
	ring->enqueued++;
	if (ring->count > ring->max_count)
		ring->max_count = ring->count;

	return 0;
}

int um_vec2_tx_ring_complete(struct um_vec2_tx_ring *ring, unsigned int count,
			     um_vec2_queue_release_fn release, void *cookie)
{
	unsigned int index;
	unsigned int i;

	if (count > ring->count)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		index = (ring->head + i) % ring->depth;
		if (ring->desc[index].state != UM_VEC2_TX_DESC_DRIVER)
			return -EIO;
	}

	for (i = 0; i < count; i++) {
		struct um_vec2_tx_desc *desc = &ring->desc[ring->head];

		um_vec2_release_slot(desc->owner, desc->len, release, cookie);
		um_vec2_tx_desc_clear(desc);
		ring->head = (ring->head + 1) % ring->depth;
		ring->count--;
		ring->completed++;
	}

	return 0;
}

void um_vec2_tx_ring_reset(struct um_vec2_tx_ring *ring,
			   um_vec2_queue_release_fn release, void *cookie)
{
	while (ring->count) {
		struct um_vec2_tx_desc *desc = &ring->desc[ring->head];

		um_vec2_release_slot(desc->owner, desc->len, release, cookie);
		um_vec2_tx_desc_clear(desc);
		ring->head = (ring->head + 1) % ring->depth;
		ring->count--;
		ring->released++;
	}

	ring->head = 0;
	ring->tail = 0;
}

int um_vec2_rx_batch_init(struct um_vec2_rx_batch *batch,
			  struct um_vec2_rx_slot *slot, unsigned int depth)
{
	unsigned int i;

	if (!batch || !slot || !depth || depth > UM_VEC2_QUEUE_MAX_DEPTH)
		return -EINVAL;

	memset(batch, 0, sizeof(*batch));
	batch->slot = slot;
	batch->depth = depth;

	for (i = 0; i < depth; i++)
		um_vec2_rx_slot_clear(&batch->slot[i]);

	return 0;
}

bool um_vec2_rx_batch_idle(const struct um_vec2_rx_batch *batch)
{
	return batch->prepared == 0 && batch->filled == 0;
}

const struct um_vec2_rx_slot *
um_vec2_rx_batch_slot(const struct um_vec2_rx_batch *batch,
		      unsigned int offset)
{
	if (offset >= batch->filled)
		return NULL;

	return &batch->slot[offset];
}

int um_vec2_rx_batch_prepare(struct um_vec2_rx_batch *batch,
			     unsigned int budget, um_vec2_rx_alloc_fn alloc,
			     um_vec2_queue_release_fn release, void *cookie)
{
	unsigned int i;

	if (!alloc || budget > batch->depth)
		return -EINVAL;
	if (!um_vec2_rx_batch_idle(batch))
		return -EBUSY;

	for (i = 0; i < budget; i++) {
		void *owner = alloc(i, cookie);

		if (!owner) {
			um_vec2_rx_batch_reset(batch, release, cookie);
			return -ENOMEM;
		}

		batch->slot[i].owner = owner;
		batch->slot[i].len = 0;
		batch->slot[i].state = UM_VEC2_RX_SLOT_PREPARED;
		batch->prepared++;
		batch->prepared_total++;
	}

	return 0;
}

int um_vec2_rx_batch_complete(struct um_vec2_rx_batch *batch,
			      unsigned int received, const unsigned int *lens,
			      um_vec2_queue_release_fn release, void *cookie)
{
	unsigned int i;

	if (received > batch->prepared)
		return -EINVAL;

	for (i = 0; i < batch->prepared; i++)
		if (batch->slot[i].state != UM_VEC2_RX_SLOT_PREPARED)
			return -EIO;

	for (i = 0; i < received; i++) {
		struct um_vec2_rx_slot *slot = &batch->slot[i];

		slot->len = lens ? lens[i] : 0;
		slot->state = UM_VEC2_RX_SLOT_FILLED;
	}

	for (i = received; i < batch->prepared; i++) {
		struct um_vec2_rx_slot *slot = &batch->slot[i];

		um_vec2_release_slot(slot->owner, slot->len, release, cookie);
		um_vec2_rx_slot_clear(slot);
		batch->released_total++;
	}

	batch->received_total += received;
	batch->filled = received;
	batch->prepared = 0;

	return 0;
}

int um_vec2_rx_batch_consume(struct um_vec2_rx_batch *batch,
			     unsigned int count,
			     um_vec2_queue_release_fn consume, void *cookie)
{
	unsigned int remaining;
	unsigned int i;

	if (count > batch->filled)
		return -EINVAL;

	for (i = 0; i < count; i++)
		if (batch->slot[i].state != UM_VEC2_RX_SLOT_FILLED)
			return -EIO;

	for (i = 0; i < count; i++) {
		struct um_vec2_rx_slot *slot = &batch->slot[i];

		um_vec2_release_slot(slot->owner, slot->len, consume, cookie);
		um_vec2_rx_slot_clear(slot);
		batch->consumed_total++;
	}

	if (!count)
		return 0;

	remaining = batch->filled - count;
	for (i = 0; i < remaining; i++) {
		batch->slot[i] = batch->slot[count + i];
		um_vec2_rx_slot_clear(&batch->slot[count + i]);
	}

	batch->filled = remaining;
	return 0;
}

void um_vec2_rx_batch_reset(struct um_vec2_rx_batch *batch,
			    um_vec2_queue_release_fn release, void *cookie)
{
	unsigned int i;

	for (i = 0; i < batch->depth; i++) {
		struct um_vec2_rx_slot *slot = &batch->slot[i];

		if (slot->state == UM_VEC2_RX_SLOT_FREE)
			continue;

		um_vec2_release_slot(slot->owner, slot->len, release, cookie);
		um_vec2_rx_slot_clear(slot);
		batch->released_total++;
	}

	batch->prepared = 0;
	batch->filled = 0;
}
