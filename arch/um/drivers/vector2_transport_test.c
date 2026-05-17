// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit coverage for UML vector networking v2 transport header helpers.
 */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/unaligned.h>

#include "vector2_transport.h"

static void vector2_transport_gre_key_sequence_test(struct kunit *test)
{
	struct um_vec2_gre_spec spec = {
		.has_key = true,
		.has_sequence = true,
		.rx_key = 0x01020304,
		.tx_key = 0x01020304,
	};
	struct um_vec2_gre_rx rx;
	u8 buf[12];

	KUNIT_EXPECT_EQ(test, um_vec2_gre_header_len(&spec), 12UL);
	KUNIT_EXPECT_EQ(test, um_vec2_gre_build(&spec, buf, sizeof(buf),
						0x11223344), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(buf),
			(u16)(UM_VEC2_GRE_FLAG_KEY | UM_VEC2_GRE_FLAG_SEQ));
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(buf + 2),
			(u16)UM_VEC2_GRE_PROTO_TEB);
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(buf + 4), spec.tx_key);
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(buf + 8), 0x11223344U);

	KUNIT_EXPECT_EQ(test, um_vec2_gre_parse(&spec, buf, sizeof(buf), &rx),
			0);
	KUNIT_EXPECT_TRUE(test, rx.has_sequence);
	KUNIT_EXPECT_EQ(test, rx.sequence, 0x11223344U);
}

static void vector2_transport_gre_short_buffer_test(struct kunit *test)
{
	struct um_vec2_gre_spec spec = {
		.has_key = true,
		.tx_key = 1,
	};
	u8 buf[8] = { 0xaa, };

	KUNIT_EXPECT_EQ(test, um_vec2_gre_build(&spec, buf, 7, 0),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, buf[0], 0xaa);
	KUNIT_EXPECT_EQ(test, um_vec2_gre_parse(&spec, buf, 7, NULL),
			-EMSGSIZE);
}

static void vector2_transport_gre_mismatch_test(struct kunit *test)
{
	struct um_vec2_gre_spec build = {
		.has_key = true,
		.tx_key = 0x01020304,
	};
	struct um_vec2_gre_spec parse = {
		.has_key = true,
		.rx_key = 0x05060708,
	};
	u8 buf[8];

	KUNIT_ASSERT_EQ(test, um_vec2_gre_build(&build, buf, sizeof(buf), 0),
			0);
	KUNIT_EXPECT_EQ(test, um_vec2_gre_parse(&parse, buf, sizeof(buf),
						NULL), -EPROTO);
}

static void vector2_transport_gre_minimal_test(struct kunit *test)
{
	struct um_vec2_gre_spec spec = {};
	u8 buf[4];

	KUNIT_EXPECT_EQ(test, um_vec2_gre_header_len(&spec), 4UL);
	KUNIT_EXPECT_EQ(test, um_vec2_gre_build(&spec, buf, sizeof(buf), 0),
			0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(buf), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(buf + 2),
			(u16)UM_VEC2_GRE_PROTO_TEB);
	KUNIT_EXPECT_EQ(test, um_vec2_gre_parse(&spec, buf, sizeof(buf),
						NULL), 0);
}

static void vector2_transport_l2tpv3_udp_cookie_counter_test(struct kunit *test)
{
	struct um_vec2_l2tpv3_spec spec = {
		.udp = true,
		.has_cookie = true,
		.cookie64 = true,
		.has_counter = true,
		.rx_session = 0x01020304,
		.tx_session = 0x01020304,
		.rx_cookie = 0x1112131415161718ULL,
		.tx_cookie = 0x1112131415161718ULL,
	};
	struct um_vec2_l2tpv3_rx rx;
	u8 buf[20];

	KUNIT_EXPECT_EQ(test, um_vec2_l2tpv3_header_len(&spec), 20UL);
	KUNIT_EXPECT_EQ(test, um_vec2_l2tpv3_build(&spec, buf, sizeof(buf),
						   0x21222324), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(buf),
			UM_VEC2_L2TPV3_DATA_PACKET);
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(buf + 4), spec.tx_session);
	KUNIT_EXPECT_EQ(test, get_unaligned_be64(buf + 8), spec.tx_cookie);
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(buf + 16), 0x21222324U);

	KUNIT_EXPECT_EQ(test, um_vec2_l2tpv3_parse(&spec, buf, sizeof(buf),
						   &rx), 0);
	KUNIT_EXPECT_TRUE(test, rx.has_counter);
	KUNIT_EXPECT_EQ(test, rx.counter, 0x21222324U);
}

static void vector2_transport_l2tpv3_cookie32_test(struct kunit *test)
{
	struct um_vec2_l2tpv3_spec spec = {
		.has_cookie = true,
		.rx_session = 0x01020304,
		.tx_session = 0x01020304,
		.rx_cookie = 0x11223344,
		.tx_cookie = 0x11223344,
	};
	u8 buf[8];

	KUNIT_EXPECT_EQ(test, um_vec2_l2tpv3_header_len(&spec), 8UL);
	KUNIT_EXPECT_EQ(test, um_vec2_l2tpv3_build(&spec, buf, sizeof(buf),
						   0), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(buf), spec.tx_session);
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(buf + 4),
			(u32)spec.tx_cookie);
	KUNIT_EXPECT_EQ(test, um_vec2_l2tpv3_parse(&spec, buf, sizeof(buf),
						   NULL), 0);
}

static void vector2_transport_l2tpv3_short_buffer_test(struct kunit *test)
{
	struct um_vec2_l2tpv3_spec spec = {
		.udp = true,
		.rx_session = 1,
		.tx_session = 1,
	};
	u8 buf[8] = { 0xbb, };

	KUNIT_EXPECT_EQ(test, um_vec2_l2tpv3_build(&spec, buf, 7, 0),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, buf[0], 0xbb);
	KUNIT_EXPECT_EQ(test, um_vec2_l2tpv3_parse(&spec, buf, 7, NULL),
			-EMSGSIZE);
}

static void vector2_transport_l2tpv3_mismatch_test(struct kunit *test)
{
	struct um_vec2_l2tpv3_spec build = {
		.rx_session = 1,
		.tx_session = 1,
	};
	struct um_vec2_l2tpv3_spec parse = {
		.rx_session = 2,
		.tx_session = 2,
	};
	u8 buf[4];

	KUNIT_ASSERT_EQ(test, um_vec2_l2tpv3_build(&build, buf, sizeof(buf),
						   0), 0);
	KUNIT_EXPECT_EQ(test, um_vec2_l2tpv3_parse(&parse, buf, sizeof(buf),
						   NULL), -EPROTO);
}

static struct kunit_case vector2_transport_test_cases[] = {
	KUNIT_CASE(vector2_transport_gre_key_sequence_test),
	KUNIT_CASE(vector2_transport_gre_short_buffer_test),
	KUNIT_CASE(vector2_transport_gre_mismatch_test),
	KUNIT_CASE(vector2_transport_gre_minimal_test),
	KUNIT_CASE(vector2_transport_l2tpv3_udp_cookie_counter_test),
	KUNIT_CASE(vector2_transport_l2tpv3_cookie32_test),
	KUNIT_CASE(vector2_transport_l2tpv3_short_buffer_test),
	KUNIT_CASE(vector2_transport_l2tpv3_mismatch_test),
	{}
};

static struct kunit_suite vector2_transport_test_suite = {
	.name = "um_vector2_transport",
	.test_cases = vector2_transport_test_cases,
};

kunit_test_suite(vector2_transport_test_suite);
