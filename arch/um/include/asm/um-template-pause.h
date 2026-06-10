/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_TEMPLATE_PAUSE_H
#define __ASM_UM_TEMPLATE_PAUSE_H

/*
 * UML template-pause hook.
 *
 * Lets a booted UML stop at a named ready point, raise SIGSTOP in its
 * host process, and resume on SIGCONT after an optional identity blob
 * has been supplied.
 *
 * In fork-on-resume mode the paused master forks a child for each take,
 * reports the child's host pid through the identity channel, and pauses
 * again for the next take. This is separate from the AFL-style
 * forkserver path in arch/um/kernel/snapshot.c, where fds 198/199 drive
 * the loop.
 */

#include <linux/errno.h>
#include <linux/types.h>

/*
 * Identity blob shape. The supervisor writes one of these to a memfd
 * that the master/child has open via env var UM_TEMPLATE_IDENTITY_FD.
 * The kernel reads, validates, and applies the identity after the
 * paused process resumes.
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
	char  mconsole_path[96];	/* mconsole socket path */
	__u8  reserved[32];		/* must be zero */
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
 *   - Host kernel suspends the process; a supervisor may update the
 *     identity memfd while the process is stopped.
 *   - When the supervisor sends SIGCONT, execution resumes here.
 *   - If UM_TEMPLATE_IDENTITY_FD is plumbed, read the identity blob,
 *     validate its magic/version, and apply the requested identity.
 *
 * Returns 0 on a clean single-shot resume, -errno on a host syscall
 * failure, or -ENODEV if template-pause was not armed on the cmdline.
 * In fork-on-resume mode the master stays in the fork loop until an
 * error or teardown; child behavior depends on the configured child
 * entry path. Identity-read failures degrade gracefully.
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
