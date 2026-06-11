/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private runtime types for the UML vector networking v2 driver.
 *
 * This header keeps ownership boundaries internal without exposing driver
 * state as public API.
 */
#ifndef __UM_VECTOR2_INTERNAL_H
#define __UM_VECTOR2_INTERNAL_H

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/virtio_net.h>

#include "vector2_config.h"
#include "vector2_host.h"
#include "vector2_model.h"
#include "vector2_queue.h"

#define UM_VEC2_DRIVER_NAME	"uml-vector-v2"
#define UM_VEC2_NAME_PREFIX	"vec2"
#define UM_VEC2_NO_FD		(-1)
#define UM_VEC2_NO_IRQ		(-1)

enum um_vec2_stat_counter {
	UM_VEC2_STAT_OPEN_ATTEMPTS,
	UM_VEC2_STAT_OPEN_FAILURES,
	UM_VEC2_STAT_CLOSES,
	UM_VEC2_STAT_NAPI_POLLS,
	UM_VEC2_STAT_RX_IRQS,
	UM_VEC2_STAT_TX_IRQS,
	UM_VEC2_STAT_TX_XMIT_CALLS,
	UM_VEC2_STAT_TX_BUSY,
	UM_VEC2_STAT_TX_DROPPED,
	UM_VEC2_STAT_TX_TRANSIENT_ERRORS,
	UM_VEC2_STAT_TX_FATAL_ERRORS,
	UM_VEC2_STAT_RX_ALLOC_ERRORS,
	UM_VEC2_STAT_RX_PROTO_DROPS,
	UM_VEC2_STAT_RX_FATAL_ERRORS,
	UM_VEC2_STAT_BACKEND_DEAD,
	UM_VEC2_STAT_MAX,
};

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

struct um_vec2_stats {
	atomic64_t counter[UM_VEC2_STAT_MAX];
};

struct um_vec2_channel {
	struct um_vec2_chan_lifecycle life;
	struct um_vec2_dev *vdev;
	struct um_vec2_host *host;
	struct um_vec2_queue_pair *queue;
	struct napi_struct napi;
	unsigned int index;
	int rx_fd;
	int tx_fd;
	int rx_irq;
	int tx_irq;
	bool vnet_hdr;
	bool rx_pending;
	bool napi_added;
	bool napi_enabled;
};

struct um_vec2_dev {
	struct list_head list;
	struct mutex lock;		/* Serializes lifecycle transitions. */
	unsigned int unit;
	struct um_vec2_config cfg;
	struct um_vec2_dev_lifecycle life;
	struct um_vec2_stats stats;
	struct net_device *netdev;
	struct um_vec2_channel *channels;
	unsigned int num_channels;
	unsigned int registered_queues;
};

struct um_vec2_netdev_priv {
	struct um_vec2_dev *vdev;
};

static inline struct um_vec2_dev *um_vec2_dev_from_netdev(struct net_device *dev)
{
	struct um_vec2_netdev_priv *priv = netdev_priv(dev);

	return priv->vdev;
}

static inline void um_vec2_stat_inc(struct um_vec2_dev *vdev,
				    enum um_vec2_stat_counter counter)
{
	atomic64_inc(&vdev->stats.counter[counter]);
}

static inline u64 um_vec2_stat_inc_return(struct um_vec2_dev *vdev,
					  enum um_vec2_stat_counter counter)
{
	return atomic64_inc_return(&vdev->stats.counter[counter]);
}

static inline u64 um_vec2_stat_read(const struct um_vec2_dev *vdev,
				    enum um_vec2_stat_counter counter)
{
	return atomic64_read(&vdev->stats.counter[counter]);
}

static inline int um_vec2_apply_vnet_hdr(struct sk_buff *skb,
					 const struct virtio_net_hdr *hdr)
{
	if (hdr->flags & VIRTIO_NET_HDR_F_DATA_VALID) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		return 0;
	}

	return virtio_net_hdr_to_skb(skb, hdr, true) ? -EPROTO : 0;
}

int um_vec2_cmdline_parse_spec(const char *arg,
			       enum um_vec2_cmdline_form form,
			       unsigned int *unit, const char **spec);
int um_vec2_cmdline_for_each(int (*fn)(const struct um_vec2_cmdline_spec *spec,
				       void *data),
			     void *data);

void um_vec2_ethtool_attach(struct net_device *dev);

unsigned int um_vec2_runtime_frame_len(const struct net_device *dev,
				       bool vnet_hdr);
unsigned int um_vec2_rx_frame_len(const struct net_device *dev,
				  const struct um_vec2_channel *channel);
int um_vec2_write_skb(int fd, struct sk_buff *skb, bool vnet_hdr);
int um_vec2_queue_pair_alloc(struct um_vec2_channel *channel,
			     unsigned int depth);
void um_vec2_queue_pair_free(struct um_vec2_channel *channel,
			     struct net_device *dev);

int um_vec2_fd_open(struct um_vec2_dev *vdev);
void um_vec2_fd_close(struct um_vec2_dev *vdev);

#if IS_ENABLED(CONFIG_UML_NET_VECTOR_V2_HOST_FD_KUNIT)
/*
 * KUnit-only fault injector: when set to a non-negative value, the next
 * call to um_vec2_fd_channel_open() whose channel index matches will
 * return -EIO before allocating any host resources. Used to exercise
 * the partial-open unwind path. Set to -1 to disarm.
 */
extern int um_vec2_fd_fault_index;
#endif
int um_vec2_tap_open(struct um_vec2_dev *vdev);
int um_vec2_tap_attach_fd(struct um_vec2_dev *vdev, int fd);
void um_vec2_tap_close(struct um_vec2_dev *vdev);
/* Re-bind to a different host TAP name. */
int um_vec2_tap_reopen_for_pool_member(struct net_device *dev,
				       const char *new_ifname);

int um_vec2_netdev_open(struct net_device *dev);
int um_vec2_netdev_stop(struct net_device *dev);
netdev_tx_t um_vec2_netdev_start_xmit(struct sk_buff *skb,
				      struct net_device *dev);
u16 um_vec2_netdev_select_queue(struct net_device *dev, struct sk_buff *skb,
				struct net_device *sb_dev);
bool um_vec2_tx_queue_uses_cpu_ordinal(unsigned int queue,
				       unsigned int cpu_ordinal,
				       unsigned int queues,
				       unsigned int cpu_count);
void um_vec2_netdev_init(struct um_vec2_dev *vdev, struct net_device *dev);
unsigned int um_vec2_netdev_queue_count(const struct um_vec2_dev *vdev);
int um_vec2_netdev_register(struct um_vec2_dev *vdev);
void um_vec2_netdev_unregister(struct um_vec2_dev *vdev);

#endif /* __UM_VECTOR2_INTERNAL_H */
