/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Bounds-checked transport header helpers for UML vector networking v2.
 */
#ifndef __UM_VECTOR2_TRANSPORT_H
#define __UM_VECTOR2_TRANSPORT_H

#include <linux/types.h>

#define UM_VEC2_GRE_PROTO_TEB		0x6558U
#define UM_VEC2_GRE_FLAG_KEY		0x2000U
#define UM_VEC2_GRE_FLAG_SEQ		0x1000U
#define UM_VEC2_L2TPV3_DATA_PACKET	0x00030000U

struct um_vec2_gre_spec {
	bool has_key;
	bool has_sequence;
	bool pin_sequence;
	u32 rx_key;
	u32 tx_key;
};

struct um_vec2_gre_rx {
	bool has_sequence;
	u32 sequence;
};

struct um_vec2_l2tpv3_spec {
	bool udp;
	bool has_cookie;
	bool cookie64;
	bool has_counter;
	bool pin_counter;
	u32 rx_session;
	u32 tx_session;
	u64 rx_cookie;
	u64 tx_cookie;
};

struct um_vec2_l2tpv3_rx {
	bool has_counter;
	u32 counter;
};

size_t um_vec2_gre_header_len(const struct um_vec2_gre_spec *spec);
int um_vec2_gre_build(const struct um_vec2_gre_spec *spec, void *buf,
		      size_t len, u32 sequence);
int um_vec2_gre_parse(const struct um_vec2_gre_spec *spec, const void *buf,
		      size_t len, struct um_vec2_gre_rx *rx);

size_t um_vec2_l2tpv3_header_len(const struct um_vec2_l2tpv3_spec *spec);
int um_vec2_l2tpv3_build(const struct um_vec2_l2tpv3_spec *spec, void *buf,
			 size_t len, u32 counter);
int um_vec2_l2tpv3_parse(const struct um_vec2_l2tpv3_spec *spec,
			 const void *buf, size_t len,
			 struct um_vec2_l2tpv3_rx *rx);

#endif /* __UM_VECTOR2_TRANSPORT_H */
