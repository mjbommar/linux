/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Generic memory-region descriptor used by struct um_backend_ops's
 * mm_region_added / mm_region_removed / mm_region_protected ops
 * (memo 25 refactor 5).
 *
 * The seccomp backend treats this as an opaque payload it ignores
 * past the fields it needs (va / len / prot / phys_fd / offset).
 * The v2 KVM backend uses it as the memslot allocation key — the
 * `backend_data` slot is for the backend's per-region state
 * (e.g., memslot ID, mmap'd host buffer, mmu_notifier handle).
 *
 * Passed by `const struct um_memory_region *` so backends can
 * reference per-region fields by pointer without copy. The struct
 * is owned by the mm-arbiter (arch/um/kernel/tlb.c::um_tlb_sync)
 * and is short-lived (single um_tlb_sync drain pass).
 */
#ifndef __UM_MEMORY_H
#define __UM_MEMORY_H

#include <linux/types.h>

struct um_memory_region {
	unsigned long	va;
	unsigned long	len;
	int		prot;
	int		phys_fd;	/* -1 for anonymous (none today) */
	u64		offset;
	void		*backend_data;	/* opaque, backend-managed */
};

#endif /* __UM_MEMORY_H */
