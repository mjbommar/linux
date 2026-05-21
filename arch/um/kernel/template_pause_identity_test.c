// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit coverage for the UML template-pause identity-apply parse
 * helpers (Memo 09 Phase 2).
 *
 * The full apply path (dev_set_mac_address, devinet_ioctl,
 * ip_rt_ioctl) needs a registered netdev and an init_net with a
 * working IP stack — neither of which is available in a KUnit-only
 * test build.  Phase 2 of the selftest harness drives the apply path
 * end-to-end against a real UML kernel boot; here we cover the
 * non-IO helpers that determine the correctness of the blob → kernel
 * arguments transformation.
 */

#include <kunit/test.h>
#include <linux/byteorder/generic.h>
#include <linux/errno.h>
#include <linux/types.h>

#include "template_pause_identity.h"

static void parse_cidr_basic_test(struct kunit *test)
{
	__be32 addr = 0;
	u8 prefix = 0;
	int rc;

	rc = um_template_identity_parse_cidr("10.7.0.42/24", &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, 0);
	KUNIT_EXPECT_EQ(test, ntohl(addr), 0x0a07002au);
	KUNIT_EXPECT_EQ(test, (int)prefix, 24);
}

static void parse_cidr_slash_zero_test(struct kunit *test)
{
	__be32 addr = 0;
	u8 prefix = 0xff;
	int rc;

	rc = um_template_identity_parse_cidr("0.0.0.0/0", &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, 0);
	KUNIT_EXPECT_EQ(test, ntohl(addr), 0u);
	KUNIT_EXPECT_EQ(test, (int)prefix, 0);
}

static void parse_cidr_slash_32_test(struct kunit *test)
{
	__be32 addr = 0;
	u8 prefix = 0;
	int rc;

	rc = um_template_identity_parse_cidr("192.168.1.1/32", &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, 0);
	KUNIT_EXPECT_EQ(test, ntohl(addr), 0xc0a80101u);
	KUNIT_EXPECT_EQ(test, (int)prefix, 32);
}

static void parse_cidr_rejects_missing_slash_test(struct kunit *test)
{
	__be32 addr = 0xdeadbeef;
	u8 prefix = 0xff;
	int rc;

	rc = um_template_identity_parse_cidr("10.7.0.42", &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, -EINVAL);
}

static void parse_cidr_rejects_bad_prefix_test(struct kunit *test)
{
	__be32 addr = 0;
	u8 prefix = 0;
	int rc;

	rc = um_template_identity_parse_cidr("10.7.0.42/33", &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, -EINVAL);

	rc = um_template_identity_parse_cidr("10.7.0.42/abc", &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, -EINVAL);

	rc = um_template_identity_parse_cidr("10.7.0.42/", &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, -EINVAL);
}

static void parse_cidr_rejects_bad_addr_test(struct kunit *test)
{
	__be32 addr = 0;
	u8 prefix = 0;
	int rc;

	rc = um_template_identity_parse_cidr("999.999.999.999/24",
					     &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, -EINVAL);

	rc = um_template_identity_parse_cidr("/24", &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, -EINVAL);
}

static void parse_cidr_empty_string_test(struct kunit *test)
{
	__be32 addr = 0;
	u8 prefix = 0;
	int rc;

	rc = um_template_identity_parse_cidr("", &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, -ENODATA);

	rc = um_template_identity_parse_cidr(NULL, &addr, &prefix);
	KUNIT_EXPECT_EQ(test, rc, -ENODATA);
}

static void parse_addr_basic_test(struct kunit *test)
{
	__be32 addr = 0;
	int rc;

	rc = um_template_identity_parse_addr("10.7.0.1", &addr);
	KUNIT_EXPECT_EQ(test, rc, 0);
	KUNIT_EXPECT_EQ(test, ntohl(addr), 0x0a070001u);
}

static void parse_addr_empty_test(struct kunit *test)
{
	__be32 addr = 0;
	int rc;

	rc = um_template_identity_parse_addr("", &addr);
	KUNIT_EXPECT_EQ(test, rc, -ENODATA);

	rc = um_template_identity_parse_addr(NULL, &addr);
	KUNIT_EXPECT_EQ(test, rc, -ENODATA);
}

static void parse_addr_rejects_garbage_test(struct kunit *test)
{
	__be32 addr = 0;
	int rc;

	rc = um_template_identity_parse_addr("not-an-ip", &addr);
	KUNIT_EXPECT_EQ(test, rc, -EINVAL);

	rc = um_template_identity_parse_addr("999.0.0.1", &addr);
	KUNIT_EXPECT_EQ(test, rc, -EINVAL);
}

static void cidr_mask_zero_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, um_template_identity_cidr_mask(0), 0u);
}

static void cidr_mask_classful_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, ntohl(um_template_identity_cidr_mask(8)),
			0xff000000u);
	KUNIT_EXPECT_EQ(test, ntohl(um_template_identity_cidr_mask(16)),
			0xffff0000u);
	KUNIT_EXPECT_EQ(test, ntohl(um_template_identity_cidr_mask(24)),
			0xffffff00u);
}

static void cidr_mask_thirtytwo_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, ntohl(um_template_identity_cidr_mask(32)),
			0xffffffffu);
	/* Out-of-range clamps to /32, never wraps. */
	KUNIT_EXPECT_EQ(test, ntohl(um_template_identity_cidr_mask(33)),
			0xffffffffu);
}

static struct kunit_case um_template_identity_test_cases[] = {
	KUNIT_CASE(parse_cidr_basic_test),
	KUNIT_CASE(parse_cidr_slash_zero_test),
	KUNIT_CASE(parse_cidr_slash_32_test),
	KUNIT_CASE(parse_cidr_rejects_missing_slash_test),
	KUNIT_CASE(parse_cidr_rejects_bad_prefix_test),
	KUNIT_CASE(parse_cidr_rejects_bad_addr_test),
	KUNIT_CASE(parse_cidr_empty_string_test),
	KUNIT_CASE(parse_addr_basic_test),
	KUNIT_CASE(parse_addr_empty_test),
	KUNIT_CASE(parse_addr_rejects_garbage_test),
	KUNIT_CASE(cidr_mask_zero_test),
	KUNIT_CASE(cidr_mask_classful_test),
	KUNIT_CASE(cidr_mask_thirtytwo_test),
	{}
};

static struct kunit_suite um_template_identity_test_suite = {
	.name = "um-template-pause-identity",
	.test_cases = um_template_identity_test_cases,
};

kunit_test_suite(um_template_identity_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for UML template-pause identity parse helpers");
