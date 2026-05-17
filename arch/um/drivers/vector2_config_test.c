// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit coverage for the UML vector networking v2 typed config parser.
 */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "vector2_config.h"

struct vector2_config_parse_result {
	struct um_vec2_config *cfg;
	struct um_vec2_config_error *err;
	int ret;
};

static struct vector2_config_parse_result parse_cfg(struct kunit *test,
						    const char *spec,
						    unsigned int flags)
{
	struct vector2_config_parse_result result;

	result.cfg = kunit_kzalloc(test, sizeof(*result.cfg), GFP_KERNEL);
	result.err = kunit_kzalloc(test, sizeof(*result.err), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, result.cfg);
	KUNIT_ASSERT_NOT_NULL(test, result.err);

	result.ret = um_vec2_config_parse(spec, flags, result.cfg, result.err);
	if (result.ret)
		kunit_info(test, "parse failed key='%s' msg='%s' ret=%d\n",
			   result.err->key, result.err->msg, result.ret);
	return result;
}

static void vector2_config_defaults_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=proxy", 0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_EQ(test, result.cfg->transport, UM_VEC2_TRANSPORT_PROXY);
	KUNIT_EXPECT_EQ(test, result.cfg->mode, UM_VEC2_HOST_AUTO);
	KUNIT_EXPECT_EQ(test, result.cfg->depth, UM_VEC2_DEFAULT_DEPTH);
	KUNIT_EXPECT_EQ(test, result.cfg->headroom, UM_VEC2_DEFAULT_HEADROOM);
	KUNIT_EXPECT_EQ(test, result.cfg->mtu, UM_VEC2_DEFAULT_MTU);
	KUNIT_EXPECT_EQ(test, result.cfg->queues, UM_VEC2_DEFAULT_QUEUES);
	KUNIT_EXPECT_TRUE(test, result.cfg->batching);
}

static void vector2_config_trusted_tap_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=tap,mode=inproc,ifname=tap0",
			   UM_VEC2_PARSE_TRUSTED_HOST);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_EQ(test, result.cfg->transport, UM_VEC2_TRANSPORT_TAP);
	KUNIT_EXPECT_EQ(test, result.cfg->mode, UM_VEC2_HOST_INPROC);
	KUNIT_EXPECT_STREQ(test, result.cfg->ifname, "tap0");

	result = parse_cfg(test, "transport=proxy,depth=128,queues=4,mtu=9000",
			   0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_EQ(test, result.cfg->depth, 128U);
	KUNIT_EXPECT_EQ(test, result.cfg->queues, 4U);
	KUNIT_EXPECT_EQ(test, result.cfg->mtu, 9000U);

	result = parse_cfg(test, "transport=proxy,fail_open_after=2", 0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_EQ(test, result.cfg->fail_open_after, 2U);

	result = parse_cfg(test, "transport=proxy,gro=1,gso=1,csum=1", 0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_TRUE(test, result.cfg->gro);
	KUNIT_EXPECT_TRUE(test, result.cfg->gso);
	KUNIT_EXPECT_TRUE(test, result.cfg->csum);

	result = parse_cfg(test, "transport=proxy,mac=02:00:00:00:00:01", 0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_mac);
	KUNIT_EXPECT_EQ(test, result.cfg->mac[0], 0x02);
}

static void vector2_config_sandbox_rejects_host_option_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=tap,ifname=tap0", 0);
	KUNIT_EXPECT_EQ(test, result.ret, -EACCES);
	KUNIT_EXPECT_STREQ(test, result.err->key, "ifname");

	result = parse_cfg(test, "transport=fd,fd=5", 0);
	KUNIT_EXPECT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_fd);
	KUNIT_EXPECT_EQ(test, result.cfg->fd, 5U);
}

static void vector2_config_exact_transport_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=tapraw",
			   UM_VEC2_PARSE_TRUSTED_HOST);
	KUNIT_EXPECT_EQ(test, result.ret, -EINVAL);
	KUNIT_EXPECT_STREQ(test, result.err->key, "transport");
}

static void vector2_config_rejects_duplicate_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=tap,transport=raw",
			   UM_VEC2_PARSE_TRUSTED_HOST);
	KUNIT_EXPECT_EQ(test, result.ret, -EEXIST);
	KUNIT_EXPECT_STREQ(test, result.err->key, "transport");
}

static void vector2_config_unknown_strict_and_compat_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=proxy,unknown=1", 0);
	KUNIT_EXPECT_EQ(test, result.ret, -EINVAL);
	KUNIT_EXPECT_STREQ(test, result.err->key, "unknown");

	result = parse_cfg(test, "transport=proxy,unknown=1",
			   UM_VEC2_PARSE_COMPAT);
	KUNIT_EXPECT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_EQ(test, result.cfg->transport, UM_VEC2_TRANSPORT_PROXY);
}

static void vector2_config_bounds_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=proxy,depth=0", 0);
	KUNIT_EXPECT_EQ(test, result.ret, -ERANGE);
	KUNIT_EXPECT_STREQ(test, result.err->key, "depth");

	result = parse_cfg(test, "transport=proxy,mtu=575", 0);
	KUNIT_EXPECT_EQ(test, result.ret, -ERANGE);
	KUNIT_EXPECT_STREQ(test, result.err->key, "mtu");

	result = parse_cfg(test, "transport=proxy,queues=0", 0);
	KUNIT_EXPECT_EQ(test, result.ret, -ERANGE);
	KUNIT_EXPECT_STREQ(test, result.err->key, "queues");
}

static void vector2_config_vec_zero_compat_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=proxy,depth=128,vec=0", 0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_FALSE(test, result.cfg->batching);
	KUNIT_EXPECT_EQ(test, result.cfg->depth, 1U);
}

static void vector2_config_bool_policy_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=proxy,gro=2", 0);
	KUNIT_EXPECT_EQ(test, result.ret, -ERANGE);
	result = parse_cfg(test, "transport=proxy,gro=2",
			   UM_VEC2_PARSE_COMPAT);
	KUNIT_EXPECT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_TRUE(test, result.cfg->gro);
}

static void vector2_config_fd_requires_fd_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=fd", UM_VEC2_PARSE_TRUSTED_HOST);
	KUNIT_EXPECT_EQ(test, result.ret, -EINVAL);
	KUNIT_EXPECT_STREQ(test, result.err->key, "fd");

	result = parse_cfg(test, "transport=fd,fd=5",
			   UM_VEC2_PARSE_TRUSTED_HOST);
	KUNIT_EXPECT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_fd);
	KUNIT_EXPECT_EQ(test, result.cfg->fd, 5U);
}

static void vector2_config_paired_keys_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=gre,rx_key=1", 0);
	KUNIT_EXPECT_EQ(test, result.ret, -EINVAL);
	KUNIT_EXPECT_STREQ(test, result.err->key, "rx_key");

	result = parse_cfg(test, "transport=gre,rx_key=1,tx_key=2", 0);
	KUNIT_EXPECT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_rx_key);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_tx_key);
}

static void vector2_config_l2tpv3_typed_fields_test(struct kunit *test)
{
	struct vector2_config_parse_result result;

	result = parse_cfg(test, "transport=l2tpv3,rx_session=10,tx_session=11",
			   0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_EQ(test, result.cfg->transport,
			UM_VEC2_TRANSPORT_L2TPV3);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_rx_session);
	KUNIT_EXPECT_EQ(test, result.cfg->rx_session, 10U);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_tx_session);
	KUNIT_EXPECT_EQ(test, result.cfg->tx_session, 11U);

	result = parse_cfg(test, "transport=l2tpv3,cookie64=1", 0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_TRUE(test, result.cfg->cookie64);

	result = parse_cfg(test, "transport=l2tpv3,rx_cookie=0x1234,tx_cookie=0x5678",
			   0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_rx_cookie);
	KUNIT_EXPECT_EQ(test, result.cfg->rx_cookie, 0x1234ULL);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_tx_cookie);
	KUNIT_EXPECT_EQ(test, result.cfg->tx_cookie, 0x5678ULL);

	result = parse_cfg(test, "transport=l2tpv3,counter=1,pin_counter=0",
			   0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_TRUE(test, result.cfg->counter);
	KUNIT_EXPECT_FALSE(test, result.cfg->pin_counter);

	result = parse_cfg(test, "transport=l2tpv3,udp=1,v6=1", 0);
	KUNIT_ASSERT_EQ(test, result.ret, 0);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_udp);
	KUNIT_EXPECT_TRUE(test, result.cfg->udp);
	KUNIT_EXPECT_TRUE(test, result.cfg->has_v6);
	KUNIT_EXPECT_TRUE(test, result.cfg->v6);
}

static struct kunit_case vector2_config_test_cases[] = {
	KUNIT_CASE(vector2_config_defaults_test),
	KUNIT_CASE(vector2_config_trusted_tap_test),
	KUNIT_CASE(vector2_config_sandbox_rejects_host_option_test),
	KUNIT_CASE(vector2_config_exact_transport_test),
	KUNIT_CASE(vector2_config_rejects_duplicate_test),
	KUNIT_CASE(vector2_config_unknown_strict_and_compat_test),
	KUNIT_CASE(vector2_config_bounds_test),
	KUNIT_CASE(vector2_config_vec_zero_compat_test),
	KUNIT_CASE(vector2_config_bool_policy_test),
	KUNIT_CASE(vector2_config_fd_requires_fd_test),
	KUNIT_CASE(vector2_config_paired_keys_test),
	KUNIT_CASE(vector2_config_l2tpv3_typed_fields_test),
	{}
};

static struct kunit_suite vector2_config_test_suite = {
	.name = "um_vector2_config",
	.test_cases = vector2_config_test_cases,
};

kunit_test_suite(vector2_config_test_suite);
