// SPDX-License-Identifier: GPL-2.0
/*
 * Experimental runtime core for UML vector networking v2.
 */

#define pr_fmt(fmt) "uml-vector2: " fmt

#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kconfig.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "vector2_internal.h"

static LIST_HEAD(um_vec2_devices);
static DEFINE_MUTEX(um_vec2_devices_lock);

struct um_vec2_build_context {
	unsigned int parse_flags;
	unsigned int found;
	unsigned int failed;
	unsigned int configured;
};

static unsigned int um_vec2_runtime_parse_flags(void)
{
	unsigned int flags = 0;

	if (IS_ENABLED(CONFIG_UML_NET_VECTOR_V2_INPROC))
		flags |= UM_VEC2_PARSE_TRUSTED_HOST;

	return flags;
}

static void um_vec2_core_free_all(void)
{
	struct um_vec2_dev *vdev, *tmp;

	mutex_lock(&um_vec2_devices_lock);
	list_for_each_entry_safe(vdev, tmp, &um_vec2_devices, list) {
		list_del(&vdev->list);
		if (um_vec2_dev_can_transition(vdev->life.state, UM_VEC2_DEV_DEAD))
			um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_DEAD);
		kfree(vdev);
	}
	mutex_unlock(&um_vec2_devices_lock);
}

static int __init um_vec2_validate_one(const struct um_vec2_cmdline_spec *spec,
				       void *data)
{
	struct um_vec2_build_context *ctx = data;
	struct um_vec2_config_error err;
	struct um_vec2_config *cfg;
	int ret;

	ctx->found++;
	cfg = kzalloc_obj(*cfg);
	if (!cfg)
		return -ENOMEM;

	ret = um_vec2_config_parse(spec->spec, ctx->parse_flags, cfg, &err);
	if (ret) {
		pr_err("vec2.%u config rejected: key='%s' msg='%s' ret=%d\n",
		       spec->unit, err.key, err.msg, ret);
		ctx->failed++;
	}
	kfree(cfg);
	return 0;
}

static int __init um_vec2_configure_one(const struct um_vec2_cmdline_spec *spec,
					void *data)
{
	struct um_vec2_build_context *ctx = data;
	struct um_vec2_config_error err;
	struct um_vec2_dev *vdev;
	int ret;

	vdev = kzalloc_obj(*vdev);
	if (!vdev)
		return -ENOMEM;

	INIT_LIST_HEAD(&vdev->list);
	mutex_init(&vdev->lock);
	vdev->unit = spec->unit;
	um_vec2_dev_lifecycle_init(&vdev->life);

	ret = um_vec2_config_parse(spec->spec, ctx->parse_flags, &vdev->cfg,
				   &err);
	if (ret) {
		pr_err("vec2.%u config rejected after validation: key='%s' msg='%s' ret=%d\n",
		       spec->unit, err.key, err.msg, ret);
		kfree(vdev);
		return ret;
	}

	ret = um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_CONFIGURED);
	if (ret) {
		kfree(vdev);
		return ret;
	}

	ret = um_vec2_netdev_register(vdev);
	if (ret) {
		um_vec2_dev_transition(&vdev->life, UM_VEC2_DEV_DEAD);
		kfree(vdev);
		return ret;
	}

	mutex_lock(&um_vec2_devices_lock);
	list_add_tail(&vdev->list, &um_vec2_devices);
	mutex_unlock(&um_vec2_devices_lock);

	ctx->configured++;
	pr_info("vec2.%u configured transport=%s mode=%s requested_queues=%u runtime_queues=%u depth=%u\n",
		vdev->unit, um_vec2_transport_name(vdev->cfg.transport),
		um_vec2_host_mode_name(vdev->cfg.mode), vdev->cfg.queues,
		vdev->registered_queues, vdev->cfg.depth);
	return 0;
}

static int __init um_vec2_core_init(void)
{
	struct um_vec2_build_context ctx = {
		.parse_flags = um_vec2_runtime_parse_flags(),
	};
	int ret;

	ret = um_vec2_cmdline_for_each(um_vec2_validate_one, &ctx);
	if (ret)
		return ret;

	if (!ctx.found)
		return 0;

	if (ctx.failed) {
		pr_err("%u vec2 command-line spec(s) rejected; no v2 devices configured\n",
		       ctx.failed);
		return -EINVAL;
	}

	ret = um_vec2_cmdline_for_each(um_vec2_configure_one, &ctx);
	if (ret) {
		um_vec2_core_free_all();
		return ret;
	}

	pr_info("configured %u experimental v2 runtime device(s)\n",
		ctx.configured);
	return 0;
}

late_initcall(um_vec2_core_init);
