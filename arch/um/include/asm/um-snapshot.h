/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_SNAPSHOT_H
#define __ASM_UM_SNAPSHOT_H

/*
 * UML snapshot / forkserver seam.
 *
 * Public interface between the cooperative in-kernel snapshot path
 * (arch/um/kernel/snapshot.c) and its callers, including debugfs,
 * mconsole, and the AFL fd-198/199 forkserver handshake.
 */

#include <linux/jump_label.h>

/*
 * Static key gating the snapshot/forkserver path. Off in every
 * profile except fuzz / fuzz-deep (see 03-profiles/fuzz.md); with
 * the key off, the in-kernel call sites short-circuit via the jump-
 * label machinery and pay no runtime cost.
 */
DECLARE_STATIC_KEY_FALSE(um_snapshot_enabled);

#ifdef CONFIG_UM_SNAPSHOT_FORKSERVER

/**
 * um_snapshot_ready() - reach the named "ready point" and, if the
 * static key is on, quiesce the parent and fork worker children.
 * @named_point: printable identifier of the call site (e.g.
 *	"postinit", "mconsole", "afl-fd"); shown in dmesg and carried
 *	into snapshot metadata such as a UML_READY_POINT note.
 *
 * Intended call pattern: one call per ready point declared by the
 * profile. With um_snapshot_enabled off, this is a direct return.
 * With it on, the parent quiesces and enters the forkserver loop;
 * workers return to the caller with um_snapshot_enabled forced
 * off locally so hot paths have zero overhead.
 */
void um_snapshot_ready(const char *named_point);

/**
 * um_snapshot_worker_init() - post-fork per-worker reinit.
 *
 * Called exactly once in each forked child, right after fork()
 * returns in the parent's forkserver loop. Handles the five things
 * fork() does not inherit: non-caller vCPU pthreads, seccomp
 * stub children, timerfd + signalfd, host-fd allowlist, pending
 * signals. Also flips um_snapshot_enabled back off in the child
 * so worker syscalls run at zero overhead.
 */
void um_snapshot_worker_init(void);

/*
 * Replicate master's physmem_fd content into a fresh per-member
 * memfd, swap the kernel-side MAP_SHARED mapping to the new fd,
 * and update the global physmem_fd so subsequent stub mmaps also
 * use the new fd.  Called by pool-member fork-child entry to
 * physically isolate this member from master and siblings while
 * preserving kernel/stub coherence within the member.
 *
 * Returns 0 on success, -errno on failure.
 */
int um_pool_replicate_physmem(void);

/*
 * Re-map kernel-VA physmem with the existing physmem_fd at the same
 * offset. The contents do not change; callers use this to validate
 * the host-side remap path independently from backing-fd replacement.
 */
int um_pool_remap_self_test(void);

#else /* !CONFIG_UM_SNAPSHOT_FORKSERVER */

static inline void um_snapshot_ready(const char *named_point) { }
static inline void um_snapshot_worker_init(void) { }
static inline int um_pool_replicate_physmem(void) { return 0; }
static inline int um_pool_remap_self_test(void) { return 0; }

#endif /* CONFIG_UM_SNAPSHOT_FORKSERVER */

#endif /* __ASM_UM_SNAPSHOT_H */
