// SPDX-License-Identifier: GPL-2.0
/*
 * UML snapshot / forkserver seam (workstream C-09).
 *
 * This translation unit is the cooperative in-kernel seam that lets a
 * UML guest reach a named "ready point," quiesce to a barrier, and
 * ``fork()`` itself for each fuzz iteration. The AFL-compatible
 * 12-byte-per-iteration wire protocol on fds 198/199 lives here too;
 * it binds directly onto syzkaller's ``Instance.RunSnapshot`` API so
 * the fuzz profile does not need its own per-iteration negotiation.
 *
 * Commit 1 lands only the skeleton: the static key, the mmap-region
 * registry, and WARN-stubbed public entry points. Commit 2 adds the
 * quiesce-and-fork path; commit 3 the worker-side post-fork reinit
 * (including the KASAN MADV_DONTFORK fix from D37 pull-forward #6);
 * commit 4 the host-fd hygiene sweep; commit 5 the user doc +
 * selftest; commit 6 the fuzz-defconfig wire-up. Bisectable by
 * design; each commit builds clean on gcc and clang.
 *
 * Design + decisions:
 *   - Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/
 *     09-snapshot-forkserver.md — the v1 design.
 *   - 04-risks/decisions-log.md D35 — v1 scope (forkserver, not CRIU).
 *   - 04-risks/decisions-log.md D36 — v2 on-disk format (ELF + notes).
 *   - 04-risks/decisions-log.md D37 — v1 pull-forward items, including
 *     this file's mmap registry.
 */

#include <linux/export.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <asm/um-mmaps.h>
#include <asm/um-snapshot.h>

/*
 * Off by default in every profile. The fuzz and fuzz-deep defconfigs
 * flip it on at boot once the ready-point handshake completes;
 * workers flip it back off locally in um_snapshot_worker_init() so
 * hot paths stay zero-cost.
 */
DEFINE_STATIC_KEY_FALSE(um_snapshot_enabled);
EXPORT_SYMBOL_GPL(um_snapshot_enabled);

/* Registry of kernel mmap regions (D37 pull-forward #1). Populated by
 * boot-time callers of um_register_mmap_region(); today append-only
 * at boot, read without the lock afterwards. The lock is declared
 * here for the day a post-boot registration path appears (hot-plug
 * hostfs, runtime-attached time-travel shm, …).
 */
LIST_HEAD(um_mmap_regions);
EXPORT_SYMBOL_GPL(um_mmap_regions);
DEFINE_SPINLOCK(um_mmap_regions_lock);
EXPORT_SYMBOL_GPL(um_mmap_regions_lock);

/**
 * um_register_mmap_region() - record a kernel-side host mmap region.
 * @r: caller-allocated region descriptor. Must outlive the kernel
 *	(typically static-storage duration).
 *
 * Thread-safe against concurrent registration; callers that register
 * at boot before SMP comes up may call this unlocked.
 */
void um_register_mmap_region(struct um_mmap_region *r)
{
	unsigned long flags;

	if (WARN_ON_ONCE(!r || !r->name || !r->len))
		return;

	spin_lock_irqsave(&um_mmap_regions_lock, flags);
	list_add_tail(&r->list, &um_mmap_regions);
	spin_unlock_irqrestore(&um_mmap_regions_lock, flags);
}
EXPORT_SYMBOL_GPL(um_register_mmap_region);

/*
 * Commit 1 stubs. Either entry point being called at this stage means
 * the caller raced ahead of the implementation series; warn loudly.
 * Commits 2 and 3 replace these with the real quiesce-and-fork and
 * worker-reinit paths.
 */
void um_snapshot_ready(const char *named_point)
{
	WARN_ONCE(1,
		  "%s(\"%s\"): only commit 1 has landed; quiesce-and-fork is in commit 2 per %s\n",
		  __func__,
		  named_point ? named_point : "(null)",
		  "Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md");
}
EXPORT_SYMBOL_GPL(um_snapshot_ready);

void um_snapshot_worker_init(void)
{
	WARN_ONCE(1,
		  "%s: only commit 1 has landed; post-fork reinit is in commit 3 per %s\n",
		  __func__,
		  "Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md");
}
EXPORT_SYMBOL_GPL(um_snapshot_worker_init);
