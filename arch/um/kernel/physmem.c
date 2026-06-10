// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/module.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <linux/thread_info.h>
#include <asm/page.h>
#include <asm/sections.h>
#include <asm/um-snapshot.h>
#include <as-layout.h>
#include <init.h>
#include <kern.h>
#include <kern_util.h>
#include <mem_user.h>
#include <os.h>
#ifdef CONFIG_UM_SNAPSHOT_FORKSERVER
#include <asm/um-mmaps.h>
#include <linux/smp-internal.h>	/* cpu_irqstacks */
#endif

static int physmem_fd = -1;

/* Changed during early boot */
unsigned long high_physmem;
EXPORT_SYMBOL(high_physmem);

#ifdef CONFIG_UM_SNAPSHOT_FORKSERVER
/*
 * Kernel mmap registry entry for the current kernel-VA guest-RAM window.
 * setup_physmem() initializes it during boot. Pool-member physmem
 * replication updates it after swapping the backing fd, because
 * arch_mm_preinit() lowers uml_reserved after the initial registration.
 * Static storage lets the registry's list linkage outlive init-time setup.
 *
 * INHERIT_COW: guest RAM is MAP_SHARED file-backed; fork() gives the
 * child a COW view of the same file-backed pages. SERIALIZE marks
 * the region for snapshot code that reads physmem_fd directly rather
 * than walking pagemap.
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
			pr_err("try increasing the host's /proc/sys/vm/max_map_count to <physical memory size>/4096\n");
		panic("%s(0x%lx, %d, 0x%llx, %ld, %d, %d, %d) failed, err = %d\n",
		      __func__, virt, fd, offset, len, r, w, x, err);
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
 * Creates an unlinked backing file of size (len) and memory maps
 * it on the last executable image address (uml_reserved).
 *
 * The offset is needed as the length of the total physical memory
 * (len) includes the size of the memory used be the executable image,
 * but the mapped-to address is the last address of the executable image
 * (uml_reserved == end address of executable image).
 *
 * The mmap of that backing file supplies memory for all user space
 * processes/kernel tasks.
 *
 * Contract: physmem_fd must
 * be a regular host file, not an anonymous mapping, and the range
 * mapped by os_map_memory() below must use MAP_SHARED. Both properties
 * are provided here: create_mem_file() -> create_tmp_file() returns a
 * real tempfile in tmpfs, and os_map_memory() always maps with
 * MAP_SHARED | MAP_FIXED (arch/um/os-Linux/process.c:96). Do not
 * silently flip either property without checking snapshot/forkserver
 * callers. Snapshot serialization depends on being able to write out
 * guest RAM by reading this one fd, and the fork-server worker depends on
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
		os_warn("%s - mapping %lu bytes of memory at 0x%p failed - errno = %d\n",
			__func__, map_size, (void *)reserve_end, err);
		exit(1);
	}

	/*
	 * This page is mapped into userspace processes from physmem_fd,
	 * so it needs to be written out there.
	 */
	os_seek_file(physmem_fd, __pa(__syscall_stub_start));
	os_write_file(physmem_fd, __syscall_stub_start, PAGE_SIZE);

	memblock_add(__pa(start), len);
	memblock_reserve(__pa(start), reserve);

	min_low_pfn = PFN_UP(__pa(reserve_end));
	max_low_pfn = min_low_pfn + (map_size >> PAGE_SHIFT);

#ifdef CONFIG_UM_SNAPSHOT_FORKSERVER
	/*
	 * Register the guest-RAM region with the mmap registry.
	 * Consumers: the fork-server worker reinit path and snapshot
	 * serialization, which reads this region out of the backing
	 * file instead of walking pagemap.
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
 * um_pool_remap_self_test() - remap the kernel-VA physmem region with
 * the same fd at the same offset. The content is unchanged; callers use
 * this to isolate whether MAP_FIXED remapping itself disrupts signal
 * delivery, independent of any fd swap or content change.
 *
 * Returns 0 on success, -errno on failure.
 */
int um_pool_remap_self_test(void)
{
	void *base = (void *)uml_reserved;
	unsigned long len;

	if (physmem_fd < 0)
		return -EINVAL;
	if (high_physmem <= uml_reserved)
		return -EINVAL;
	len = high_physmem - uml_reserved;

	return os_remap_region_shared(base, physmem_fd,
				      (unsigned long long)__pa(uml_reserved),
				      len);
}
EXPORT_SYMBOL_GPL(um_pool_remap_self_test);

/**
 * um_pool_replicate_physmem() - give a forked pool member private physmem backing.
 *
 * Copies the current physmem contents into a new backing file, remaps
 * the kernel physmem area to that file, and updates phys_mapping() to
 * hand the new fd to subsequently spawned stubs.  The master process
 * and its existing stubs keep using their original physmem fd.
 *
 * Call after the pool member has switched to a private kernel stack and
 * before it resumes normal kernel allocation.
 *
 * Return: 0 on success or a negative errno on failure.
 */
int um_pool_replicate_physmem(void)
{
	void *scratch;
	void *base = (void *)uml_reserved;
	int new_fd, ret;
	unsigned long long phys_off;
	unsigned long len;
	unsigned long flags;

	if (physmem_fd < 0)
		return -EINVAL;
	if (high_physmem <= uml_reserved)
		return -EINVAL;
	len = high_physmem - uml_reserved;

	local_irq_save(flags);

	/*
	 * Use O_TMPFILE on /dev/shm or /tmp to match setup_physmem's
	 * boot-time physmem_fd shape exactly.
	 */
	new_fd = os_create_tmpfile("/dev/shm", physmem_size);
	if (new_fd < 0)
		new_fd = os_create_tmpfile("/tmp", physmem_size);
	if (new_fd < 0) {
		local_irq_restore(flags);
		return new_fd;
	}

	/*
	 * mmap the new fd as a scratch VA so we can populate it
	 * from the kernel's existing physmem-VA mapping (which is
	 * still backed by master's physmem_fd at this point).
	 */
	ret = os_mmap_rw_scratch(new_fd, 0, physmem_size, &scratch);
	if (ret < 0) {
		os_close_file(new_fd);
		local_irq_restore(flags);
		return ret;
	}

	phys_off = (unsigned long long)__pa((unsigned long)base);
	memcpy((char *)scratch + phys_off, base, len);

	if (phys_off > 0) {
		ret = os_seek_file(physmem_fd, 0);
		if (ret == 0)
			ret = os_read_file(physmem_fd, scratch, phys_off);
		if (ret < 0) {
			os_unmap_memory(scratch, physmem_size);
			os_close_file(new_fd);
			local_irq_restore(flags);
			return ret;
		}
	}

	if (os_unmap_memory(scratch, physmem_size) < 0) {
		os_close_file(new_fd);
		local_irq_restore(flags);
		return -EFAULT;
	}

	/*
	 * Install the new file through an intermediate anonymous mapping.
	 * This preserves host signal delivery while replacing the backing
	 * inode for the kernel physmem area.
	 */
	ret = os_remap_region_via_anon(base, new_fd, phys_off, len);
	if (ret < 0) {
		os_close_file(new_fd);
		local_irq_restore(flags);
		return ret;
	}

	physmem_fd = new_fd;
	physmem_mmap_region.base = base;
	physmem_mmap_region.len = len;
	physmem_mmap_region.backing_fd = new_fd;

	local_irq_restore(flags);
	return 0;
}
EXPORT_SYMBOL_GPL(um_pool_replicate_physmem);
#endif /* CONFIG_UM_SNAPSHOT_FORKSERVER */

static int __init uml_mem_setup(char *line, int *add)
{
	char *retptr;

	*add = 0;
	physmem_size = memparse(line, &retptr);
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
