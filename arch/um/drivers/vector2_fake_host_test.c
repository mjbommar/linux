// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit coverage for UML vector networking v2 fake host operations.
 */

#include <kunit/test.h>
#include <linux/errno.h>

#include "vector2_fake_host.h"

#define VECTOR2_FAKE_TRACE_MAX		16

struct vector2_fake_trace {
	unsigned int count;
	unsigned int len_sum;
	unsigned long owner[VECTOR2_FAKE_TRACE_MAX];
};

struct vector2_fake_rx_alloc_ctx {
	unsigned int fail_at;
	bool fail_enabled;
	struct vector2_fake_trace *trace;
};

static void vector2_fake_trace_release(void *owner, unsigned int len,
				       void *cookie)
{
	struct vector2_fake_trace *trace = cookie;

	if (trace->count >= VECTOR2_FAKE_TRACE_MAX)
		return;

	trace->owner[trace->count] = (unsigned long)owner;
	trace->count++;
	trace->len_sum += len;
}

static void vector2_fake_alloc_ctx_release(void *owner, unsigned int len,
					   void *cookie)
{
	struct vector2_fake_rx_alloc_ctx *ctx = cookie;

	vector2_fake_trace_release(owner, len, ctx->trace);
}

static void *vector2_fake_rx_alloc(unsigned int slot, void *cookie)
{
	struct vector2_fake_rx_alloc_ctx *ctx = cookie;

	if (ctx && ctx->fail_enabled && slot == ctx->fail_at)
		return NULL;

	return (void *)(unsigned long)(slot + 1);
}

static void vector2_fake_host_tx_complete_test(struct kunit *test)
{
	struct um_vec2_tx_desc desc[4];
	struct vector2_fake_trace trace = {};
	struct um_vec2_fake_host fake;
	struct um_vec2_host *host;
	struct um_vec2_tx_ring ring;

	um_vec2_fake_host_init(&fake);
	host = um_vec2_fake_host_base(&fake);
	KUNIT_ASSERT_STREQ(test, host->ops->name, "fake");
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_init(&ring, desc, 4), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)1UL, 10),
			0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)2UL, 20),
			0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)3UL, 30),
			0);

	KUNIT_EXPECT_EQ(test, host->ops->tx_batch(host, &ring, 3,
						  vector2_fake_trace_release,
						  &trace), 3);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_empty(&ring));
	KUNIT_EXPECT_EQ(test, trace.count, 3U);
	KUNIT_EXPECT_EQ(test, trace.len_sum, 60U);
	KUNIT_EXPECT_EQ(test, fake.stats.tx_packets, 3ULL);
}

static void vector2_fake_host_tx_partial_test(struct kunit *test)
{
	struct um_vec2_tx_desc desc[4];
	struct vector2_fake_trace trace = {};
	struct um_vec2_fake_host fake;
	struct um_vec2_host *host;
	struct um_vec2_tx_ring ring;
	const struct um_vec2_tx_desc *peek;

	um_vec2_fake_host_init(&fake);
	um_vec2_fake_host_set_tx_limit(&fake, 2);
	host = um_vec2_fake_host_base(&fake);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_init(&ring, desc, 4), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)1UL, 10),
			0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)2UL, 20),
			0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)3UL, 30),
			0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)4UL, 40),
			0);

	KUNIT_EXPECT_EQ(test, host->ops->tx_batch(host, &ring, 4,
						  vector2_fake_trace_release,
						  &trace), 2);
	KUNIT_EXPECT_EQ(test, ring.count, 2U);
	peek = um_vec2_tx_ring_peek(&ring, 0);
	KUNIT_ASSERT_NOT_NULL(test, peek);
	KUNIT_EXPECT_EQ(test, (unsigned long)peek->owner, 3UL);

	KUNIT_EXPECT_EQ(test, host->ops->tx_batch(host, &ring, 4,
						  vector2_fake_trace_release,
						  &trace), 2);
	KUNIT_EXPECT_TRUE(test, um_vec2_tx_ring_empty(&ring));
	KUNIT_EXPECT_EQ(test, trace.count, 4U);
}

static void vector2_fake_host_tx_error_preserves_test(struct kunit *test)
{
	struct um_vec2_tx_desc desc[2];
	struct vector2_fake_trace trace = {};
	struct um_vec2_fake_host fake;
	struct um_vec2_host *host;
	struct um_vec2_tx_ring ring;

	um_vec2_fake_host_init(&fake);
	um_vec2_fake_host_set_tx_error(&fake, -EAGAIN);
	host = um_vec2_fake_host_base(&fake);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_init(&ring, desc, 2), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)1UL, 10),
			0);

	KUNIT_EXPECT_EQ(test, host->ops->tx_batch(host, &ring, 1,
						  vector2_fake_trace_release,
						  &trace), -EAGAIN);
	KUNIT_EXPECT_EQ(test, ring.count, 1U);
	KUNIT_EXPECT_EQ(test, trace.count, 0U);
	KUNIT_EXPECT_EQ(test, fake.stats.tx_errors, 1ULL);
}

static void vector2_fake_host_tx_dead_preserves_test(struct kunit *test)
{
	struct um_vec2_tx_desc desc[2];
	struct vector2_fake_trace trace = {};
	struct um_vec2_fake_host fake;
	struct um_vec2_host *host;
	struct um_vec2_tx_ring ring;

	um_vec2_fake_host_init(&fake);
	um_vec2_fake_host_kill(&fake);
	host = um_vec2_fake_host_base(&fake);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_init(&ring, desc, 2), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_tx_ring_enqueue(&ring, (void *)1UL, 10),
			0);

	KUNIT_EXPECT_EQ(test, host->ops->tx_batch(host, &ring, 1,
						  vector2_fake_trace_release,
						  &trace), -ENODEV);
	KUNIT_EXPECT_EQ(test, ring.count, 1U);
	KUNIT_EXPECT_EQ(test, trace.count, 0U);
	KUNIT_EXPECT_EQ(test, fake.stats.fd_deaths, 1ULL);
}

static void vector2_fake_host_rx_packets_test(struct kunit *test)
{
	struct vector2_fake_trace release_trace = {};
	struct vector2_fake_trace consume_trace = {};
	struct um_vec2_rx_slot slots[4];
	struct um_vec2_rx_batch batch;
	struct um_vec2_fake_host fake;
	struct um_vec2_host *host;

	um_vec2_fake_host_init(&fake);
	host = um_vec2_fake_host_base(&fake);
	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_init(&batch, slots, 4), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_fake_host_push_rx(&fake, 64), 0);
	KUNIT_ASSERT_EQ(test, um_vec2_fake_host_push_rx(&fake, 128), 0);

	KUNIT_EXPECT_EQ(test, host->ops->rx_batch(host, &batch, 4,
						  vector2_fake_rx_alloc,
						  vector2_fake_trace_release,
						  &release_trace), 2);
	KUNIT_EXPECT_EQ(test, release_trace.count, 2U);
	KUNIT_EXPECT_EQ(test, batch.filled, 2U);
	KUNIT_EXPECT_EQ(test, fake.stats.rx_packets, 2ULL);
	KUNIT_EXPECT_EQ(test, um_vec2_rx_batch_consume(&batch, 2,
						       vector2_fake_trace_release,
						       &consume_trace), 0);
	KUNIT_EXPECT_EQ(test, consume_trace.count, 2U);
	KUNIT_EXPECT_EQ(test, consume_trace.len_sum, 192U);
	KUNIT_EXPECT_TRUE(test, um_vec2_rx_batch_idle(&batch));
}

static void vector2_fake_host_rx_empty_test(struct kunit *test)
{
	struct vector2_fake_trace release_trace = {};
	struct um_vec2_rx_slot slots[4];
	struct um_vec2_rx_batch batch;
	struct um_vec2_fake_host fake;
	struct um_vec2_host *host;

	um_vec2_fake_host_init(&fake);
	host = um_vec2_fake_host_base(&fake);
	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_init(&batch, slots, 4), 0);

	KUNIT_EXPECT_EQ(test, host->ops->rx_batch(host, &batch, 4,
						  vector2_fake_rx_alloc,
						  vector2_fake_trace_release,
						  &release_trace), 0);
	KUNIT_EXPECT_TRUE(test, um_vec2_rx_batch_idle(&batch));
	KUNIT_EXPECT_EQ(test, release_trace.count, 4U);
	KUNIT_EXPECT_EQ(test, fake.stats.rx_empty, 1ULL);
}

static void vector2_fake_host_rx_alloc_failure_test(struct kunit *test)
{
	struct vector2_fake_trace trace = {};
	struct vector2_fake_rx_alloc_ctx alloc = {
		.fail_at = 1,
		.fail_enabled = true,
		.trace = &trace,
	};
	struct um_vec2_rx_slot slots[4];
	struct um_vec2_rx_batch batch;
	struct um_vec2_fake_host fake;
	struct um_vec2_host *host;

	um_vec2_fake_host_init(&fake);
	host = um_vec2_fake_host_base(&fake);
	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_init(&batch, slots, 4), 0);

	KUNIT_EXPECT_EQ(test, host->ops->rx_batch(host, &batch, 4,
						  vector2_fake_rx_alloc,
						  vector2_fake_alloc_ctx_release,
						  &alloc), -ENOMEM);
	KUNIT_EXPECT_TRUE(test, um_vec2_rx_batch_idle(&batch));
	KUNIT_EXPECT_EQ(test, trace.count, 1U);
	KUNIT_EXPECT_EQ(test, fake.stats.rx_errors, 1ULL);
}

static void vector2_fake_host_rx_dead_preserves_test(struct kunit *test)
{
	struct vector2_fake_trace trace = {};
	struct um_vec2_rx_slot slots[4];
	struct um_vec2_rx_batch batch;
	struct um_vec2_fake_host fake;
	struct um_vec2_host *host;

	um_vec2_fake_host_init(&fake);
	um_vec2_fake_host_kill(&fake);
	host = um_vec2_fake_host_base(&fake);
	KUNIT_ASSERT_EQ(test, um_vec2_rx_batch_init(&batch, slots, 4), 0);

	KUNIT_EXPECT_EQ(test, host->ops->rx_batch(host, &batch, 4,
						  vector2_fake_rx_alloc,
						  vector2_fake_trace_release,
						  &trace), -ENODEV);
	KUNIT_EXPECT_TRUE(test, um_vec2_rx_batch_idle(&batch));
	KUNIT_EXPECT_EQ(test, trace.count, 0U);
	KUNIT_EXPECT_EQ(test, fake.stats.fd_deaths, 1ULL);
}

static struct kunit_case vector2_fake_host_test_cases[] = {
	KUNIT_CASE(vector2_fake_host_tx_complete_test),
	KUNIT_CASE(vector2_fake_host_tx_partial_test),
	KUNIT_CASE(vector2_fake_host_tx_error_preserves_test),
	KUNIT_CASE(vector2_fake_host_tx_dead_preserves_test),
	KUNIT_CASE(vector2_fake_host_rx_packets_test),
	KUNIT_CASE(vector2_fake_host_rx_empty_test),
	KUNIT_CASE(vector2_fake_host_rx_alloc_failure_test),
	KUNIT_CASE(vector2_fake_host_rx_dead_preserves_test),
	{}
};

static struct kunit_suite vector2_fake_host_test_suite = {
	.name = "um_vector2_fake_host",
	.test_cases = vector2_fake_host_test_cases,
};

kunit_test_suite(vector2_fake_host_test_suite);
