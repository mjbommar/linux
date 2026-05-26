/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML template-pause identity application — internal interface
 * shared between template_pause_identity.c (implementation), the
 * KUnit test TU (template_pause_identity_test.c), and the fork-on-
 * resume loop driver (template_pause.c).
 *
 * See template_pause_identity.c for the architectural rationale.
 */

#ifndef _ARCH_UM_KERNEL_TEMPLATE_PAUSE_IDENTITY_H
#define _ARCH_UM_KERNEL_TEMPLATE_PAUSE_IDENTITY_H

#include <linux/types.h>

struct um_template_identity;

/*
 * Parse helpers (pure C — KUnit-testable, no kernel net stack
 * dependencies).  The implementation lives in
 * template_pause_identity.c; the KUnit TU consumes them via this
 * header without dragging in the net-stack-heavy apply helpers.
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
 * Log the parsed identity blob to dmesg.  Always succeeds (no
 * external state required); selftests can grep for the
 * "identity-parsed" marker to verify the read+parse pipeline
 * without requiring a netdev for the apply step.
 */
void um_template_identity_log_parsed(const struct um_template_identity *blob);

#endif /* _ARCH_UM_KERNEL_TEMPLATE_PAUSE_IDENTITY_H */
