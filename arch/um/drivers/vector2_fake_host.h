/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Deterministic fake host backend for UML vector networking v2 tests.
 */
#ifndef __UM_VECTOR2_FAKE_HOST_H
#define __UM_VECTOR2_FAKE_HOST_H

#include <linux/types.h>

#include "vector2_host.h"

#define UM_VEC2_FAKE_HOST_MAX_RX	64U

struct um_vec2_fake_host_stats {
	u64 tx_calls;
	u64 tx_packets;
	u64 tx_errors;
	u64 rx_calls;
	u64 rx_packets;
	u64 rx_empty;
	u64 rx_errors;
	u64 fd_deaths;
};

struct um_vec2_fake_host {
	struct um_vec2_host host;
	struct um_vec2_fake_host_stats stats;
	bool dead;
	int tx_error;
	int rx_error;
	unsigned int tx_limit;
	unsigned int rx_limit;
	unsigned int rx_head;
	unsigned int rx_count;
	unsigned int rx_len[UM_VEC2_FAKE_HOST_MAX_RX];
};

void um_vec2_fake_host_init(struct um_vec2_fake_host *fake);
struct um_vec2_host *um_vec2_fake_host_base(struct um_vec2_fake_host *fake);
void um_vec2_fake_host_set_tx_limit(struct um_vec2_fake_host *fake,
				    unsigned int limit);
void um_vec2_fake_host_set_rx_limit(struct um_vec2_fake_host *fake,
				    unsigned int limit);
void um_vec2_fake_host_set_tx_error(struct um_vec2_fake_host *fake, int error);
void um_vec2_fake_host_set_rx_error(struct um_vec2_fake_host *fake, int error);
void um_vec2_fake_host_kill(struct um_vec2_fake_host *fake);
int um_vec2_fake_host_push_rx(struct um_vec2_fake_host *fake,
			      unsigned int len);

#endif /* __UM_VECTOR2_FAKE_HOST_H */
