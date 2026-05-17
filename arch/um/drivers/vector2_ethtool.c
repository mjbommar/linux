// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal ethtool surface for UML vector networking v2.
 */

#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/string.h>

#include "vector2_internal.h"

static void um_vec2_get_drvinfo(struct net_device *dev,
	struct ethtool_drvinfo *info)
{
	strscpy(info->driver, UM_VEC2_DRIVER_NAME, sizeof(info->driver));
	strscpy(info->version, "r4-tap-skeleton", sizeof(info->version));
	strscpy(info->bus_info, "uml", sizeof(info->bus_info));
}

static const struct ethtool_ops um_vec2_ethtool_ops = {
	.get_drvinfo	= um_vec2_get_drvinfo,
	.get_link	= ethtool_op_get_link,
	.get_ts_info	= ethtool_op_get_ts_info,
};

void um_vec2_ethtool_attach(struct net_device *dev)
{
	dev->ethtool_ops = &um_vec2_ethtool_ops;
}
