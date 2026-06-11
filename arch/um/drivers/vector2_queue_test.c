// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit coverage for UML vector networking v2 queue ownership helpers.
 */

#include <kunit/test.h>
#include <linux/errno.h>

#include "vector2_queue.h"

#define VECTOR2_QUEUE_TRACE_MAX		16

struct vector2_queue_trace {
	unsigned int count;
	unsigned int len_sum;
	unsigned long owner[VECTOR2_QUEUE_TRACE_MAX];
};

struct vector2_rx_alloc_ctx {
	unsigned int fail_at;
	bool fail_enabled;
	struct vector2_queue_trace *trace;
};

static void vector2_queue_trace_release(void *owner, unsigned int len,
					void *cookie)
{
	struct vector2_queue_trace *trace = cookie;

	if (trace->count >= VECTOR2_QUEUE_TRACE_MAX)
		return;

	trace->owner[trace->count] = (unsigned long)owner;
	trace->count++;
	trace->len_sum += len;
}

static void vector2_queue_alloc_ctx_release(void *owner, unsigned int len,
					    void *cookie)
{
	struct vector2_rx_alloc_ctx *ctx = cookie;

	vector2_queue_trace_release(owner, len, ctx->trace);
}

static void *vector2_rx_alloc(unsigned int slot, void *cookie)
{
	struct vector2_rx_alloc_ctx *ctx = cookie;

	if (ctx && ctx->fail_enabled && slot == ctx->fail_at)
		return NULL;

	return (void *)(unsigned long)(slot + 1);
}

static void vector2_queue_tx_wraparound_test(struct kunit *test)
{
	struct um_vec2_tx_desc desc[4];
	struct vector2_queue_trace trace = {};
	struct um_vec2_tx_ring ring;
	const struct um_vec2_tx_desc *peek;

	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_init(&ring, desc, 4), 0);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_empty(&ring));
	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_space(&ring), 4U);

	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)1UL, 10),
			0);
	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)2UL, 20),
			0);
	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)3UL, 30),
			0);
	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_complete(&ring, 2,
						       vector2_queue_trace_release,
						       &trace), 0);
	KUNIT_EXPECT_EQ(test, trace.count, 2U);
	KUNIT_EXPECT_EQ(test, trace.owner[0], 1UL);
	KUNIT_EXPECT_EQ(test, trace.owner[1], 2UL);
	KUNIT_EXPECT_EQ(test, trace.len_sum, 30U);

	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)4UL, 40),
			0);
	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)5UL, 50),
			0);
	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)6UL, 60),
			0);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_full(&ring));
	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_contig_count(&ring), 2U);

	peek = um_vec2_tx_ring_peek(&ring, 0);
	KUNIT_ASSERT_NOT_NULL(test, peek);
	KUNIT_EXPECT_EQ(test, (unsigned long)peek->owner, 3UL);
	peek = um_vec2_tx_ring_peek(&ring, 3);
	KUNIT_ASSERT_NOT_NULL(test, peek);
	KUNIT_EXPECT_EQ(test, (unsigned long)peek->owner, 6UL);
	KUNIT_EXPECT_NULL(test, um_vec2_tx_ring_peek(&ring, 4));
}

static void vector2_queue_tx_partial_completion_test(struct kunit *test)
{
	struct um_vec2_tx_desc desc[4];
	struct vector2_queue_trace trace = {};
	struct um_vec2_tx_ring ring;
	const struct um_vec2_tx_desc *peek;

	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_init(&ring, desc, 4), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)1UL, 10),
			0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)2UL, 20),
			0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)3UL, 30),
			0);

	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_complete(&ring, 1,
						       vector2_queue_trace_release,
						       &trace), 0);
	KUNIT_EXPECT_EQ(test, ring.count, 2U);
	KUNIT_EXPECT_EQ(test, ring.completed, 1ULL);

	peek = um_vec2_tx_ring_peek(&ring, 0);
	KUNIT_ASSERT_NOT_NULL(test, peek);
	KUNIT_EXPECT_EQ(test, (unsigned long)peek->owner, 2UL);
	KUNIT_EXPECT_EQ(test, peek->len, 20U);
}

static void vector2_queue_tx_rejects_invalid_test(struct kunit *test)
{
	struct um_vec2_tx_desc desc[2];
	struct um_vec2_tx_ring ring;

	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_init(&ring, NULL, 2), -EINVAL);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_init(&ring, desc, 2), 0);
	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_enqueue(&ring, NULL, 1),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)1UL, 0),
			-EINVAL);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)1UL, 10),
			0);
	KUNIT_EXPECT_EQ(test, um_vec2_tx_ring_complete(&ring, 2, NULL, NULL),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, ring.count, 1U);
}

static void vector2_queue_tx_reset_releases_test(struct kunit *test)
{
	struct vector2_queue_trace trace = {};
	struct um_vec2_tx_desc desc[3];
	struct um_vec2_tx_ring ring;

	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_init(&ring, desc, 3), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)1UL, 10),
			0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)2UL, 20),
			0);

	um_vec2_tx_ring_reset(&ring, vector2_queue_trace_release, &trace);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_empty(&ring));
	KUNIT_EXPECT_EQ(test, trace.count, 2U);
	KUNIT_EXPECT_EQ(test, trace.len_sum, 30U);
	KUNIT_EXPECT_EQ(test, ring.released, 2ULL);
}

static void vector2_queue_rx_prepare_receive_consume_test(struct kunit *test)
{
	struct vector2_queue_trace release_trace = {};
	struct vector2_queue_trace consume_trace = {};
	struct um_vec2_rx_slot slots[4];
	struct um_vec2_rx_batch batch;
	unsigned int lens[] = { 100, 200 };
	const struct um_vec2_rx_slot *slot;

	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_init(&batch, slots, 4), 0);
	KUNIT_EXPECT_TRUE(test, um_vec2_rx_batch_idle(&batch));

	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_prepare(&batch, 4,
						       vector2_rx_alloc,
						       vector2_queue_trace_release,
						       NULL), 0);
	KUNIT_EXPECT_EQ(test, batch.prepared, 4U);
	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_complete(&batch, 2, lens,
							vector2_queue_trace_release,
							&release_trace), 0);
	KUNIT_EXPECT_EQ(test, release_trace.count, 2U);
	KUNIT_EXPECT_EQ(test, batch.filled, 2U);

	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_consume(&batch, 1,
						       vector2_queue_trace_release,
						       &consume_trace), 0);
	KUNIT_EXPECT_EQ(test, consume_trace.count, 1U);
	KUNIT_EXPECT_EQ(test, consume_trace.owner[0], 1UL);
	KUNIT_EXPECT_EQ(test, consume_trace.len_sum, 100U);
	slot = um_vec2_rx_batch_slot(&batch, 0);
	KUNIT_ASSERT_NOT_NULL(test, slot);
	KUNIT_EXPECT_EQ(test, (unsigned long)slot->owner, 2UL);
	KUNIT_EXPECT_EQ(test, slot->len, 200U);

	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_consume(&batch, 1,
						       vector2_queue_trace_release,
						       &consume_trace), 0);
	KUNIT_EXPECT_TRUE(test, um_vec2_rx_batch_idle(&batch));
}

static void vector2_queue_rx_alloc_failure_unwinds_test(struct kunit *test)
{
	struct vector2_queue_trace trace = {};
	struct vector2_rx_alloc_ctx alloc = {
		.fail_at = 2,
		.fail_enabled = true,
		.trace = &trace,
	};
	struct um_vec2_rx_slot slots[4];
	struct um_vec2_rx_batch batch;

	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_init(&batch, slots, 4), 0);
	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_prepare(&batch, 4,
						       vector2_rx_alloc,
						       vector2_queue_alloc_ctx_release,
						       &alloc), -ENOMEM);
	KUNIT_EXPECT_TRUE(test, um_vec2_rx_batch_idle(&batch));
	KUNIT_EXPECT_EQ(test, trace.count, 2U);
	KUNIT_EXPECT_EQ(test, batch.released_total, 2ULL);
}

static void vector2_queue_rx_prepare_next_test(struct kunit *test)
{
	struct vector2_queue_trace release_trace = {};
	struct vector2_rx_alloc_ctx alloc = {
		.fail_at = 2,
		.fail_enabled = true,
	};
	struct um_vec2_rx_slot slots[4];
	struct um_vec2_rx_batch batch;
	unsigned int lens[] = { 64 };

	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_init(&batch, slots, 4), 0);
	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_prepare_next(&batch,
							    vector2_rx_alloc,
							    &alloc), 0);
	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_prepare_next(&batch,
							    vector2_rx_alloc,
							    &alloc), 0);
	KUNIT_EXPECT_EQ(test, batch.prepared, 2U);
	KUNIT_EXPECT_EQ(test, batch.prepared_total, 2ULL);
	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_prepare_next(&batch,
							    vector2_rx_alloc,
							    &alloc), -ENOMEM);
	KUNIT_EXPECT_EQ(test, batch.prepared, 2U);

	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_complete(&batch, 1, lens,
							vector2_queue_trace_release,
							&release_trace), 0);
	KUNIT_EXPECT_EQ(test, release_trace.count, 1U);
	KUNIT_EXPECT_EQ(test, release_trace.owner[0], 2UL);
	KUNIT_EXPECT_EQ(test, batch.filled, 1U);
	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_prepare_next(&batch,
							    vector2_rx_alloc,
							    NULL), -EBUSY);
}

static void vector2_queue_rx_rejects_busy_and_invalid_test(struct kunit *test)
{
	struct vector2_queue_trace trace = {};
	struct um_vec2_rx_slot slots[2];
	struct um_vec2_rx_batch batch;

	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_init(&batch, slots, 2), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_prepare(&batch, 2,
						       vector2_rx_alloc, NULL,
						       NULL), 0);
	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_prepare(&batch, 1,
						       vector2_rx_alloc, NULL,
						       NULL), -EBUSY);
	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_complete(&batch, 3, NULL, NULL,
							NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, batch.prepared, 2U);
	KUNIT_EXPECT_EQ(test, batch.filled, 0U);

	um_vec2_rx_batch_reset(&batch, vector2_queue_trace_release, &trace);
	KUNIT_EXPECT_TRUE(test, um_vec2_rx_batch_idle(&batch));
	KUNIT_EXPECT_EQ(test, trace.count, 2U);
}

static void vector2_queue_rx_reset_releases_filled_test(struct kunit *test)
{
	struct vector2_queue_trace trace = {};
	struct um_vec2_rx_slot slots[3];
	struct um_vec2_rx_batch batch;
	unsigned int lens[] = { 64, 128 };

	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_init(&batch, slots, 3), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_prepare(&batch, 2,
						       vector2_rx_alloc, NULL,
						       NULL), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_complete(&batch, 2, lens, NULL,
							NULL), 0);
	um_vec2_rx_batch_reset(&batch, vector2_queue_trace_release, &trace);
	KUNIT_EXPECT_TRUE(test, um_vec2_rx_batch_idle(&batch));
	KUNIT_EXPECT_EQ(test, trace.count, 2U);
	KUNIT_EXPECT_EQ(test, trace.len_sum, 192U);
}

static struct kunit_case vector2_queue_test_cases[] = {
	KUNIT_CASE(vector2_queue_tx_wraparound_test),
	KUNIT_CASE(vector2_queue_tx_partial_completion_test),
	KUNIT_CASE(vector2_queue_tx_rejects_invalid_test),
	KUNIT_CASE(vector2_queue_tx_reset_releases_test),
	KUNIT_CASE(vector2_queue_rx_prepare_receive_consume_test),
	KUNIT_CASE(vector2_queue_rx_alloc_failure_unwinds_test),
	KUNIT_CASE(vector2_queue_rx_prepare_next_test),
	KUNIT_CASE(vector2_queue_rx_rejects_busy_and_invalid_test),
	KUNIT_CASE(vector2_queue_rx_reset_releases_filled_test),
	{}
};

static struct kunit_suite vector2_queue_test_suite = {
	.name = "um_vector2_queue",
	.test_cases = vector2_queue_test_cases,
};

kunit_test_suite(vector2_queue_test_suite);
