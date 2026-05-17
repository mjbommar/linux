/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Typed configuration for the UML vector networking v2 rewrite.
 *
 * This header is intentionally independent of the old arglist parser in
 * vector_user.h.  The v2 parser keeps command-line compatibility at the
 * boundary, then hands the rest of the driver a validated structure with
 * explicit policy decisions.
 */
#ifndef __UM_VECTOR2_CONFIG_H
#define __UM_VECTOR2_CONFIG_H

#include <linux/bits.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/types.h>

#define UM_VEC2_DEFAULT_DEPTH		64U
#define UM_VEC2_DEFAULT_HEADROOM	2U
#define UM_VEC2_DEFAULT_MTU		1500U
#define UM_VEC2_DEFAULT_QUEUES		1U

#define UM_VEC2_MAX_DEPTH		4096U
#define UM_VEC2_MAX_HEADROOM		4096U
#define UM_VEC2_MAX_QUEUES		1024U
#define UM_VEC2_MAX_MTU			65535U
#define UM_VEC2_MIN_MTU			576U

#define UM_VEC2_ADDR_MAX		128
#define UM_VEC2_PORT_MAX		32
#define UM_VEC2_PATH_MAX		256
#define UM_VEC2_DESCR_MAX		128

enum um_vec2_transport {
	UM_VEC2_TRANSPORT_UNSPEC,
	UM_VEC2_TRANSPORT_TAP,
	UM_VEC2_TRANSPORT_RAW,
	UM_VEC2_TRANSPORT_GRE,
	UM_VEC2_TRANSPORT_L2TPV3,
	UM_VEC2_TRANSPORT_HYBRID,
	UM_VEC2_TRANSPORT_BESS,
	UM_VEC2_TRANSPORT_FD,
	UM_VEC2_TRANSPORT_VDE,
	UM_VEC2_TRANSPORT_PROXY,
};

enum um_vec2_host_mode {
	UM_VEC2_HOST_AUTO,
	UM_VEC2_HOST_INPROC,
	UM_VEC2_HOST_FD,
	UM_VEC2_HOST_PROXY,
};

enum um_vec2_parse_flags {
	/*
	 * Ignore unknown keys.  This exists only for staged migration from
	 * the legacy parser; strict mode is the default.
	 */
	UM_VEC2_PARSE_COMPAT		= BIT(0),

	/*
	 * Permit options that make the UML process directly consume host
	 * networking authority: host interface names, raw addresses, helper
	 * scripts, and BPF files.  Inherited fd numbers are intentionally
	 * outside this flag: they consume authority already delegated by the
	 * launcher and do not create host networking resources inside UML.
	 */
	UM_VEC2_PARSE_TRUSTED_HOST	= BIT(1),
};

struct um_vec2_config_error {
	char key[32];
	char msg[96];
};

struct um_vec2_config {
	enum um_vec2_transport transport;
	enum um_vec2_host_mode mode;

	unsigned int depth;
	unsigned int headroom;
	unsigned int mtu;
	unsigned int queues;
	unsigned int coalesce_usecs;
	unsigned int fail_open_after;

	bool batching;
	bool gro;
	bool gso;
	bool csum;
	bool has_mac;
	u8 mac[ETH_ALEN];

	bool has_v6;
	bool v6;
	bool has_udp;
	bool udp;

	bool has_fd;
	unsigned int fd;

	bool has_rx_key;
	bool has_tx_key;
	u32 rx_key;
	u32 tx_key;
	bool sequence;
	bool pin_sequence;

	bool has_rx_session;
	bool has_tx_session;
	u32 rx_session;
	u32 tx_session;
	bool cookie64;
	bool has_rx_cookie;
	bool has_tx_cookie;
	u64 rx_cookie;
	u64 tx_cookie;
	bool counter;
	bool pin_counter;

	char ifname[IFNAMSIZ];
	char src[UM_VEC2_ADDR_MAX];
	char dst[UM_VEC2_ADDR_MAX];
	char srcport[UM_VEC2_PORT_MAX];
	char dstport[UM_VEC2_PORT_MAX];
	char ifup[UM_VEC2_PATH_MAX];
	char bpffile[UM_VEC2_PATH_MAX];
	char vnl[UM_VEC2_PATH_MAX];
	char descr[UM_VEC2_DESCR_MAX];
	char port[UM_VEC2_PORT_MAX];
	char group[UM_VEC2_PORT_MAX];
};

void um_vec2_config_init(struct um_vec2_config *cfg);
int um_vec2_config_parse(const char *spec, unsigned int flags,
			 struct um_vec2_config *cfg,
			 struct um_vec2_config_error *err);
const char *um_vec2_transport_name(enum um_vec2_transport transport);
const char *um_vec2_host_mode_name(enum um_vec2_host_mode mode);

#endif /* __UM_VECTOR2_CONFIG_H */
