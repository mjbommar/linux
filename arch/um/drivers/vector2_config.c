// SPDX-License-Identifier: GPL-2.0
/*
 * Typed configuration parser for UML vector networking v2.
 *
 * Validate option strings at the boundary and expose typed state to the
 * rest of the driver.
 */

#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/hex.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "vector2_config.h"

enum um_vec2_key_id {
	UM_VEC2_KEY_TRANSPORT,
	UM_VEC2_KEY_MODE,
	UM_VEC2_KEY_DEPTH,
	UM_VEC2_KEY_HEADROOM,
	UM_VEC2_KEY_MTU,
	UM_VEC2_KEY_QUEUES,
	UM_VEC2_KEY_GRO,
	UM_VEC2_KEY_GSO,
	UM_VEC2_KEY_CSUM,
	UM_VEC2_KEY_MAC,
	UM_VEC2_KEY_COALESCE_USECS,
	UM_VEC2_KEY_VEC,
	UM_VEC2_KEY_IFNAME,
	UM_VEC2_KEY_SRC,
	UM_VEC2_KEY_DST,
	UM_VEC2_KEY_SRCPORT,
	UM_VEC2_KEY_DSTPORT,
	UM_VEC2_KEY_IFUP,
	UM_VEC2_KEY_BPFFILE,
	UM_VEC2_KEY_V6,
	UM_VEC2_KEY_UDP,
	UM_VEC2_KEY_FD,
	UM_VEC2_KEY_RX_KEY,
	UM_VEC2_KEY_TX_KEY,
	UM_VEC2_KEY_SEQUENCE,
	UM_VEC2_KEY_PIN_SEQUENCE,
	UM_VEC2_KEY_RX_SESSION,
	UM_VEC2_KEY_TX_SESSION,
	UM_VEC2_KEY_COOKIE64,
	UM_VEC2_KEY_RX_COOKIE,
	UM_VEC2_KEY_TX_COOKIE,
	UM_VEC2_KEY_COUNTER,
	UM_VEC2_KEY_PIN_COUNTER,
	UM_VEC2_KEY_VNL,
	UM_VEC2_KEY_DESCR,
	UM_VEC2_KEY_PORT,
	UM_VEC2_KEY_GROUP,
	UM_VEC2_KEY_FAIL_OPEN_AFTER,
	UM_VEC2_KEY_UNKNOWN,
};

struct um_vec2_key_spec {
	const char *name;
	enum um_vec2_key_id id;
	bool trusted_host_only;
};

static const struct um_vec2_key_spec um_vec2_keys[] = {
	{ "transport",		UM_VEC2_KEY_TRANSPORT },
	{ "mode",		UM_VEC2_KEY_MODE },
	{ "depth",		UM_VEC2_KEY_DEPTH },
	{ "headroom",		UM_VEC2_KEY_HEADROOM },
	{ "mtu",		UM_VEC2_KEY_MTU },
	{ "queues",		UM_VEC2_KEY_QUEUES },
	{ "gro",		UM_VEC2_KEY_GRO },
	{ "gso",		UM_VEC2_KEY_GSO },
	{ "csum",		UM_VEC2_KEY_CSUM },
	{ "mac",		UM_VEC2_KEY_MAC },
	{ "coalesce_usecs",	UM_VEC2_KEY_COALESCE_USECS },
	{ "vec",		UM_VEC2_KEY_VEC },
	{ "ifname",		UM_VEC2_KEY_IFNAME, true },
	{ "src",		UM_VEC2_KEY_SRC, true },
	{ "dst",		UM_VEC2_KEY_DST, true },
	{ "srcport",		UM_VEC2_KEY_SRCPORT, true },
	{ "dstport",		UM_VEC2_KEY_DSTPORT, true },
	{ "ifup",		UM_VEC2_KEY_IFUP, true },
	{ "bpffile",		UM_VEC2_KEY_BPFFILE, true },
	{ "v6",			UM_VEC2_KEY_V6 },
	{ "udp",		UM_VEC2_KEY_UDP },
	{ "fd",			UM_VEC2_KEY_FD },
	{ "rx_key",		UM_VEC2_KEY_RX_KEY },
	{ "tx_key",		UM_VEC2_KEY_TX_KEY },
	{ "sequence",		UM_VEC2_KEY_SEQUENCE },
	{ "pin_sequence",	UM_VEC2_KEY_PIN_SEQUENCE },
	{ "rx_session",		UM_VEC2_KEY_RX_SESSION },
	{ "tx_session",		UM_VEC2_KEY_TX_SESSION },
	{ "cookie64",		UM_VEC2_KEY_COOKIE64 },
	{ "rx_cookie",		UM_VEC2_KEY_RX_COOKIE },
	{ "tx_cookie",		UM_VEC2_KEY_TX_COOKIE },
	{ "counter",		UM_VEC2_KEY_COUNTER },
	{ "pin_counter",	UM_VEC2_KEY_PIN_COUNTER },
	{ "vnl",		UM_VEC2_KEY_VNL, true },
	{ "descr",		UM_VEC2_KEY_DESCR, true },
	{ "port",		UM_VEC2_KEY_PORT, true },
	{ "group",		UM_VEC2_KEY_GROUP, true },
	{ "fail_open_after",	UM_VEC2_KEY_FAIL_OPEN_AFTER },
};

static void um_vec2_set_err(struct um_vec2_config_error *err,
			    const char *key, const char *msg)
{
	if (!err)
		return;
	if (key)
		strscpy(err->key, key);
	else
		err->key[0] = '\0';
	strscpy(err->msg, msg);
}

void um_vec2_config_init(struct um_vec2_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->transport = UM_VEC2_TRANSPORT_UNSPEC;
	cfg->mode = UM_VEC2_HOST_AUTO;
	cfg->depth = UM_VEC2_DEFAULT_DEPTH;
	cfg->headroom = UM_VEC2_DEFAULT_HEADROOM;
	cfg->mtu = UM_VEC2_DEFAULT_MTU;
	cfg->queues = UM_VEC2_DEFAULT_QUEUES;
	cfg->batching = true;
}

const char *um_vec2_transport_name(enum um_vec2_transport transport)
{
	switch (transport) {
	case UM_VEC2_TRANSPORT_UNSPEC:
		return "unspec";
	case UM_VEC2_TRANSPORT_TAP:
		return "tap";
	case UM_VEC2_TRANSPORT_RAW:
		return "raw";
	case UM_VEC2_TRANSPORT_GRE:
		return "gre";
	case UM_VEC2_TRANSPORT_L2TPV3:
		return "l2tpv3";
	case UM_VEC2_TRANSPORT_HYBRID:
		return "hybrid";
	case UM_VEC2_TRANSPORT_BESS:
		return "bess";
	case UM_VEC2_TRANSPORT_FD:
		return "fd";
	case UM_VEC2_TRANSPORT_VDE:
		return "vde";
	case UM_VEC2_TRANSPORT_PROXY:
		return "proxy";
	default:
		return "invalid";
	}
}

const char *um_vec2_host_mode_name(enum um_vec2_host_mode mode)
{
	switch (mode) {
	case UM_VEC2_HOST_AUTO:
		return "auto";
	case UM_VEC2_HOST_INPROC:
		return "inproc";
	case UM_VEC2_HOST_FD:
		return "fd";
	case UM_VEC2_HOST_PROXY:
		return "proxy";
	default:
		return "invalid";
	}
}

static const struct um_vec2_key_spec *um_vec2_key_lookup(const char *key)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(um_vec2_keys); i++) {
		if (!strcmp(key, um_vec2_keys[i].name))
			return &um_vec2_keys[i];
	}

	return NULL;
}

static int um_vec2_copy_value(char *dst, size_t dst_len, const char *key,
			      const char *value, struct um_vec2_config_error *err)
{
	if (strscpy(dst, value, dst_len) < 0) {
		um_vec2_set_err(err, key, "value too long");
		return -E2BIG;
	}
	return 0;
}

static int um_vec2_parse_uint_range(const char *key, const char *value,
				    unsigned int min, unsigned int max,
				    unsigned int *out,
				    struct um_vec2_config_error *err)
{
	unsigned int parsed;
	int ret;

	ret = kstrtouint(value, 0, &parsed);
	if (ret) {
		um_vec2_set_err(err, key, "invalid unsigned integer");
		return ret;
	}
	if (parsed < min || parsed > max) {
		um_vec2_set_err(err, key, "integer out of range");
		return -ERANGE;
	}
	*out = parsed;
	return 0;
}

static int um_vec2_parse_u64(const char *key, const char *value, u64 *out,
			     struct um_vec2_config_error *err)
{
	int ret = kstrtou64(value, 0, out);

	if (ret)
		um_vec2_set_err(err, key, "invalid unsigned integer");
	return ret;
}

static int um_vec2_parse_bool(const char *key, const char *value,
			      bool compat, bool *out,
			      struct um_vec2_config_error *err)
{
	unsigned int parsed;
	int ret;

	ret = kstrtouint(value, 0, &parsed);
	if (ret) {
		um_vec2_set_err(err, key, "invalid boolean");
		return ret;
	}
	if (compat) {
		*out = parsed > 0;
		return 0;
	}
	if (parsed > 1) {
		um_vec2_set_err(err, key, "boolean must be 0 or 1");
		return -ERANGE;
	}
	*out = parsed == 1;
	return 0;
}

static int um_vec2_parse_transport(const char *key, const char *value,
				   struct um_vec2_config *cfg,
				   struct um_vec2_config_error *err)
{
	if (!strcmp(value, "tap")) {
		cfg->transport = UM_VEC2_TRANSPORT_TAP;
	} else if (!strcmp(value, "raw")) {
		cfg->transport = UM_VEC2_TRANSPORT_RAW;
	} else if (!strcmp(value, "gre")) {
		cfg->transport = UM_VEC2_TRANSPORT_GRE;
	} else if (!strcmp(value, "l2tpv3")) {
		cfg->transport = UM_VEC2_TRANSPORT_L2TPV3;
	} else if (!strcmp(value, "hybrid")) {
		cfg->transport = UM_VEC2_TRANSPORT_HYBRID;
	} else if (!strcmp(value, "bess")) {
		cfg->transport = UM_VEC2_TRANSPORT_BESS;
	} else if (!strcmp(value, "fd")) {
		cfg->transport = UM_VEC2_TRANSPORT_FD;
	} else if (!strcmp(value, "vde")) {
		cfg->transport = UM_VEC2_TRANSPORT_VDE;
	} else if (!strcmp(value, "proxy")) {
		cfg->transport = UM_VEC2_TRANSPORT_PROXY;
	} else {
		um_vec2_set_err(err, key, "unknown transport");
		return -EINVAL;
	}
	return 0;
}

static int um_vec2_parse_mode(const char *key, const char *value,
			      struct um_vec2_config *cfg,
			      struct um_vec2_config_error *err)
{
	if (!strcmp(value, "auto")) {
		cfg->mode = UM_VEC2_HOST_AUTO;
	} else if (!strcmp(value, "inproc")) {
		cfg->mode = UM_VEC2_HOST_INPROC;
	} else if (!strcmp(value, "fd")) {
		cfg->mode = UM_VEC2_HOST_FD;
	} else if (!strcmp(value, "proxy")) {
		cfg->mode = UM_VEC2_HOST_PROXY;
	} else {
		um_vec2_set_err(err, key, "unknown host mode");
		return -EINVAL;
	}
	return 0;
}

static int um_vec2_parse_mac(const char *key, const char *value,
			     struct um_vec2_config *cfg,
			     struct um_vec2_config_error *err)
{
	u8 addr[ETH_ALEN];

	if (!mac_pton(value, addr)) {
		um_vec2_set_err(err, key, "invalid ethernet address");
		return -EINVAL;
	}
	if (!is_valid_ether_addr(addr)) {
		um_vec2_set_err(err, key, "ethernet address is not unicast");
		return -EINVAL;
	}
	ether_addr_copy(cfg->mac, addr);
	cfg->has_mac = true;
	return 0;
}

static int um_vec2_parse_present_bool(const char *key, const char *value,
				      bool compat, bool *out, bool *present,
				      struct um_vec2_config_error *err)
{
	int ret;

	ret = um_vec2_parse_bool(key, value, compat, out, err);
	*present = ret == 0;
	return ret;
}

static int um_vec2_parse_present_uint(const char *key, const char *value,
				      unsigned int max, unsigned int *out,
				      bool *present,
				      struct um_vec2_config_error *err)
{
	int ret;

	ret = um_vec2_parse_uint_range(key, value, 0, max, out, err);
	*present = ret == 0;
	return ret;
}

static int um_vec2_parse_present_u64(const char *key, const char *value,
				     u64 *out, bool *present,
				     struct um_vec2_config_error *err)
{
	int ret;

	ret = um_vec2_parse_u64(key, value, out, err);
	*present = ret == 0;
	return ret;
}

static int um_vec2_parse_core_value(enum um_vec2_key_id id, const char *key,
				    const char *value, bool compat,
				    struct um_vec2_config *cfg,
				    struct um_vec2_config_error *err)
{
	switch (id) {
	case UM_VEC2_KEY_TRANSPORT:
		return um_vec2_parse_transport(key, value, cfg, err);
	case UM_VEC2_KEY_MODE:
		return um_vec2_parse_mode(key, value, cfg, err);
	case UM_VEC2_KEY_DEPTH:
		return um_vec2_parse_uint_range(key, value, 1,
						UM_VEC2_MAX_DEPTH,
						&cfg->depth, err);
	case UM_VEC2_KEY_HEADROOM:
		return um_vec2_parse_uint_range(key, value, 0,
						UM_VEC2_MAX_HEADROOM,
						&cfg->headroom, err);
	case UM_VEC2_KEY_MTU:
		return um_vec2_parse_uint_range(key, value, UM_VEC2_MIN_MTU,
						UM_VEC2_MAX_MTU,
						&cfg->mtu, err);
	case UM_VEC2_KEY_QUEUES:
		return um_vec2_parse_uint_range(key, value, 1,
						UM_VEC2_MAX_QUEUES,
						&cfg->queues, err);
	case UM_VEC2_KEY_GRO:
		return um_vec2_parse_bool(key, value, compat, &cfg->gro, err);
	case UM_VEC2_KEY_GSO:
		return um_vec2_parse_bool(key, value, compat, &cfg->gso, err);
	case UM_VEC2_KEY_CSUM:
		return um_vec2_parse_bool(key, value, compat, &cfg->csum, err);
	case UM_VEC2_KEY_MAC:
		return um_vec2_parse_mac(key, value, cfg, err);
	case UM_VEC2_KEY_COALESCE_USECS:
		return um_vec2_parse_uint_range(key, value, 0, UINT_MAX,
						&cfg->coalesce_usecs, err);
	default:
		return -ENOENT;
	}
}

static int um_vec2_apply_vec_compat(const char *key, const char *value,
				    struct um_vec2_config *cfg,
				    struct um_vec2_config_error *err)
{
	unsigned int parsed;
	int ret;

	ret = um_vec2_parse_uint_range(key, value, 0, 1, &parsed, err);
	if (ret)
		return ret;

	cfg->batching = parsed != 0;
	if (!cfg->batching)
		cfg->depth = 1;
	return 0;
}

static int um_vec2_parse_host_value(enum um_vec2_key_id id, const char *key,
				    const char *value,
				    struct um_vec2_config *cfg,
				    struct um_vec2_config_error *err)
{
	switch (id) {
	case UM_VEC2_KEY_IFNAME:
		return um_vec2_copy_value(cfg->ifname, sizeof(cfg->ifname),
					  key, value, err);
	case UM_VEC2_KEY_SRC:
		return um_vec2_copy_value(cfg->src, sizeof(cfg->src),
					  key, value, err);
	case UM_VEC2_KEY_DST:
		return um_vec2_copy_value(cfg->dst, sizeof(cfg->dst),
					  key, value, err);
	case UM_VEC2_KEY_SRCPORT:
		return um_vec2_copy_value(cfg->srcport, sizeof(cfg->srcport),
					  key, value, err);
	case UM_VEC2_KEY_DSTPORT:
		return um_vec2_copy_value(cfg->dstport, sizeof(cfg->dstport),
					  key, value, err);
	case UM_VEC2_KEY_IFUP:
		return um_vec2_copy_value(cfg->ifup, sizeof(cfg->ifup),
					  key, value, err);
	case UM_VEC2_KEY_BPFFILE:
		return um_vec2_copy_value(cfg->bpffile, sizeof(cfg->bpffile),
					  key, value, err);
	case UM_VEC2_KEY_VNL:
		return um_vec2_copy_value(cfg->vnl, sizeof(cfg->vnl),
					  key, value, err);
	case UM_VEC2_KEY_DESCR:
		return um_vec2_copy_value(cfg->descr, sizeof(cfg->descr),
					  key, value, err);
	case UM_VEC2_KEY_PORT:
		return um_vec2_copy_value(cfg->port, sizeof(cfg->port),
					  key, value, err);
	case UM_VEC2_KEY_GROUP:
		return um_vec2_copy_value(cfg->group, sizeof(cfg->group),
					  key, value, err);
	default:
		return -ENOENT;
	}
}

static int um_vec2_parse_transport_presence(enum um_vec2_key_id id,
					    const char *key, const char *value,
					    bool compat,
					    struct um_vec2_config *cfg,
					    struct um_vec2_config_error *err)
{
	switch (id) {
	case UM_VEC2_KEY_V6:
		return um_vec2_parse_present_bool(key, value, compat,
						  &cfg->v6, &cfg->has_v6,
						  err);
	case UM_VEC2_KEY_UDP:
		return um_vec2_parse_present_bool(key, value, compat,
						  &cfg->udp, &cfg->has_udp,
						  err);
	default:
		return -ENOENT;
	}
}

static int um_vec2_parse_transport_fd(enum um_vec2_key_id id, const char *key,
				      const char *value,
				      struct um_vec2_config *cfg,
				      struct um_vec2_config_error *err)
{
	switch (id) {
	case UM_VEC2_KEY_FD:
		return um_vec2_parse_present_uint(key, value, INT_MAX,
						  &cfg->fd, &cfg->has_fd,
						  err);
	case UM_VEC2_KEY_FAIL_OPEN_AFTER:
		return um_vec2_parse_uint_range(key, value, 0, UINT_MAX,
						&cfg->fail_open_after, err);
	default:
		return -ENOENT;
	}
}

static int um_vec2_parse_transport_keys(enum um_vec2_key_id id,
					const char *key, const char *value,
					struct um_vec2_config *cfg,
					struct um_vec2_config_error *err)
{
	switch (id) {
	case UM_VEC2_KEY_RX_KEY:
		return um_vec2_parse_present_uint(key, value, UINT_MAX,
						  &cfg->rx_key,
						  &cfg->has_rx_key, err);
	case UM_VEC2_KEY_TX_KEY:
		return um_vec2_parse_present_uint(key, value, UINT_MAX,
						  &cfg->tx_key,
						  &cfg->has_tx_key, err);
	default:
		return -ENOENT;
	}
}

static int um_vec2_parse_transport_sequence(enum um_vec2_key_id id,
					    const char *key, const char *value,
					    bool compat,
					    struct um_vec2_config *cfg,
					    struct um_vec2_config_error *err)
{
	switch (id) {
	case UM_VEC2_KEY_SEQUENCE:
		return um_vec2_parse_bool(key, value, compat,
					  &cfg->sequence, err);
	case UM_VEC2_KEY_PIN_SEQUENCE:
		return um_vec2_parse_bool(key, value, compat,
					  &cfg->pin_sequence, err);
	default:
		return -ENOENT;
	}
}

static int um_vec2_parse_transport_sessions(enum um_vec2_key_id id,
					    const char *key, const char *value,
					    struct um_vec2_config *cfg,
					    struct um_vec2_config_error *err)
{
	switch (id) {
	case UM_VEC2_KEY_RX_SESSION:
		return um_vec2_parse_present_uint(key, value, UINT_MAX,
						  &cfg->rx_session,
						  &cfg->has_rx_session,
						  err);
	case UM_VEC2_KEY_TX_SESSION:
		return um_vec2_parse_present_uint(key, value, UINT_MAX,
						  &cfg->tx_session,
						  &cfg->has_tx_session,
						  err);
	default:
		return -ENOENT;
	}
}

static int um_vec2_parse_transport_cookies(enum um_vec2_key_id id,
					   const char *key, const char *value,
					   bool compat,
					   struct um_vec2_config *cfg,
					   struct um_vec2_config_error *err)
{
	switch (id) {
	case UM_VEC2_KEY_COOKIE64:
		return um_vec2_parse_bool(key, value, compat,
					  &cfg->cookie64, err);
	case UM_VEC2_KEY_RX_COOKIE:
		return um_vec2_parse_present_u64(key, value,
						 &cfg->rx_cookie,
						 &cfg->has_rx_cookie, err);
	case UM_VEC2_KEY_TX_COOKIE:
		return um_vec2_parse_present_u64(key, value,
						 &cfg->tx_cookie,
						 &cfg->has_tx_cookie, err);
	default:
		return -ENOENT;
	}
}

static int um_vec2_parse_transport_counter(enum um_vec2_key_id id,
					   const char *key, const char *value,
					   bool compat,
					   struct um_vec2_config *cfg,
					   struct um_vec2_config_error *err)
{
	switch (id) {
	case UM_VEC2_KEY_COUNTER:
		return um_vec2_parse_bool(key, value, compat,
					  &cfg->counter, err);
	case UM_VEC2_KEY_PIN_COUNTER:
		return um_vec2_parse_bool(key, value, compat,
					  &cfg->pin_counter, err);
	default:
		return -ENOENT;
	}
}

static int um_vec2_parse_transport_value(enum um_vec2_key_id id,
					 const char *key, const char *value,
					 bool compat,
					 struct um_vec2_config *cfg,
					 struct um_vec2_config_error *err)
{
	int ret;

	ret = um_vec2_parse_transport_presence(id, key, value, compat, cfg, err);
	if (ret != -ENOENT)
		return ret;
	ret = um_vec2_parse_transport_fd(id, key, value, cfg, err);
	if (ret != -ENOENT)
		return ret;
	ret = um_vec2_parse_transport_keys(id, key, value, cfg, err);
	if (ret != -ENOENT)
		return ret;
	ret = um_vec2_parse_transport_sequence(id, key, value, compat, cfg, err);
	if (ret != -ENOENT)
		return ret;
	ret = um_vec2_parse_transport_sessions(id, key, value, cfg, err);
	if (ret != -ENOENT)
		return ret;
	ret = um_vec2_parse_transport_cookies(id, key, value, compat, cfg, err);
	if (ret != -ENOENT)
		return ret;
	return um_vec2_parse_transport_counter(id, key, value, compat, cfg, err);
}

static int um_vec2_parse_value(enum um_vec2_key_id id, const char *key,
			       const char *value, bool compat,
			       struct um_vec2_config *cfg,
			       struct um_vec2_config_error *err)
{
	int ret;

	if (id == UM_VEC2_KEY_VEC)
		return um_vec2_apply_vec_compat(key, value, cfg, err);

	ret = um_vec2_parse_core_value(id, key, value, compat, cfg, err);
	if (ret != -ENOENT)
		return ret;

	ret = um_vec2_parse_host_value(id, key, value, cfg, err);
	if (ret != -ENOENT)
		return ret;

	ret = um_vec2_parse_transport_value(id, key, value, compat, cfg, err);
	if (ret != -ENOENT)
		return ret;

	um_vec2_set_err(err, key, "unknown key");
	return -EINVAL;
}

static int um_vec2_parse_token(char *token, bool compat, bool trusted,
			       u64 *seen, struct um_vec2_config *cfg,
			       struct um_vec2_config_error *err)
{
	const struct um_vec2_key_spec *spec;
	char *value;

	if (!*token) {
		um_vec2_set_err(err, NULL, "empty token");
		return -EINVAL;
	}

	value = strchr(token, '=');
	if (!value || value == token || value[1] == '\0') {
		um_vec2_set_err(err, token, "expected key=value");
		return -EINVAL;
	}
	*value++ = '\0';

	spec = um_vec2_key_lookup(token);
	if (!spec) {
		if (compat)
			return 0;
		um_vec2_set_err(err, token, "unknown key");
		return -EINVAL;
	}

	if (*seen & BIT_ULL(spec->id)) {
		um_vec2_set_err(err, token, "duplicate key");
		return -EEXIST;
	}
	*seen |= BIT_ULL(spec->id);

	if (!trusted && spec->trusted_host_only) {
		um_vec2_set_err(err, token,
				"trusted host option not permitted");
		return -EACCES;
	}

	return um_vec2_parse_value(spec->id, token, value, compat, cfg, err);
}

static int um_vec2_config_validate(struct um_vec2_config *cfg,
				   struct um_vec2_config_error *err)
{
	if (cfg->transport == UM_VEC2_TRANSPORT_UNSPEC) {
		um_vec2_set_err(err, "transport", "missing transport");
		return -EINVAL;
	}

	if (cfg->transport == UM_VEC2_TRANSPORT_FD && !cfg->has_fd) {
		um_vec2_set_err(err, "fd", "fd transport requires fd");
		return -EINVAL;
	}

	if (cfg->has_rx_key != cfg->has_tx_key) {
		um_vec2_set_err(err, "rx_key", "rx_key and tx_key must pair");
		return -EINVAL;
	}

	if (cfg->has_rx_session != cfg->has_tx_session) {
		um_vec2_set_err(err, "rx_session",
				"rx_session and tx_session must pair");
		return -EINVAL;
	}

	if (cfg->has_rx_cookie != cfg->has_tx_cookie) {
		um_vec2_set_err(err, "rx_cookie",
				"rx_cookie and tx_cookie must pair");
		return -EINVAL;
	}

	return 0;
}

int um_vec2_config_parse(const char *spec, unsigned int flags,
			 struct um_vec2_config *cfg,
			 struct um_vec2_config_error *err)
{
	bool compat = flags & UM_VEC2_PARSE_COMPAT;
	bool trusted = flags & UM_VEC2_PARSE_TRUSTED_HOST;
	char *work, *cursor, *token;
	u64 seen = 0;
	int ret = 0;

	if (err)
		memset(err, 0, sizeof(*err));
	if (!cfg) {
		um_vec2_set_err(err, NULL, "missing output config");
		return -EINVAL;
	}
	um_vec2_config_init(cfg);
	if (!spec || !*spec) {
		um_vec2_set_err(err, NULL, "empty config");
		return -EINVAL;
	}

	work = kstrdup(spec, GFP_KERNEL);
	if (!work) {
		um_vec2_set_err(err, NULL, "allocation failed");
		return -ENOMEM;
	}

	cursor = work;
	while ((token = strsep(&cursor, ",")) != NULL) {
		ret = um_vec2_parse_token(token, compat, trusted, &seen,
					  cfg, err);
		if (ret)
			goto out;
	}

	ret = um_vec2_config_validate(cfg, err);

out:
	kfree(work);
	return ret;
}
