// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/module.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <asm/page.h>
#include <asm/sections.h>
#include <as-layout.h>
#include <init.h>
#include <kern.h>
#include <kern_util.h>
#include <mem_user.h>
#include <os.h>
#ifdef CONFIG_UM_SNAPSHOT_FORKSERVER
#include <asm/um-mmaps.h>
#endif

static int physmem_fd = -1;

/* Changed during early boot */
unsigned long high_physmem;
EXPORT_SYMBOL(high_physmem);

#ifdef CONFIG_UM_SNAPSHOT_FORKSERVER
/*
 * Entry in the C-09 kernel mmap registry (D37 pull-forward #1) that
 * describes the guest-RAM mapping set up by setup_physmem(). Filled
 * in at boot and registered once, before user tasks exist. Static
 * storage so the registry's list linkage outlives the kernel's init
 * phase.
 *
 * INHERIT_COW: guest RAM is MAP_SHARED file-backed; fork() gives the
 * child a COW view of the same file-backed pages. SERIALIZE: v2
 * snapshot-to-disk reads the physmem_fd's content directly rather
 * than walking pagemap. See D36.
 */
static struct um_mmap_region physmem_mmap_region = {
	.name  = "physmem",
	.flags = UM_MMAP_INHERIT_COW | UM_MMAP_SERIALIZE,
};
#endif

void map_memory(unsigned long virt, unsigned long phys, unsigned long len,
		int r, int w, int x)
{
	__u64 offset;
	int fd, err;

	fd = phys_mapping(phys, &offset);
	err = os_map_memory((void *) virt, fd, offset, len, r, w, x);
	if (err) {
		if (err == -ENOMEM)
			printk(KERN_ERR "try increasing the host's "
			       "/proc/sys/vm/max_map_count to <physical "
			       "memory size>/4096\n");
		panic("map_memory(0x%lx, %d, 0x%llx, %ld, %d, %d, %d) failed, "
		      "err = %d\n", virt, fd, offset, len, r, w, x, err);
	}
}

/**
 * setup_physmem() - Setup physical memory for UML
 * @start:	Start address of the physical kernel memory,
 *		i.e start address of the executable image.
 * @reserve_end:	end address of the physical kernel memory.
 * @len:	Length of total physical memory that should be mapped/made
 *		available, in bytes.
 *
 * Creates an unlinked temporary file of size (len) and memory maps
 * it on the last executable image address (uml_reserved).
 *
 * The offset is needed as the length of the total physical memory
 * (len) includes the size of the memory used be the executable image,
 * but the mapped-to address is the last address of the executable image
 * (uml_reserved == end address of executable image).
 *
 * The memory mapped memory of the temporary file is used as backing memory
 * of all user space processes/kernel tasks.
 *
 * Contract (workstream C-09, D37 pull-forward #5): physmem_fd must
 * be a regular host file — not an anonymous mapping — and the range
 * mapped by os_map_memory() below must use MAP_SHARED. Both hold
 * today: create_mem_file() → create_tmp_file() returns a real
 * tempfile in tmpfs, and os_map_memory() always maps with
 * MAP_SHARED | MAP_FIXED (arch/um/os-Linux/process.c:96). Do not
 * silently flip either property without re-reading
 * `Documentation/virt/uml/redesign/04-risks/decisions-log.md` D37:
 * v2 snapshot-to-disk depends on being able to write out guest RAM
 * by reading this one fd, and the fork-server worker depends on
 * inheriting guest RAM via MAP_SHARED COW.
 */
void __init setup_physmem(unsigned long start, unsigned long reserve_end,
			  unsigned long len)
{
	unsigned long reserve = reserve_end - start;
	unsigned long map_size = len - reserve;
	int err;

	if (len <= reserve) {
		os_warn("Too few physical memory! Needed=%lu, given=%lu\n",
			reserve, len);
		exit(1);
	}

	physmem_fd = create_mem_file(len);

	err = os_map_memory((void *) reserve_end, physmem_fd, reserve,
			    map_size, 1, 1, 1);
	if (err < 0) {
		os_warn("setup_physmem - mapping %lu bytes of memory at 0x%p "
			"failed - errno = %d\n", map_size,
			(void *) reserve_end, err);
		exit(1);
	}

	/*
	 * Special kludge - This page will be mapped in to userspace processes
	 * from physmem_fd, so it needs to be written out there.
	 */
	os_seek_file(physmem_fd, __pa(__syscall_stub_start));
	os_write_file(physmem_fd, __syscall_stub_start, PAGE_SIZE);

	memblock_add(__pa(start), len);
	memblock_reserve(__pa(start), reserve);

	min_low_pfn = PFN_UP(__pa(reserve_end));
	max_low_pfn = min_low_pfn + (map_size >> PAGE_SHIFT);

#ifdef CONFIG_UM_SNAPSHOT_FORKSERVER
	/* Register the guest-RAM region with the C-09 mmap registry.
	 * Consumers: the fork-server worker reinit path (commit 3c),
	 * v2 snapshot-to-disk (D36; reads this region out of the
	 * backing file instead of walking pagemap).
	 */
	physmem_mmap_region.base = (void *)reserve_end;
	physmem_mmap_region.len  = map_size;
	physmem_mmap_region.backing_fd = physmem_fd;
	um_register_mmap_region(&physmem_mmap_region);
#endif
}

int phys_mapping(unsigned long phys, unsigned long long *offset_out)
{
	int fd = -1;

	if (phys < physmem_size) {
		fd = physmem_fd;
		*offset_out = phys;
	}

	return fd;
}
EXPORT_SYMBOL(phys_mapping);

#ifdef CONFIG_UM_SNAPSHOT_FORKSERVER
/**
 * um_pool_replicate_physmem() — fork-child entry helper that
 * isolates this UML kernel's physmem from master and sibling pool
 * members.
 *
 * Replicates master's physmem_fd content into a fresh memfd, swaps
 * the kernel-side MAP_SHARED mapping over to the new fd, and
 * updates the global physmem_fd so that:
 *
 *   - Subsequent kernel slab/page-allocator writes land in the
 *     child-private memfd.
 *   - phys_mapping() returns the new fd, so new stubs spawned by
 *     start_userspace_fresh() also mmap the new fd (kernel↔stub
 *     coherence within the member preserved by both ends using
 *     MAP_SHARED on the same fd).
 *   - Master's physmem_fd and master's existing stubs are
 *     untouched (they continue using the original fd).
 *
 * MUST be called from the pool-member child entry, AFTER Path A
 * pivot (private kernel stack so the MAP_FIXED remap doesn't
 * unmap the page we're running on) and BEFORE any kernel slab
 * write that would otherwise leak back to master.
 *
 * Returns 0 on success, -errno on failure (kernel still mapped to
 * the original physmem_fd; caller continues but iters > 1 will
 * crash via aliasing).
 */
int um_pool_replicate_physmem(void)
{
	void *scratch;
	int new_fd, old_fd, ret;
	unsigned long long phys_off;
	unsigned long flags;

	if (physmem_fd < 0)
		return -EINVAL;
	if (!physmem_mmap_region.base || !physmem_mmap_region.len)
		return -EINVAL;

	/* Block UML's own SIGALRM delivery for the duration — the
	 * mmap-FIXED swap below has a brief window where the host
	 * kernel transitions VMAs; if SIGALRM interrupted mid-swap
	 * and the handler touched the remapped VA, we'd risk a
	 * spurious fault.  block_signals_hard() blocks SIGALRM,
	 * SIGIO, SIGVTALRM, SIGUSR1 — exactly the IRQ-class signals
	 * UML uses to drive its scheduler tick.
	 */
	local_irq_save(flags);

	new_fd = os_create_memfd("um-pool-physmem", physmem_size);
	if (new_fd < 0) {
		local_irq_restore(flags);
		return new_fd;
	}

	/* mmap the new fd as a scratch VA so we can populate it
	 * from the kernel's existing physmem-VA mapping (which is
	 * still backed by master's physmem_fd at this point).
	 */
	ret = os_mmap_rw_scratch(new_fd, 0, physmem_size, &scratch);
	if (ret < 0) {
		os_close_file(new_fd);
		return ret;
	}

	/* Replicate master's full physmem content into the new
	 * memfd.  The source VA is the kernel-side mapping of
	 * physmem_fd, which spans __pa(base) .. __pa(base)+len.
	 * Bytes outside that range (the kernel image region at
	 * offset 0..reserve) come from physmem_fd via a direct
	 * read.
	 */
	phys_off = (unsigned long long)__pa((unsigned long)
					    physmem_mmap_region.base);
	memcpy((char *)scratch + phys_off, physmem_mmap_region.base,
	       physmem_mmap_region.len);

	if (phys_off > 0) {
		/* Read [0..phys_off) from master's fd into scratch.
		 * Holds kernel image bytes + the syscall_stub_start
		 * page that setup_physmem() wrote there at boot.
		 */
		ret = os_seek_file(physmem_fd, 0);
		if (ret == 0)
			ret = os_read_file(physmem_fd, scratch, phys_off);
		if (ret < 0) {
			os_unmap_memory(scratch, physmem_size);
			os_close_file(new_fd);
			return ret;
		}
	}

	if (os_unmap_memory(scratch, physmem_size) < 0) {
		os_close_file(new_fd);
		local_irq_restore(flags);
		return -EFAULT;
	}

	/* Swap the kernel-side mapping to the new fd at the same
	 * VA + offset.  MAP_SHARED preserves kernel↔stub coherence
	 * within this member (the stub uses MAP_SHARED, so both
	 * ends mmap the same backing fd).
	 */
	ret = os_remap_region_shared(physmem_mmap_region.base, new_fd,
				     phys_off, physmem_mmap_region.len);
	if (ret < 0) {
		os_close_file(new_fd);
		local_irq_restore(flags);
		return ret;
	}

	old_fd = physmem_fd;
	physmem_fd = new_fd;
	physmem_mmap_region.backing_fd = new_fd;

	/* DON'T close old_fd for now — leaving it open in case
	 * something still references it.  Will revisit fd hygiene
	 * once sustained-smoke is stable.
	 */
	(void)old_fd;

	local_irq_restore(flags);
	return 0;
}
EXPORT_SYMBOL_GPL(um_pool_replicate_physmem);
#endif /* CONFIG_UM_SNAPSHOT_FORKSERVER */

static int __init uml_mem_setup(char *line, int *add)
{
	char *retptr;

	*add = 0;
	physmem_size = memparse(line,&retptr);
	return 0;
}
__uml_setup("mem=", uml_mem_setup,
"mem=<Amount of desired ram>\n"
"    This controls how much \"physical\" memory the kernel allocates\n"
"    for the system. The size is specified as a number followed by\n"
"    one of 'k', 'K', 'm', 'M', which have the obvious meanings.\n"
"    This is not related to the amount of memory in the host.  It can\n"
"    be more, and the excess, if it's ever used, will just be swapped out.\n"
"	Example: mem=64M\n\n"
);
