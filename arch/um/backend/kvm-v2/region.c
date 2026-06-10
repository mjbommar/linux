// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend memory-region hooks.
 *
 * KVM v2 uses the giant physmem memslot installed by context.c for the
 * actual TDP coverage. These hooks serialize the mm update with UML's
 * turnstile but do not create per-region KVM memslots.
 */

#include <linux/errno.h>
#include <linux/mm_types.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <asm/um_memory.h>
#include <skas/mm_id.h>		/* enter_turnstile / exit_turnstile */

#include "kvm_v2_backend.h"

static int kvm_v2_region_begin(struct mm_struct *mm,
			       const struct um_memory_region *region,
			       const char *op, struct mm_id **mm_id)
{
	if (!kvm_v2_vm_get()) {
		pr_warn_ratelimited("um: kvm-v2 %s: VM not initialised\n", op);
		return -ENODEV;
	}
	if (!mm || !region || !region->len)
		return -EINVAL;

	*mm_id = &mm->context.id;
	enter_turnstile(*mm_id);
	return 0;
}

int kvm_v2_mm_region_added(struct mm_struct *mm,
			   const struct um_memory_region *region)
{
	struct mm_id *mm_id;
	int rc;

	rc = kvm_v2_region_begin(mm, region, "region_added", &mm_id);
	if (rc)
		return rc;

	/* Slot 0 covers UML physical pages; per-region KVM slots are unused. */
	exit_turnstile(mm_id);
	return 0;
}

int kvm_v2_mm_region_removed(struct mm_struct *mm,
			     const struct um_memory_region *region)
{
	struct mm_id *mm_id;
	int rc;

	rc = kvm_v2_region_begin(mm, region, "region_removed", &mm_id);
	if (rc)
		return rc;

	/* No per-region KVM state was installed by region_added(). */
	exit_turnstile(mm_id);
	return 0;
}
