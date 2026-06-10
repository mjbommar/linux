// SPDX-License-Identifier: GPL-2.0
/*
 * Command-line collection for UML vector networking v2.
 */

#define pr_fmt(fmt) "uml-vector2: " fmt

#include <linux/cache.h>
#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/string.h>

#include <init.h>

#include "vector2_internal.h"

static LIST_HEAD(um_vec2_cmdline_specs);

int um_vec2_cmdline_parse_spec(const char *arg,
			       enum um_vec2_cmdline_form form,
			       unsigned int *unit, const char **spec)
{
	const char *delim;
	char unit_buf[16];
	size_t unit_len;
	size_t i;
	int ret;

	if (!arg || !unit || !spec)
		return -EINVAL;

	if (form == UM_VEC2_CMDLINE_DOT)
		delim = strchr(arg, ':');
	else
		delim = strchr(arg, ',');
	if (!delim || delim == arg || delim[1] == '\0')
		return -EINVAL;

	unit_len = delim - arg;
	if (unit_len >= sizeof(unit_buf))
		return -E2BIG;
	for (i = 0; i < unit_len; i++) {
		if (!isdigit(arg[i]))
			return -EINVAL;
	}

	memcpy(unit_buf, arg, unit_len);
	unit_buf[unit_len] = '\0';
	ret = kstrtouint(unit_buf, 10, unit);
	if (ret)
		return ret;

	*spec = delim + 1;
	return 0;
}

static bool um_vec2_cmdline_unit_exists(unsigned int unit)
{
	struct um_vec2_cmdline_spec *entry;

	list_for_each_entry(entry, &um_vec2_cmdline_specs, list) {
		if (entry->unit == unit)
			return true;
	}
	return false;
}

static int __init um_vec2_cmdline_add(char *arg,
				      enum um_vec2_cmdline_form form)
{
	struct um_vec2_cmdline_spec *entry;
	const char *spec;
	unsigned int unit;
	int ret;

	ret = um_vec2_cmdline_parse_spec(arg, form, &unit, &spec);
	if (ret) {
		pr_err("could not parse vec2 spec '%s': %d\n", arg, ret);
		return ret;
	}
	if (um_vec2_cmdline_unit_exists(unit)) {
		pr_err("duplicate vec2.%u spec ignored\n", unit);
		return -EEXIST;
	}

	entry = memblock_alloc_or_panic(sizeof(*entry), SMP_CACHE_BYTES);
	INIT_LIST_HEAD(&entry->list);
	entry->unit = unit;
	entry->spec = spec;
	list_add_tail(&entry->list, &um_vec2_cmdline_specs);
	return 0;
}

int um_vec2_cmdline_for_each(int (*fn)(const struct um_vec2_cmdline_spec *spec,
				       void *data),
			     void *data)
{
	struct um_vec2_cmdline_spec *entry;
	int ret;

	if (!fn)
		return -EINVAL;

	list_for_each_entry(entry, &um_vec2_cmdline_specs, list) {
		ret = fn(entry, data);
		if (ret)
			return ret;
	}
	return 0;
}

static int __init um_vec2_setup_dot(char *str)
{
	um_vec2_cmdline_add(str, UM_VEC2_CMDLINE_DOT);
	return 1;
}

static int __init um_vec2_setup_equals(char *str)
{
	um_vec2_cmdline_add(str, UM_VEC2_CMDLINE_EQUALS);
	return 1;
}

__setup("vec2.", um_vec2_setup_dot);
__setup("vec2=", um_vec2_setup_equals);

__uml_help(um_vec2_setup_dot,
	   "vec2.<n>:<option>=<value>,<option>=<value>\n"
	   "    Configure a vector io v2 network device.\n\n");

__uml_help(um_vec2_setup_equals,
	   "vec2=<n>,<option>=<value>,<option>=<value>\n"
	   "    Configure a vector io v2 network device.\n\n");
