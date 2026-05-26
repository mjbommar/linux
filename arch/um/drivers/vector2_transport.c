// SPDX-License-Identifier: GPL-2.0
/*
 * Bounds-checked transport header helpers for UML vector networking v2.
 */

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "vector2_transport.h"

static bool um_vec2_range_ok(size_t len, size_t offset, size_t size)
{
	return offset <= len && size <= len - offset;
}

static int um_vec2_put_be16(void *buf, size_t len, size_t offset, u16 value)
{
	u8 *data = buf;

	if (!um_vec2_range_ok(len, offset, sizeof(value)))
		return -EMSGSIZE;

	put_unaligned_be16(value, data + offset);
	return 0;
}

static int um_vec2_get_be16(const void *buf, size_t len, size_t offset,
			    u16 *value)
{
	const u8 *data = buf;

	if (!um_vec2_range_ok(len, offset, sizeof(*value)))
		return -EMSGSIZE;

	*value = get_unaligned_be16(data + offset);
	return 0;
}

static int um_vec2_put_be32(void *buf, size_t len, size_t offset, u32 value)
{
	u8 *data = buf;

	if (!um_vec2_range_ok(len, offset, sizeof(value)))
		return -EMSGSIZE;

	put_unaligned_be32(value, data + offset);
	return 0;
}

static int um_vec2_get_be32(const void *buf, size_t len, size_t offset,
			    u32 *value)
{
	const u8 *data = buf;

	if (!um_vec2_range_ok(len, offset, sizeof(*value)))
		return -EMSGSIZE;

	*value = get_unaligned_be32(data + offset);
	return 0;
}

static int um_vec2_put_be64(void *buf, size_t len, size_t offset, u64 value)
{
	u8 *data = buf;

	if (!um_vec2_range_ok(len, offset, sizeof(value)))
		return -EMSGSIZE;

	put_unaligned_be64(value, data + offset);
	return 0;
}

static int um_vec2_get_be64(const void *buf, size_t len, size_t offset,
			    u64 *value)
{
	const u8 *data = buf;

	if (!um_vec2_range_ok(len, offset, sizeof(*value)))
		return -EMSGSIZE;

	*value = get_unaligned_be64(data + offset);
	return 0;
}

size_t um_vec2_gre_header_len(const struct um_vec2_gre_spec *spec)
{
	size_t len = 4;

	if (spec->has_key)
		len += 4;
	if (spec->has_sequence)
		len += 4;

	return len;
}

static u16 um_vec2_gre_flags(const struct um_vec2_gre_spec *spec)
{
	u16 flags = 0;

	if (spec->has_key)
		flags |= UM_VEC2_GRE_FLAG_KEY;
	if (spec->has_sequence)
		flags |= UM_VEC2_GRE_FLAG_SEQ;

	return flags;
}

int um_vec2_gre_build(const struct um_vec2_gre_spec *spec, void *buf,
		      size_t len, u32 sequence)
{
	size_t need = um_vec2_gre_header_len(spec);
	size_t offset = 0;
	int err;

	if (!buf || len < need)
		return -EMSGSIZE;

	err = um_vec2_put_be16(buf, len, offset, um_vec2_gre_flags(spec));
	if (err)
		return err;
	offset += 2;

	err = um_vec2_put_be16(buf, len, offset, UM_VEC2_GRE_PROTO_TEB);
	if (err)
		return err;
	offset += 2;

	if (spec->has_key) {
		err = um_vec2_put_be32(buf, len, offset, spec->tx_key);
		if (err)
			return err;
		offset += 4;
	}

	if (spec->has_sequence) {
		if (spec->pin_sequence)
			sequence = 0;
		err = um_vec2_put_be32(buf, len, offset, sequence);
		if (err)
			return err;
	}

	return 0;
}

int um_vec2_gre_parse(const struct um_vec2_gre_spec *spec, const void *buf,
		      size_t len, struct um_vec2_gre_rx *rx)
{
	size_t need = um_vec2_gre_header_len(spec);
	size_t offset = 0;
	u16 flags;
	u16 proto;
	u32 key;
	int err;

	if (!buf || len < need)
		return -EMSGSIZE;

	if (rx)
		memset(rx, 0, sizeof(*rx));

	err = um_vec2_get_be16(buf, len, offset, &flags);
	if (err)
		return err;
	offset += 2;

	err = um_vec2_get_be16(buf, len, offset, &proto);
	if (err)
		return err;
	offset += 2;

	if (flags != um_vec2_gre_flags(spec) || proto != UM_VEC2_GRE_PROTO_TEB)
		return -EPROTO;

	if (spec->has_key) {
		err = um_vec2_get_be32(buf, len, offset, &key);
		if (err)
			return err;
		if (key != spec->rx_key)
			return -EPROTO;
		offset += 4;
	}

	if (spec->has_sequence && rx) {
		err = um_vec2_get_be32(buf, len, offset, &rx->sequence);
		if (err)
			return err;
		rx->has_sequence = true;
	}

	return 0;
}

size_t um_vec2_l2tpv3_header_len(const struct um_vec2_l2tpv3_spec *spec)
{
	size_t len = 4;

	if (spec->udp)
		len += 4;
	if (spec->has_cookie)
		len += spec->cookie64 ? 8 : 4;
	if (spec->has_counter)
		len += 4;

	return len;
}

int um_vec2_l2tpv3_build(const struct um_vec2_l2tpv3_spec *spec, void *buf,
			 size_t len, u32 counter)
{
	size_t need = um_vec2_l2tpv3_header_len(spec);
	size_t offset = 0;
	int err;

	if (!buf || len < need)
		return -EMSGSIZE;

	if (spec->udp) {
		err = um_vec2_put_be32(buf, len, offset,
				       UM_VEC2_L2TPV3_DATA_PACKET);
		if (err)
			return err;
		offset += 4;
	}

	err = um_vec2_put_be32(buf, len, offset, spec->tx_session);
	if (err)
		return err;
	offset += 4;

	if (spec->has_cookie) {
		if (spec->cookie64)
			err = um_vec2_put_be64(buf, len, offset,
					       spec->tx_cookie);
		else
			err = um_vec2_put_be32(buf, len, offset,
					       spec->tx_cookie);
		if (err)
			return err;
		offset += spec->cookie64 ? 8 : 4;
	}

	if (spec->has_counter) {
		if (spec->pin_counter)
			counter = 0;
		err = um_vec2_put_be32(buf, len, offset, counter);
		if (err)
			return err;
	}

	return 0;
}

int um_vec2_l2tpv3_parse(const struct um_vec2_l2tpv3_spec *spec,
			 const void *buf, size_t len,
			 struct um_vec2_l2tpv3_rx *rx)
{
	size_t need = um_vec2_l2tpv3_header_len(spec);
	size_t offset = 0;
	u64 cookie64;
	u32 value;
	int err;

	if (!buf || len < need)
		return -EMSGSIZE;

	if (rx)
		memset(rx, 0, sizeof(*rx));

	if (spec->udp) {
		err = um_vec2_get_be32(buf, len, offset, &value);
		if (err)
			return err;
		if (value != UM_VEC2_L2TPV3_DATA_PACKET)
			return -EPROTO;
		offset += 4;
	}

	err = um_vec2_get_be32(buf, len, offset, &value);
	if (err)
		return err;
	if (value != spec->rx_session)
		return -EPROTO;
	offset += 4;

	if (spec->has_cookie) {
		if (spec->cookie64) {
			err = um_vec2_get_be64(buf, len, offset, &cookie64);
			if (err)
				return err;
			if (cookie64 != spec->rx_cookie)
				return -EPROTO;
			offset += 8;
		} else {
			err = um_vec2_get_be32(buf, len, offset, &value);
			if (err)
				return err;
			if (value != (u32)spec->rx_cookie)
				return -EPROTO;
			offset += 4;
		}
	}

	if (spec->has_counter && rx) {
		err = um_vec2_get_be32(buf, len, offset, &rx->counter);
		if (err)
			return err;
		rx->has_counter = true;
	}

	return 0;
}
