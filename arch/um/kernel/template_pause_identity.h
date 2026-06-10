/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML template-pause identity application.
 *
 * Shared declarations for template_pause_identity.c, the KUnit test
 * translation unit, and the fork-on-resume loop in template_pause.c.
 *
 * See template_pause_identity.c for the architectural rationale.
 */

#ifndef _ARCH_UM_KERNEL_TEMPLATE_PAUSE_IDENTITY_H
#define _ARCH_UM_KERNEL_TEMPLATE_PAUSE_IDENTITY_H

#include <linux/types.h>

struct um_template_identity;

/*
 * Parse helpers with no kernel net-stack dependencies. The KUnit
 * test translation unit consumes them through this header without
 * pulling in the apply path.
 */
int um_template_identity_parse_cidr(const char *str, __be32 *addr_be,
				    u8 *prefix_len);
int um_template_identity_parse_addr(const char *str, __be32 *addr_be);
__be32 um_template_identity_cidr_mask(u8 prefix_len);

/*
 * Apply the identity blob to the in-guest netdev.  See the file-
 * level rationale in template_pause_identity.c for why this is
 * called from the MASTER (parent) of the fork loop rather than from
 * the M-fork child.
 */
int um_template_identity_apply(const struct um_template_identity *blob);

/*
 * Log the parsed identity blob to dmesg. Always succeeds because it
 * requires no external state; selftests use the stable
 * "identity-parsed" log line to verify the read+parse pipeline
 * without requiring a netdev for the apply step.
 */
void um_template_identity_log_parsed(const struct um_template_identity *blob);

#endif /* _ARCH_UM_KERNEL_TEMPLATE_PAUSE_IDENTITY_H */
