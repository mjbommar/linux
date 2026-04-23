// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — mm ops.
 *
 * Workstream D-03b: mm_attach / mm_detach. They're per-UML-mm
 * bookkeeping hooks that refcount the single shared KVM VM
 * created in kvm_init() — see decisions-log D57 for why the
 * backend uses one VM fd per UML kernel process rather than one
 * per UML mm_struct. No KVM ioctls happen here today; every UML
 * guest mm shares the same vm_fd and is distinguished at
 * run-time by CR3 (D-04).
 *
 * mm_map / mm_unmap are still -EOPNOTSUPP stubs in stubs.c; they
 * land in D-03c with KVM_SET_USER_MEMORY_REGION plumbing.
 */
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/refcount.h>

#include <skas/mm_id.h>
#include <asm/backend.h>

#include "kvm_backend.h"

int kvm_mm_attach(struct mm_id *id)
{
	struct kvm_um *ctx = kvm_backend_ctx();

	(void)id;

	if (ctx->vm_fd < 0) {
		pr_err("um: kvm mm_attach before init — vm_fd unset\n");
		return -EIO;
	}

	/*
	 * First attach initializes the refcount (kvm_init leaves it
	 * at 0 since no mm is attached yet); subsequent attaches
	 * increment. refcount_inc() would WARN on 0→1, so split the
	 * first-attach case explicitly.
	 */
	if (refcount_read(&ctx->mm_refcount) == 0)
		refcount_set(&ctx->mm_refcount, 1);
	else
		refcount_inc(&ctx->mm_refcount);

	return 0;
}

void kvm_mm_detach(struct mm_id *id)
{
	struct kvm_um *ctx = kvm_backend_ctx();

	(void)id;

	if (ctx->vm_fd < 0)
		return;

	/*
	 * refcount_dec() WARNs on 1→0; use refcount_dec_and_test()
	 * so hitting zero is a valid bookkeeping transition rather
	 * than a bug. Shutdown still closes vm_fd; this refcount is
	 * for "don't close under an attached mm" not ownership
	 * transfer.
	 */
	if (refcount_read(&ctx->mm_refcount) > 0)
		refcount_dec_and_test(&ctx->mm_refcount);
}
