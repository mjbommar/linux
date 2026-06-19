// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011 Richard Weinberger <richrd@nod.at>
 */

#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/timekeeping.h>
#include <asm/page.h>
#include <asm/elf.h>
#include <linux/init.h>
#include <kern_util.h>
#include "um_vdso.h"

unsigned long um_vdso_addr;
static struct page *um_vdso;
static struct page *um_vdso_data_page;
/* The data page is mapped immediately below the vDSO code page. */
static struct page *um_vdso_pages[2];
static struct um_vdso_data *um_vdso_data;

extern unsigned long task_size;
extern char vdso_start[], vdso_end[];

/* Called from the timer tick (arch/um/kernel/time.c) when not time-travelling. */
void um_vdso_update_coarse(void)
{
	struct um_vdso_data *vd = um_vdso_data;
	struct timespec64 m, r;

	if (!vd)
		return;

	ktime_get_coarse_ts64(&m);
	ktime_get_coarse_real_ts64(&r);

	WRITE_ONCE(vd->seq, vd->seq + 1);	/* -> odd */
	/* Publish the odd seq before the fields the reader copies. */
	smp_wmb();
	vd->mono_coarse_sec  = m.tv_sec;
	vd->mono_coarse_nsec = m.tv_nsec;
	vd->real_coarse_sec  = r.tv_sec;
	vd->real_coarse_nsec = r.tv_nsec;
	/* Make the fields visible before the even seq that validates them. */
	smp_wmb();
	vd->valid = 1;
	WRITE_ONCE(vd->seq, vd->seq + 1);	/* -> even */
}

static int __init init_vdso(void)
{
	BUG_ON(vdso_end - vdso_start > PAGE_SIZE);

	um_vdso_addr = task_size - PAGE_SIZE;

	um_vdso = alloc_page(GFP_KERNEL);
	if (!um_vdso)
		panic("Cannot allocate vdso\n");

	copy_page(page_address(um_vdso), vdso_start);

	um_vdso_data_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!um_vdso_data_page)
		panic("Cannot allocate vdso data page\n");
	um_vdso_data = page_address(um_vdso_data_page);

	um_vdso_pages[0] = um_vdso_data_page;	/* lower address */
	um_vdso_pages[1] = um_vdso;		/* vDSO code (== um_vdso_addr) */

	return 0;
}
subsys_initcall(init_vdso);

int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;
	static struct vm_special_mapping vdso_mapping = {
		.name = "[vdso]",
		.pages = um_vdso_pages,
	};

	if (mmap_write_lock_killable(mm))
		return -EINTR;

	/* Map [data page][vDSO code page]; the ELF base stays um_vdso_addr. */
	vma = _install_special_mapping(mm, um_vdso_addr - PAGE_SIZE, 2 * PAGE_SIZE,
		VM_READ|VM_EXEC|
		VM_MAYREAD|VM_MAYWRITE|VM_MAYEXEC,
		&vdso_mapping);

	mmap_write_unlock(mm);

	return IS_ERR(vma) ? PTR_ERR(vma) : 0;
}
