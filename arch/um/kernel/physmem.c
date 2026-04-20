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
