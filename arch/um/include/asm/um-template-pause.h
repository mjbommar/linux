/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_TEMPLATE_PAUSE_H
#define __ASM_UM_TEMPLATE_PAUSE_H

/*
 * UML template-pause + fork hook (Memo 09).
 *
 * The cooperative in-kernel seam that lets a booted UML quiesce at a
 * named "ready point," signal its host supervisor (via raise(SIGSTOP)),
 * and resume execution on SIGCONT after the supervisor has forked the
 * host process N times and replumbed each child's identity.
 *
 * Distinct from the older AFL-style forkserver path in
 * arch/um/kernel/snapshot.c (workstream C-09): that path's parent
 * never returns and uses fds 198/199 to drive per-iteration fork. The
 * template-pause path's parent DOES return after SIGCONT and is meant
 * to be forked externally by the supervisor (umlctl pool serve), so
 * each forked child wakes from the same SIGSTOP point with its own
 * identity blob to apply.
 *
 * Long-form design:
 *   Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-
 *   next-sprint/09-fork-server-snapshot-restore.md
 */

#include <linux/errno.h>
#include <linux/types.h>

/*
 * Identity blob shape. The supervisor writes one of these to a memfd
 * that the master/child has open via env var UM_TEMPLATE_IDENTITY_FD.
 * Phase 1a (this commit): kernel reads and logs the blob but does
 * NOT yet apply MAC / IP / netdev changes — that's Phase 2.
 */
#define UM_TEMPLATE_IDENTITY_MAGIC 0x44495455u	/* 'UTID' little-endian */
#define UM_TEMPLATE_IDENTITY_VERSION 1u

struct um_template_identity {
	__u32 magic;			/* must equal UM_TEMPLATE_IDENTITY_MAGIC */
	__u32 version;			/* must equal UM_TEMPLATE_IDENTITY_VERSION */
	char  instance_name[64];	/* NUL-terminated; supervisor's pool member name */
	__u8  mac_addr[6];		/* new MAC for the forked child's netdev */
	__u8  _pad[2];
	char  tap_name[16];		/* new IFF_TAP device name */
	char  ipv4_cidr[20];		/* e.g. "10.7.0.42/24"; empty = unchanged */
	char  ipv4_gateway[16];		/* e.g. "10.7.0.1"; empty = unchanged */
	char  mconsole_path[96];	/* mconsole socket path (Phase 2+) */
	__u8  reserved[32];		/* zero-init; future-proofing */
};

#ifdef CONFIG_UM_TEMPLATE_PAUSE

/**
 * um_template_pause_enter() - quiesce at @named_point and wait for SIGCONT.
 * @named_point: printable identifier of the call site; logged to dmesg.
 *
 * Pre-conditions:
 *   - Kernel was booted with um_template_pause on the cmdline (the
 *     setup callback flips the arm flag).
 *   - Caller is in task context with signals enabled (UML
 *     signals_enabled == 1).
 *
 * Behavior:
 *   - Logs entry, then raises SIGSTOP on the UML host process.
 *   - Host kernel suspends the process; the supervisor sees the stop
 *     via waitpid(WUNTRACED) and fork(2)s the master.  Each child
 *     inherits the SIGSTOPped state.
 *   - When the supervisor SIGCONTs the (child) process, kill() returns
 *     and we resume here.
 *   - If UM_TEMPLATE_IDENTITY_FD is plumbed, read the identity blob
 *     and validate its magic/version.  Phase 1a logs; Phase 2 applies.
 *
 * Returns 0 on a clean resume, -errno on a host syscall failure, or
 * -ENODEV if template-pause was not armed on the cmdline.  Identity-
 * read failures degrade gracefully (logged, but pause still succeeds).
 */
int um_template_pause_enter(const char *named_point);

/**
 * um_template_pause_armed() - true if um_template_pause was set on cmdline.
 *
 * Used by callers that want to gate entry on the cmdline arm. The
 * /proc/um/template_pause write handler also checks this and refuses
 * the pause if not armed, so a benign poke from a non-pool boot is a
 * no-op rather than a host process hang.
 */
bool um_template_pause_armed(void);

#else /* !CONFIG_UM_TEMPLATE_PAUSE */

static inline int um_template_pause_enter(const char *named_point) { return -ENODEV; }
static inline bool um_template_pause_armed(void) { return false; }

#endif /* CONFIG_UM_TEMPLATE_PAUSE */

#endif /* __ASM_UM_TEMPLATE_PAUSE_H */
