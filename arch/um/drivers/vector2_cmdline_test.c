// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit coverage for the UML vector networking v2 command-line splitter.
 */

#include <kunit/test.h>
#include <linux/errno.h>

#include "vector2_internal.h"

static void vector2_cmdline_dot_form_test(struct kunit *test)
{
	const char *spec;
	unsigned int unit;

	KUNIT_EXPECT_EQ(test,
			um_vec2_cmdline_parse_spec("0:transport=proxy",
						   UM_VEC2_CMDLINE_DOT,
						   &unit, &spec),
			0);
	KUNIT_EXPECT_EQ(test, unit, 0U);
	KUNIT_EXPECT_STREQ(test, spec, "transport=proxy");
}

static void vector2_cmdline_equals_form_test(struct kunit *test)
{
	const char *spec;
	unsigned int unit;

	KUNIT_EXPECT_EQ(test,
			um_vec2_cmdline_parse_spec("7,transport=proxy,depth=128",
						   UM_VEC2_CMDLINE_EQUALS,
						   &unit, &spec),
			0);
	KUNIT_EXPECT_EQ(test, unit, 7U);
	KUNIT_EXPECT_STREQ(test, spec, "transport=proxy,depth=128");
}

static void vector2_cmdline_rejects_missing_delimiter_test(struct kunit *test)
{
	const char *spec = NULL;
	unsigned int unit = 0;

	KUNIT_EXPECT_EQ(test,
			um_vec2_cmdline_parse_spec("0,transport=proxy",
						   UM_VEC2_CMDLINE_DOT,
						   &unit, &spec),
			-EINVAL);
	KUNIT_EXPECT_NULL(test, spec);
}

static void vector2_cmdline_rejects_empty_spec_test(struct kunit *test)
{
	const char *spec = NULL;
	unsigned int unit = 0;

	KUNIT_EXPECT_EQ(test,
			um_vec2_cmdline_parse_spec("0:",
						   UM_VEC2_CMDLINE_DOT,
						   &unit, &spec),
			-EINVAL);
	KUNIT_EXPECT_NULL(test, spec);
}

static void vector2_cmdline_rejects_bad_unit_test(struct kunit *test)
{
	const char *spec = NULL;
	unsigned int unit = 0;

	KUNIT_EXPECT_EQ(test,
			um_vec2_cmdline_parse_spec("x:transport=proxy",
						   UM_VEC2_CMDLINE_DOT,
						   &unit, &spec),
			-EINVAL);
	KUNIT_EXPECT_NULL(test, spec);
}

static struct kunit_case vector2_cmdline_test_cases[] = {
	KUNIT_CASE(vector2_cmdline_dot_form_test),
	KUNIT_CASE(vector2_cmdline_equals_form_test),
	KUNIT_CASE(vector2_cmdline_rejects_missing_delimiter_test),
	KUNIT_CASE(vector2_cmdline_rejects_empty_spec_test),
	KUNIT_CASE(vector2_cmdline_rejects_bad_unit_test),
	{}
};

static struct kunit_suite vector2_cmdline_test_suite = {
	.name = "um_vector2_cmdline",
	.test_cases = vector2_cmdline_test_cases,
};

kunit_test_suite(vector2_cmdline_test_suite);
