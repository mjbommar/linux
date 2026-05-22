// SPDX-License-Identifier: GPL-2.0
/*
 * UML template-pause identity application (Memo 09 Phase 2).
 *
 * Apply the identity blob delivered by the supervisor (instance MAC,
 * IPv4 CIDR, IPv4 gateway) to in-guest kernel state.  Phase 1a only
 * read + logged the blob; Phase 2 (this TU) actually mutates the
 * netdev MAC and rebinds the IPv4 address.
 *
 * Architectural choice (design memo 2026-05-21):
 *
 *   The IDENTITY APPLY happens in the MASTER (parent) of the fork-on-
 *   resume loop, AFTER reading the per-take blob and BEFORE forking
 *   for that take.  The forked child inherits the applied identity via
 *   CoW.  This sidesteps the bug-B1 hazard zone (the M-fork child runs
 *   on a private stack and cannot safely re-enter kernel C code that
 *   touches shared physmem) — kernel C helpers like dev_set_mac_address
 *   and devinet_ioctl take RTNL and walk net-global data structures,
 *   which the post-fork child is not safe to do.
 *
 *   Cost: the master's own netdev identity drifts across iterations
 *   (it always carries the LAST applied blob).  The master is never
 *   exposed as a pool member, so this is invisible to consumers.  Each
 *   `apply` is idempotent — re-applying the same blob is a no-op, and
 *   applying a NEW blob cleanly replaces the previous one.
 *
 *   Tap-name semantics: blob->tap_name is the HOST-side TAP device
 *   name (created by the supervisor via `ip tuntap add`).  The
 *   IN-GUEST netdev that the supervisor's TAP is attached to is named
 *   by UML's vector driver ("vecN") or the legacy uml_net driver
 *   ("ethN").  We resolve the target by scanning for the first
 *   registered netdev whose name matches a known UML pattern, since
 *   the blob's tap_name does NOT correspond to any in-guest netdev
 *   name.  This avoids adding a new field to the blob (which would
 *   break the wire format already shared with the umlctl userspace).
 *
 *   If the operator needs an explicit override, add a new field to a
 *   future blob version (UM_TEMPLATE_IDENTITY_VERSION = 2) — Phase 2
 *   doesn't need that complexity today.
 *
 * Routes:
 *   The default gateway is added via ip_rt_ioctl(SIOCADDRT).  Any
 *   prior default route on the same netdev is deleted first so this
 *   helper is idempotent across multiple identity applications.
 */

#include <linux/errno.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/printk.h>
#include <linux/rtnetlink.h>
#include <linux/route.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/types.h>
#include <net/ip_fib.h>
#include <net/net_namespace.h>
#include <net/route.h>
#include <net/sock.h>
#include <uapi/linux/in.h>
#include <uapi/linux/sockios.h>

#include <asm/um-template-pause.h>
#include "template_pause_identity.h"
#include "../drivers/mconsole.h"	/* mconsole_reinit_for_pool_member — Phase 4 */
#if IS_ENABLED(CONFIG_UML_NET_VECTOR_V2)
/* um_vec2_tap_reopen_for_pool_member — Memo 09 Phase 2.2 */
#include "../drivers/vector2_internal.h"
#endif

/*
 * Parse an IPv4 CIDR string like "10.7.0.42/24" into a network-order
 * address and prefix length.  Returns 0 on success, -EINVAL on parse
 * failure or out-of-range prefix.
 *
 * Empty input → -ENODATA, distinguishable from a malformed string.
 * The caller can treat -ENODATA as "no IPv4 change requested" rather
 * than as an error.
 */
int um_template_identity_parse_cidr(const char *str, __be32 *addr_be,
				    u8 *prefix_len)
{
	const char *slash;
	u8 a[4];
	unsigned long pl;
	char prefix_buf[8];
	size_t prefix_n;
	int rc;

	if (!str || !*str)
		return -ENODATA;
	if (!addr_be || !prefix_len)
		return -EINVAL;

	slash = strchr(str, '/');
	if (!slash)
		return -EINVAL;
	if (slash == str)
		return -EINVAL;

	rc = in4_pton(str, slash - str, a, '/', NULL);
	if (rc != 1)
		return -EINVAL;

	prefix_n = strlen(slash + 1);
	if (!prefix_n || prefix_n >= sizeof(prefix_buf))
		return -EINVAL;
	memcpy(prefix_buf, slash + 1, prefix_n);
	prefix_buf[prefix_n] = '\0';

	if (kstrtoul(prefix_buf, 10, &pl))
		return -EINVAL;
	if (pl > 32)
		return -EINVAL;

	*addr_be = *(__be32 *)a;
	*prefix_len = (u8)pl;
	return 0;
}

/*
 * Parse a bare IPv4 string like "10.7.0.1" into a network-order
 * address.  Empty input → -ENODATA so callers can distinguish "no
 * gateway requested" from a malformed gateway string.
 */
int um_template_identity_parse_addr(const char *str, __be32 *addr_be)
{
	u8 a[4];
	int rc;

	if (!str || !*str)
		return -ENODATA;
	if (!addr_be)
		return -EINVAL;

	rc = in4_pton(str, -1, a, -1, NULL);
	if (rc != 1)
		return -EINVAL;

	*addr_be = *(__be32 *)a;
	return 0;
}

/*
 * Convert a /N prefix length to a network-order netmask.  N=0 →
 * 0.0.0.0; N=32 → 255.255.255.255.  Any N>32 is clamped to 32 by the
 * shift guard.
 */
__be32 um_template_identity_cidr_mask(u8 prefix_len)
{
	if (prefix_len == 0)
		return 0;
	if (prefix_len >= 32)
		return htonl(0xFFFFFFFFu);
	return htonl(0xFFFFFFFFu << (32 - prefix_len));
}

#ifndef CONFIG_KUNIT_UM_TEMPLATE_PAUSE_IDENTITY_HOSTED

/*
 * Locate the in-guest netdev that the supervisor's host-side TAP is
 * attached to.  UML's vector driver registers netdevs as "vecN"; the
 * legacy uml_net driver uses "ethN".  We scan in priority order: the
 * first registered "vec*" wins, then "eth*".  Loopback ("lo") is
 * always skipped.
 *
 * Returns a pointer to the net_device on success (NO refcount held;
 * caller must be under RTNL), or NULL if no candidate is found.
 *
 * Why not blob->tap_name?  See file-level comment.
 */
static struct net_device *find_target_netdev(struct net *net)
{
	struct net_device *dev, *best = NULL;
	int best_score = 0;

	ASSERT_RTNL();

	for_each_netdev(net, dev) {
		int score;

		if (!strcmp(dev->name, "lo"))
			continue;
		if (!strncmp(dev->name, "vec", 3))
			score = 3;
		else if (!strncmp(dev->name, "eth", 3))
			score = 2;
		else
			score = 1;
		if (score > best_score) {
			best = dev;
			best_score = score;
		}
	}
	return best;
}

/*
 * Apply the MAC address from @blob to @dev under RTNL.  Idempotent:
 * if the device already has this MAC, nothing changes (the net stack
 * itself short-circuits same-address writes).
 */
static int apply_mac(struct net_device *dev,
		     const struct um_template_identity *blob)
{
	struct sockaddr_storage ss = { };
	int rc;

	ASSERT_RTNL();

	ss.ss_family = dev->type;
	memcpy(ss.__data, blob->mac_addr, ETH_ALEN);

	rc = dev_set_mac_address(dev, &ss, NULL);
	if (rc)
		pr_warn("template_pause: dev_set_mac_address(%s, %pM) failed: %d\n",
			dev->name, blob->mac_addr, rc);
	else
		pr_info("template_pause: MAC set on %s -> %pM\n",
			dev->name, blob->mac_addr);
	return rc;
}

/*
 * Remove the first IPv4 address currently bound to @dev.  Called
 * before apply_ipv4 to give idempotence — without clearing, a second
 * identity-apply with a different CIDR would leave the old address
 * behind as a secondary.  Called WITHOUT RTNL (devinet_ioctl takes
 * it internally).
 *
 * SIOCSIFADDR with addr=0.0.0.0 is the in-kernel "delete primary"
 * convention.  Idempotent: -EADDRNOTAVAIL on an already-clean iface
 * is fine.
 */
static int clear_ipv4_addrs(struct net_device *dev)
{
	struct ifreq ifr = { };
	struct sockaddr_in *sin;
	int rc;

	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = 0;
	strscpy(ifr.ifr_name, dev->name, IFNAMSIZ);

	rc = devinet_ioctl(dev_net(dev), SIOCSIFADDR, &ifr);
	if (rc && rc != -EADDRNOTAVAIL)
		pr_warn("template_pause: clear-addrs %s failed: %d\n",
			dev->name, rc);
	return (rc == -EADDRNOTAVAIL) ? 0 : rc;
}

/*
 * Apply the IPv4 CIDR from @blob to @dev.  Steps:
 *   1. Clear any prior IPv4 addresses (idempotence).
 *   2. SIOCSIFADDR sets the primary address.
 *   3. SIOCSIFNETMASK sets the netmask derived from the prefix.
 *   4. SIOCSIFFLAGS sets IFF_UP so the interface is operational.
 *
 * Each ioctl runs without holding RTNL (devinet_ioctl acquires it
 * internally).  Errors are logged and propagated; the caller decides
 * whether to abort the rest of the apply.
 */
static int apply_ipv4(struct net_device *dev,
		      const struct um_template_identity *blob)
{
	struct net *net = dev_net(dev);
	struct ifreq ifr = { };
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	__be32 addr_be = 0, mask_be = 0;
	u8 prefix_len = 0;
	int rc;

	rc = um_template_identity_parse_cidr(blob->ipv4_cidr,
					     &addr_be, &prefix_len);
	if (rc == -ENODATA) {
		pr_info("template_pause: ipv4_cidr empty on %s; skipping IPv4 rebind\n",
			dev->name);
		return 0;
	}
	if (rc) {
		pr_warn("template_pause: bad ipv4_cidr \"%s\": %d\n",
			blob->ipv4_cidr, rc);
		return rc;
	}
	mask_be = um_template_identity_cidr_mask(prefix_len);

	(void)clear_ipv4_addrs(dev);

	strscpy(ifr.ifr_name, dev->name, IFNAMSIZ);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr_be;

	rc = devinet_ioctl(net, SIOCSIFADDR, &ifr);
	if (rc) {
		pr_warn("template_pause: SIOCSIFADDR(%s, %pI4) failed: %d\n",
			dev->name, &addr_be, rc);
		return rc;
	}

	memset(&ifr, 0, sizeof(ifr));
	strscpy(ifr.ifr_name, dev->name, IFNAMSIZ);
	sin = (struct sockaddr_in *)&ifr.ifr_netmask;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = mask_be;

	rc = devinet_ioctl(net, SIOCSIFNETMASK, &ifr);
	if (rc) {
		pr_warn("template_pause: SIOCSIFNETMASK(%s, /%u) failed: %d\n",
			dev->name, prefix_len, rc);
		return rc;
	}

	memset(&ifr, 0, sizeof(ifr));
	strscpy(ifr.ifr_name, dev->name, IFNAMSIZ);
	ifr.ifr_flags = IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST;
	rc = devinet_ioctl(net, SIOCSIFFLAGS, &ifr);
	if (rc)
		pr_warn("template_pause: SIOCSIFFLAGS(%s, UP) failed: %d\n",
			dev->name, rc);

	pr_info("template_pause: IPv4 set on %s -> %pI4/%u\n",
		dev->name, &addr_be, prefix_len);
	return 0;
}

/*
 * Apply the IPv4 default gateway from @blob.  Uses ip_rt_ioctl with
 * SIOCADDRT to install a default route via @gateway_be on @dev.
 *
 * Idempotence: SIOCADDRT returns -EEXIST if the same route already
 * exists.  We treat -EEXIST as success.  For "gateway changed
 * between takes" we would need a SIOCDELRT first, but that requires
 * remembering the previous gateway.  The simpler path: cooperatively
 * assume the supervisor only changes the gateway when the underlying
 * network changes (which would also change tap/MAC) — and accept the
 * -EEXIST in the steady state.
 */
static int apply_default_route(struct net_device *dev,
			       const struct um_template_identity *blob)
{
	struct net *net = dev_net(dev);
	struct rtentry rt = { };
	struct sockaddr_in *sin;
	__be32 gw_be = 0;
	int rc;

	rc = um_template_identity_parse_addr(blob->ipv4_gateway, &gw_be);
	if (rc == -ENODATA) {
		pr_info("template_pause: ipv4_gateway empty on %s; skipping default route\n",
			dev->name);
		return 0;
	}
	if (rc) {
		pr_warn("template_pause: bad ipv4_gateway \"%s\": %d\n",
			blob->ipv4_gateway, rc);
		return rc;
	}

	sin = (struct sockaddr_in *)&rt.rt_dst;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = 0;	/* 0.0.0.0 default */

	sin = (struct sockaddr_in *)&rt.rt_genmask;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = 0;

	sin = (struct sockaddr_in *)&rt.rt_gateway;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = gw_be;

	rt.rt_flags = RTF_UP | RTF_GATEWAY;
	rt.rt_dev = (char *)dev->name;

	rc = ip_rt_ioctl(net, SIOCADDRT, &rt);
	if (rc == -EEXIST) {
		pr_info("template_pause: default route via %pI4 already present on %s\n",
			&gw_be, dev->name);
		return 0;
	}
	if (rc) {
		pr_warn("template_pause: SIOCADDRT default via %pI4 dev %s failed: %d\n",
			&gw_be, dev->name, rc);
		return rc;
	}

	pr_info("template_pause: default route set via %pI4 dev %s\n",
		&gw_be, dev->name);
	return 0;
}

/*
 * Public entry: apply @blob's identity (MAC + IPv4 + gateway) to the
 * in-guest netdev.  Returns 0 on success, -errno on any one step
 * failing.  Errors do NOT abort subsequent steps — the function
 * applies as much as it can and reports the first failure, so a
 * partial-apply (e.g., MAC succeeded but IP didn't) is the visible
 * outcome rather than an all-or-nothing.
 *
 * Must be called in process context with no locks held.  RTNL is
 * taken internally as needed.
 */
/*
 * um_template_identity_log_parsed() — log the blob's contents
 * after validation.  Split out from um_template_identity_apply()
 * so callers can verify "did the blob parse cleanly?" without
 * also requiring a target netdev to exist (selftests with hostfs-
 * only boots have no netdev, so the apply step returns -ENODEV
 * even though the parse half is correct).
 */
void um_template_identity_log_parsed(const struct um_template_identity *blob)
{
	if (!blob)
		return;
	pr_info("template_pause: identity-parsed name=\"%s\" mac=%pM tap=\"%s\" ip=\"%s\" gw=\"%s\"\n",
		blob->instance_name, blob->mac_addr,
		blob->tap_name, blob->ipv4_cidr, blob->ipv4_gateway);
}
EXPORT_SYMBOL_GPL(um_template_identity_log_parsed);

int um_template_identity_apply(const struct um_template_identity *blob)
{
	struct net_device *dev;
	int first_err = 0, rc;

	if (!blob)
		return -EINVAL;

	/*
	 * Log the parsed blob first.  This always succeeds (blob is
	 * non-NULL by this point) and gives selftests a reliable
	 * marker that the read+parse pipeline worked, independent of
	 * whether a netdev is present to apply to.
	 */
	um_template_identity_log_parsed(blob);

	rtnl_lock();
	dev = find_target_netdev(&init_net);
	if (!dev) {
		rtnl_unlock();
		pr_warn("template_pause: no target netdev found; identity NOT applied (parse OK)\n");
		return -ENODEV;
	}
	pr_info("template_pause: applying identity to in-guest netdev %s (blob tap=\"%s\")\n",
		dev->name, blob->tap_name);

	rc = apply_mac(dev, blob);
	if (rc && !first_err)
		first_err = rc;
	rtnl_unlock();

	/*
	 * Memo 09 Phase 2.2 (hardening-plan §1.4): swap the netdev's
	 * underlying host TAP to a per-member name.  Without this,
	 * every pool member ends up sharing the master's TAP via CoW
	 * — the interface name is wrong, the host's bridge / IP
	 * configuration was set up against ONE TAP, and SIOCSIFFLAGS
	 * UP later fails because the inherited fd is bogus from the
	 * test harness's perspective.
	 *
	 * blob->tap_name carries the per-member host TAP name the
	 * daemon allocated.  We open a fresh /dev/net/tun fd with
	 * TUNSETIFF(<that name>) and attach it to the netdev.  The
	 * operator is responsible for the host-side TAP existing (or
	 * being creatable with the calling guest's CAP_NET_ADMIN).
	 *
	 * Reopen only valid for vec2 TAP-backed netdevs.  Non-vec2
	 * (legacy uml_net eth0, virtio-net) → skip silently; those
	 * use different mechanisms.  Failure is non-fatal but
	 * recorded as first_err so the caller knows the network is
	 * not yet usable.
	 *
	 * Done BEFORE apply_ipv4 + apply_default_route so the
	 * IPv4-on-up sequence below sees the right fd.
	 */
#if IS_ENABLED(CONFIG_UML_NET_VECTOR_V2)
	if (blob->tap_name[0] != '\0' && !strncmp(dev->name, "vec", 3)) {
		size_t tlen;
		char tap_name[sizeof(blob->tap_name) + 1];

		memcpy(tap_name, blob->tap_name, sizeof(blob->tap_name));
		tap_name[sizeof(blob->tap_name)] = '\0';
		tlen = strnlen(tap_name, sizeof(blob->tap_name));
		if (tlen > 0) {
			rc = um_vec2_tap_reopen_for_pool_member(dev, tap_name);
			if (rc) {
				pr_warn("template_pause: tap-reopen(%s, %s) failed: %d; identity partially applied\n",
					dev->name, tap_name, rc);
				if (!first_err)
					first_err = rc;
			} else {
				pr_info("template_pause: tap-reopened %s on host TAP %s\n",
					dev->name, tap_name);
			}
		}
	}
#else
	if (blob->tap_name[0] != '\0')
		pr_warn_once("template_pause: blob->tap_name=%s but CONFIG_UML_NET_VECTOR_V2=n; per-member TAP swap unavailable\n",
			     blob->tap_name);
#endif

	rc = apply_ipv4(dev, blob);
	if (rc && !first_err)
		first_err = rc;

	rc = apply_default_route(dev, blob);
	if (rc && !first_err)
		first_err = rc;

	/*
	 * Memo 09 Phase 4 (hardening-plan §1.6): per-pool-member
	 * mconsole socket.  Without this, every pool member inherits
	 * the master's mconsole IRQ + socket file, so `umlctl exec
	 * --pid <member>` ends up addressing the master rather than
	 * the specific member.  The identity blob's mconsole_path
	 * (96 bytes) carries the per-member path the daemon synthesized;
	 * if it's non-empty, rebind here.
	 *
	 * Failure is not fatal — the member still works for everything
	 * except remote exec.  Log via apply_mconsole's own pr_info on
	 * success; mconsole_reinit_for_pool_member already pr_warn's
	 * on failure.
	 */
	if (blob->mconsole_path[0] != '\0') {
		size_t plen;
		char path[sizeof(blob->mconsole_path) + 1];

		/* Defensive NUL-terminate; blob fields are not guaranteed
		 * to be NUL-terminated.
		 */
		memcpy(path, blob->mconsole_path, sizeof(blob->mconsole_path));
		path[sizeof(blob->mconsole_path)] = '\0';
		plen = strnlen(path, sizeof(blob->mconsole_path));
		if (plen > 0) {
			rc = mconsole_reinit_for_pool_member(path);
			if (rc && !first_err)
				first_err = rc;
		}
	}

	return first_err;
}
EXPORT_SYMBOL_GPL(um_template_identity_apply);

#endif /* !CONFIG_KUNIT_UM_TEMPLATE_PAUSE_IDENTITY_HOSTED */
