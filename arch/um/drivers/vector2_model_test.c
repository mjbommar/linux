// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit coverage for UML vector networking v2 lifecycle states.
 */

#include <kunit/test.h>
#include <linux/errno.h>

#include "vector2_model.h"

static void vector2_model_dev_happy_path_test(struct kunit *test)
{
	struct um_vec2_dev_lifecycle life;

	um_vec2_dev_lifecycle_init(&life);
	KUNIT_EXPECT_EQ(test, life.state, UM_VEC2_DEV_NEW);
	KUNIT_EXPECT_FALSE(test, um_vec2_dev_can_open(&life));
	KUNIT_EXPECT_FALSE(test, um_vec2_dev_can_xmit(&life));

	KUNIT_EXPECT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_CONFIGURED),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_REGISTERED),
			0);
	KUNIT_EXPECT_TRUE(test, um_vec2_dev_can_open(&life));

	KUNIT_EXPECT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_OPENING),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_RUNNING),
			0);
	KUNIT_EXPECT_TRUE(test, um_vec2_dev_can_xmit(&life));

	KUNIT_EXPECT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_QUIESCING),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_REGISTERED),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_DEAD),
			0);
}

static void vector2_model_dev_open_failure_test(struct kunit *test)
{
	struct um_vec2_dev_lifecycle life;

	um_vec2_dev_lifecycle_init(&life);
	KUNIT_ASSERT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_CONFIGURED),
			0);
	KUNIT_ASSERT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_REGISTERED),
			0);
	KUNIT_ASSERT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_OPENING),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_QUIESCING),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_REGISTERED),
			0);
}

static void vector2_model_dev_rejects_illegal_test(struct kunit *test)
{
	struct um_vec2_dev_lifecycle life;

	um_vec2_dev_lifecycle_init(&life);
	KUNIT_EXPECT_EQ(test,
			um_vec2_dev_transition(&life, UM_VEC2_DEV_RUNNING),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, life.state, UM_VEC2_DEV_NEW);

	KUNIT_EXPECT_FALSE(test,
			   um_vec2_dev_can_transition(UM_VEC2_DEV_DEAD,
						      UM_VEC2_DEV_NEW));
}

static void vector2_model_chan_happy_path_test(struct kunit *test)
{
	struct um_vec2_chan_lifecycle life;

	um_vec2_chan_lifecycle_init(&life);
	KUNIT_EXPECT_EQ(test, life.state, UM_VEC2_CHAN_UNINIT);
	KUNIT_EXPECT_FALSE(test, um_vec2_chan_is_active(&life));

	KUNIT_EXPECT_EQ(test,
			um_vec2_chan_transition(&life, UM_VEC2_CHAN_ALLOCATED),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_chan_transition(&life, UM_VEC2_CHAN_FD_ATTACHED),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_chan_transition(&life,
						UM_VEC2_CHAN_IRQ_ATTACHED),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_chan_transition(&life,
						UM_VEC2_CHAN_NAPI_ENABLED),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_chan_transition(&life, UM_VEC2_CHAN_ACTIVE),
			0);
	KUNIT_EXPECT_TRUE(test, um_vec2_chan_is_active(&life));

	KUNIT_EXPECT_EQ(test,
			um_vec2_chan_transition(&life, UM_VEC2_CHAN_QUIESCING),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_chan_transition(&life, UM_VEC2_CHAN_CLOSED),
			0);
}

static void vector2_model_chan_failure_unwinds_test(struct kunit *test)
{
	struct um_vec2_chan_lifecycle life;

	um_vec2_chan_lifecycle_init(&life);
	KUNIT_ASSERT_EQ(test,
			um_vec2_chan_transition(&life, UM_VEC2_CHAN_ALLOCATED),
			0);
	KUNIT_ASSERT_EQ(test,
			um_vec2_chan_transition(&life, UM_VEC2_CHAN_FD_ATTACHED),
			0);
	KUNIT_ASSERT_EQ(test,
			um_vec2_chan_transition(&life,
						UM_VEC2_CHAN_IRQ_ATTACHED),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_chan_transition(&life, UM_VEC2_CHAN_QUIESCING),
			0);
	KUNIT_EXPECT_EQ(test,
			um_vec2_chan_transition(&life, UM_VEC2_CHAN_CLOSED),
			0);
}

static void vector2_model_chan_rejects_illegal_test(struct kunit *test)
{
	struct um_vec2_chan_lifecycle life;

	um_vec2_chan_lifecycle_init(&life);
	KUNIT_EXPECT_EQ(test,
			um_vec2_chan_transition(&life, UM_VEC2_CHAN_ACTIVE),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, life.state, UM_VEC2_CHAN_UNINIT);

	KUNIT_EXPECT_FALSE(test,
			   um_vec2_chan_can_transition(UM_VEC2_CHAN_CLOSED,
						       UM_VEC2_CHAN_ACTIVE));
}

static void vector2_model_state_names_test(struct kunit *test)
{
	KUNIT_EXPECT_STREQ(test,
			   um_vec2_dev_state_name(UM_VEC2_DEV_RUNNING),
			   "RUNNING");
	KUNIT_EXPECT_STREQ(test,
			   um_vec2_chan_state_name(UM_VEC2_CHAN_ACTIVE),
			   "ACTIVE");
	KUNIT_EXPECT_STREQ(test,
			   um_vec2_dev_state_name((enum um_vec2_dev_state)-1),
			   "INVALID");
	KUNIT_EXPECT_STREQ(test,
			   um_vec2_chan_state_name((enum um_vec2_chan_state)-1),
			   "INVALID");
}

static struct kunit_case vector2_model_test_cases[] = {
	KUNIT_CASE(vector2_model_dev_happy_path_test),
	KUNIT_CASE(vector2_model_dev_open_failure_test),
	KUNIT_CASE(vector2_model_dev_rejects_illegal_test),
	KUNIT_CASE(vector2_model_chan_happy_path_test),
	KUNIT_CASE(vector2_model_chan_failure_unwinds_test),
	KUNIT_CASE(vector2_model_chan_rejects_illegal_test),
	KUNIT_CASE(vector2_model_state_names_test),
	{}
};

static struct kunit_suite vector2_model_test_suite = {
	.name = "um_vector2_model",
	.test_cases = vector2_model_test_cases,
};

kunit_test_suite(vector2_model_test_suite);
