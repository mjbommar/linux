/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private runtime types for the UML vector networking v2 rewrite.
 *
 * The v2 runtime is intentionally built in phases.  This header gives each
 * phase named ownership boundaries without exposing partially implemented
 * driver state as public API.
 */
#ifndef __UM_VECTOR2_INTERNAL_H
#define __UM_VECTOR2_INTERNAL_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/types.h>

#include "vector2_config.h"
#include "vector2_host.h"
#include "vector2_model.h"
#include "vector2_queue.h"

#define UM_VEC2_DRIVER_NAME	"uml-vector-v2"
#define UM_VEC2_NAME_PREFIX	"vec2"

enum um_vec2_cmdline_form {
	UM_VEC2_CMDLINE_DOT,
	UM_VEC2_CMDLINE_EQUALS,
};

struct um_vec2_cmdline_spec {
	struct list_head list;
	unsigned int unit;
	const char *spec;
};

struct um_vec2_queue_pair {
	spinlock_t tx_lock;		/* Protects TX ring ownership. */
	spinlock_t rx_lock;		/* Protects RX batch ownership. */
	struct um_vec2_tx_ring tx;
	struct um_vec2_rx_batch rx;
	struct um_vec2_tx_desc *tx_desc;
	struct um_vec2_rx_slot *rx_slot;
};

struct um_vec2_channel {
	struct um_vec2_chan_lifecycle life;
	struct um_vec2_host *host;
	struct um_vec2_queue_pair *queue;
	int rx_irq;
	int tx_irq;
};

struct um_vec2_dev {
	struct list_head list;
	struct mutex lock;		/* Serializes lifecycle transitions. */
	unsigned int unit;
	struct um_vec2_config cfg;
	struct um_vec2_dev_lifecycle life;
	struct net_device *netdev;
	struct um_vec2_channel *channels;
	unsigned int num_channels;
};

struct um_vec2_netdev_priv {
	struct um_vec2_dev *vdev;
};

int um_vec2_cmdline_parse_spec(const char *arg,
			       enum um_vec2_cmdline_form form,
			       unsigned int *unit, const char **spec);
int um_vec2_cmdline_for_each(int (*fn)(const struct um_vec2_cmdline_spec *spec,
				       void *data),
			     void *data);

void um_vec2_ethtool_attach(struct net_device *dev);

int um_vec2_fd_open(struct um_vec2_dev *vdev);
void um_vec2_fd_close(struct um_vec2_dev *vdev);
int um_vec2_tap_open(struct um_vec2_dev *vdev);
int um_vec2_tap_attach_fd(struct um_vec2_dev *vdev, int fd);
void um_vec2_tap_close(struct um_vec2_dev *vdev);

int um_vec2_netdev_open(struct net_device *dev);
int um_vec2_netdev_stop(struct net_device *dev);
netdev_tx_t um_vec2_netdev_start_xmit(struct sk_buff *skb,
				      struct net_device *dev);
void um_vec2_netdev_init(struct um_vec2_dev *vdev, struct net_device *dev);
int um_vec2_netdev_register(struct um_vec2_dev *vdev);
void um_vec2_netdev_unregister(struct um_vec2_dev *vdev);

#endif /* __UM_VECTOR2_INTERNAL_H */
