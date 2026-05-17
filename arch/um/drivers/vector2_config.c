// SPDX-License-Identifier: GPL-2.0
/*
 * Typed configuration parser for UML vector networking v2.
 *
 * The legacy vector driver parses option strings by destructively
 * splitting them into parallel token/value arrays.  This parser is the
 * first v2 building block: validate at the boundary, then let the rest
 * of the driver consume explicit state.
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

static enum um_vec2_key_id um_vec2_key_id(const char *key)
{
	if (!strcmp(key, "transport"))
		return UM_VEC2_KEY_TRANSPORT;
	if (!strcmp(key, "mode"))
		return UM_VEC2_KEY_MODE;
	if (!strcmp(key, "depth"))
		return UM_VEC2_KEY_DEPTH;
	if (!strcmp(key, "headroom"))
		return UM_VEC2_KEY_HEADROOM;
	if (!strcmp(key, "mtu"))
		return UM_VEC2_KEY_MTU;
	if (!strcmp(key, "queues"))
		return UM_VEC2_KEY_QUEUES;
	if (!strcmp(key, "gro"))
		return UM_VEC2_KEY_GRO;
	if (!strcmp(key, "gso"))
		return UM_VEC2_KEY_GSO;
	if (!strcmp(key, "csum"))
		return UM_VEC2_KEY_CSUM;
	if (!strcmp(key, "mac"))
		return UM_VEC2_KEY_MAC;
	if (!strcmp(key, "coalesce_usecs"))
		return UM_VEC2_KEY_COALESCE_USECS;
	if (!strcmp(key, "vec"))
		return UM_VEC2_KEY_VEC;
	if (!strcmp(key, "ifname"))
		return UM_VEC2_KEY_IFNAME;
	if (!strcmp(key, "src"))
		return UM_VEC2_KEY_SRC;
	if (!strcmp(key, "dst"))
		return UM_VEC2_KEY_DST;
	if (!strcmp(key, "srcport"))
		return UM_VEC2_KEY_SRCPORT;
	if (!strcmp(key, "dstport"))
		return UM_VEC2_KEY_DSTPORT;
	if (!strcmp(key, "ifup"))
		return UM_VEC2_KEY_IFUP;
	if (!strcmp(key, "bpffile"))
		return UM_VEC2_KEY_BPFFILE;
	if (!strcmp(key, "v6"))
		return UM_VEC2_KEY_V6;
	if (!strcmp(key, "udp"))
		return UM_VEC2_KEY_UDP;
	if (!strcmp(key, "fd"))
		return UM_VEC2_KEY_FD;
	if (!strcmp(key, "rx_key"))
		return UM_VEC2_KEY_RX_KEY;
	if (!strcmp(key, "tx_key"))
		return UM_VEC2_KEY_TX_KEY;
	if (!strcmp(key, "sequence"))
		return UM_VEC2_KEY_SEQUENCE;
	if (!strcmp(key, "pin_sequence"))
		return UM_VEC2_KEY_PIN_SEQUENCE;
	if (!strcmp(key, "rx_session"))
		return UM_VEC2_KEY_RX_SESSION;
	if (!strcmp(key, "tx_session"))
		return UM_VEC2_KEY_TX_SESSION;
	if (!strcmp(key, "cookie64"))
		return UM_VEC2_KEY_COOKIE64;
	if (!strcmp(key, "rx_cookie"))
		return UM_VEC2_KEY_RX_COOKIE;
	if (!strcmp(key, "tx_cookie"))
		return UM_VEC2_KEY_TX_COOKIE;
	if (!strcmp(key, "counter"))
		return UM_VEC2_KEY_COUNTER;
	if (!strcmp(key, "pin_counter"))
		return UM_VEC2_KEY_PIN_COUNTER;
	if (!strcmp(key, "vnl"))
		return UM_VEC2_KEY_VNL;
	if (!strcmp(key, "descr"))
		return UM_VEC2_KEY_DESCR;
	if (!strcmp(key, "port"))
		return UM_VEC2_KEY_PORT;
	if (!strcmp(key, "group"))
		return UM_VEC2_KEY_GROUP;
	if (!strcmp(key, "fail_open_after"))
		return UM_VEC2_KEY_FAIL_OPEN_AFTER;
	return UM_VEC2_KEY_UNKNOWN;
}

static bool um_vec2_key_trusted_host_only(enum um_vec2_key_id key)
{
	switch (key) {
	case UM_VEC2_KEY_IFNAME:
	case UM_VEC2_KEY_SRC:
	case UM_VEC2_KEY_DST:
	case UM_VEC2_KEY_SRCPORT:
	case UM_VEC2_KEY_DSTPORT:
	case UM_VEC2_KEY_IFUP:
	case UM_VEC2_KEY_BPFFILE:
	case UM_VEC2_KEY_VNL:
	case UM_VEC2_KEY_DESCR:
	case UM_VEC2_KEY_PORT:
	case UM_VEC2_KEY_GROUP:
		return true;
	default:
		return false;
	}
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

static int um_vec2_parse_value(enum um_vec2_key_id id, const char *key,
			       const char *value, bool compat,
			       struct um_vec2_config *cfg,
			       struct um_vec2_config_error *err)
{
	unsigned int parsed;
	int ret;

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
	case UM_VEC2_KEY_VEC:
		ret = um_vec2_parse_uint_range(key, value, 0, 1, &parsed, err);
		if (ret)
			return ret;
		cfg->batching = parsed != 0;
		if (!cfg->batching)
			cfg->depth = 1;
		return 0;
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
	case UM_VEC2_KEY_V6:
		ret = um_vec2_parse_bool(key, value, compat, &cfg->v6, err);
		cfg->has_v6 = ret == 0;
		return ret;
	case UM_VEC2_KEY_UDP:
		ret = um_vec2_parse_bool(key, value, compat, &cfg->udp, err);
		cfg->has_udp = ret == 0;
		return ret;
	case UM_VEC2_KEY_FD:
		ret = um_vec2_parse_uint_range(key, value, 0, INT_MAX,
					       &cfg->fd, err);
		cfg->has_fd = ret == 0;
		return ret;
	case UM_VEC2_KEY_RX_KEY:
		ret = um_vec2_parse_uint_range(key, value, 0, UINT_MAX,
					       &cfg->rx_key, err);
		cfg->has_rx_key = ret == 0;
		return ret;
	case UM_VEC2_KEY_TX_KEY:
		ret = um_vec2_parse_uint_range(key, value, 0, UINT_MAX,
					       &cfg->tx_key, err);
		cfg->has_tx_key = ret == 0;
		return ret;
	case UM_VEC2_KEY_SEQUENCE:
		return um_vec2_parse_bool(key, value, compat,
					  &cfg->sequence, err);
	case UM_VEC2_KEY_PIN_SEQUENCE:
		return um_vec2_parse_bool(key, value, compat,
					  &cfg->pin_sequence, err);
	case UM_VEC2_KEY_RX_SESSION:
		ret = um_vec2_parse_uint_range(key, value, 0, UINT_MAX,
					       &cfg->rx_session, err);
		cfg->has_rx_session = ret == 0;
		return ret;
	case UM_VEC2_KEY_TX_SESSION:
		ret = um_vec2_parse_uint_range(key, value, 0, UINT_MAX,
					       &cfg->tx_session, err);
		cfg->has_tx_session = ret == 0;
		return ret;
	case UM_VEC2_KEY_COOKIE64:
		return um_vec2_parse_bool(key, value, compat,
					  &cfg->cookie64, err);
	case UM_VEC2_KEY_RX_COOKIE:
		ret = um_vec2_parse_u64(key, value, &cfg->rx_cookie, err);
		cfg->has_rx_cookie = ret == 0;
		return ret;
	case UM_VEC2_KEY_TX_COOKIE:
		ret = um_vec2_parse_u64(key, value, &cfg->tx_cookie, err);
		cfg->has_tx_cookie = ret == 0;
		return ret;
	case UM_VEC2_KEY_COUNTER:
		return um_vec2_parse_bool(key, value, compat,
					  &cfg->counter, err);
	case UM_VEC2_KEY_PIN_COUNTER:
		return um_vec2_parse_bool(key, value, compat,
					  &cfg->pin_counter, err);
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
	case UM_VEC2_KEY_FAIL_OPEN_AFTER:
		return um_vec2_parse_uint_range(key, value, 0, UINT_MAX,
						&cfg->fail_open_after, err);
	default:
		um_vec2_set_err(err, key, "unknown key");
		return -EINVAL;
	}
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
		enum um_vec2_key_id id;
		char *value;

		if (!*token) {
			um_vec2_set_err(err, NULL, "empty token");
			ret = -EINVAL;
			goto out;
		}

		value = strchr(token, '=');
		if (!value || value == token || value[1] == '\0') {
			um_vec2_set_err(err, token, "expected key=value");
			ret = -EINVAL;
			goto out;
		}
		*value++ = '\0';
		id = um_vec2_key_id(token);
		if (id == UM_VEC2_KEY_UNKNOWN) {
			if (compat)
				continue;
			um_vec2_set_err(err, token, "unknown key");
			ret = -EINVAL;
			goto out;
		}
		if (seen & BIT_ULL(id)) {
			um_vec2_set_err(err, token, "duplicate key");
			ret = -EEXIST;
			goto out;
		}
		seen |= BIT_ULL(id);

		if (!trusted && um_vec2_key_trusted_host_only(id)) {
			um_vec2_set_err(err, token,
					"trusted host option not permitted");
			ret = -EACCES;
			goto out;
		}

		ret = um_vec2_parse_value(id, token, value, compat, cfg, err);
		if (ret)
			goto out;
	}

	ret = um_vec2_config_validate(cfg, err);

out:
	kfree(work);
	return ret;
}
