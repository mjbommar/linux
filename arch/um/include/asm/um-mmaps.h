/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_MMAPS_H
#define __ASM_UM_MMAPS_H

/*
 * Enumeration of UML's kernel-side host mmap regions.
 *
 * UML runs the Linux kernel as a host-userspace ELF process. The
 * kernel's "physical" memory, KASAN shadow, vmalloc region, stub
 * stacks, and time-travel shared memory are all ordinary host mmaps
 * in the UML process's address space. This table gives those mappings one
 * typed registry instead of leaving each mmap call site to describe itself.
 *
 * This header introduces a typed, registry-style table so that:
 *
 *   - The fork-server worker_init path can know which regions
 *     to re-map, which to let COW, and which are ephemeral (see the
 *     disposition enum below).
 *   - The FD-hygiene sweep can cross-reference which
 *     mmaps are file-backed and need FD disposition tags.
 *   - Snapshot serializers can walk this table to emit one ELF PT_LOAD per
 *     entry with the right sparseness / zero-fill policy.
 *
 * Registration lives in arch/um/kernel/snapshot.c. The table is
 * populated at boot by setup_physmem() and kasan_map_memory();
 * other mmap sites can register when they become snapshot-aware.
 */

#include <linux/compiler.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/**
 * enum um_mmap_disposition - how each kernel mmap region should be
 * handled by fork-server worker re-init and snapshot serialization.
 *
 * @UM_MMAP_INHERIT_COW: region is inherited across fork() via
 *	copy-on-write (MAP_SHARED file-backed, or MAP_PRIVATE without
 *	MADV_DONTFORK). Worker sees the parent's state; pages copy on
 *	first write. Example: physmem_fd guest RAM.
 * @UM_MMAP_REMAP_IN_WORKER: region is NOT inherited; worker must
 *	re-map it. Used by regions that are explicitly excluded from
 *	fork inheritance.
 * @UM_MMAP_EPHEMERAL: region is ephemeral scratch space; neither
 *	worker nor snapshot serialization needs its contents. Example:
 *	boot-time stub probe buffers, per-CPU IRQ stacks.
 * @UM_MMAP_SERIALIZE: region must be written by snapshot serializers.
 *	This is the subset that matters for survive-reboot. Usually
 *	implies INHERIT_COW (fork sees it too).
 *
 * A region can be INHERIT_COW without being SERIALIZE (guest RAM that
 * is reconstructible from boot) and SERIALIZE without being
 * INHERIT_COW (for example, a region only needed on restore). The two axes
 * are tracked together in um_mmap_region.flags rather than as a single
 * enum.
 */
enum um_mmap_disposition {
	UM_MMAP_INHERIT_COW		= 1 << 0,
	UM_MMAP_REMAP_IN_WORKER		= 1 << 1,
	UM_MMAP_EPHEMERAL		= 1 << 2,
	UM_MMAP_SERIALIZE		= 1 << 3,
};

/**
 * struct um_mmap_region - one host mmap owned by the UML kernel.
 * @name: short, unique, printable identifier; used in WARN/OOPS paths
 *	and as a stable note/section name for snapshot serializers.
 * @base: region start virtual address in the UML process.
 * @len: region length in bytes. Must be a multiple of PAGE_SIZE.
 * @flags: OR of enum um_mmap_disposition bits.
 * @backing_fd: host fd backing the mapping, or -1 if anonymous.
 *	For physmem_fd-backed regions this is the one tempfile
 *	that holds all of guest RAM.
 * @list: linkage in um_mmap_regions list, protected by
 *	um_mmap_regions_lock.
 */
struct um_mmap_region {
	const char		*name;
	void			*base;
	size_t			 len;
	unsigned int		 flags;
	int			 backing_fd;
	struct list_head	 list;
};

/*
 * Registry of all registered kernel mmap regions. Populated by
 * boot-time callers of um_register_mmap_region(); iterated by
 * snapshot / worker_init code behind um_mmap_regions_lock.
 *
 * The list is append-only at boot and never modified afterwards, so
 * readers need no lock. The lock also covers a post-boot registration
 * path, such as hot-pluggable hostfs mounts.
 */
extern struct list_head um_mmap_regions;
extern spinlock_t um_mmap_regions_lock;

#ifdef CONFIG_UM_SNAPSHOT_FORKSERVER
void um_register_mmap_region(struct um_mmap_region *r);
#else
static inline void um_register_mmap_region(struct um_mmap_region *r) { }
#endif

#endif /* __ASM_UM_MMAPS_H */
