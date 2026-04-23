// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — mm ops.
 *
 * Workstream D-03b: mm_attach / mm_detach — per-UML-mm
 * bookkeeping hooks that refcount the single shared KVM VM
 * created in kvm_init() per decisions-log D57.
 * Workstream D-03d: mm_map / mm_unmap — host-side mmap / munmap
 * against UML's own process VA per the 03b-memslot-policy.md
 * Policy A decision (the KVM memslot registered in kvm_init()
 * already covers the whole UML address space, so these ops do
 * not touch KVM ioctls on the hot path).
 *
 * None of the four ops issue KVM ioctls: memslot state is
 * static after init, guest-vs-host isolation is handled by
 * CR3 switching (D-04).
 */
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/refcount.h>
#include <linux/types.h>

#include <os.h>
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

/*
 * Policy A (03b-memslot-policy.md): the KVM memslot registered in
 * kvm_init() covers the whole UML address space, so mm_map /
 * mm_unmap need no KVM ioctls. They reduce to host-side mmap /
 * munmap against UML's own process VA — exactly what `kern_map`
 * in arch/um/kernel/tlb.c already does for the init_mm path.
 * We just call through the same os_map_memory() /
 * os_unmap_memory() helpers for user mms too.
 *
 * Note on phys_fd: ptrace / seccomp translate phys_fd into the
 * stub child's fd namespace via get_stub_fd() before queueing
 * the mmap. For the KVM backend there is no stub child — phys_fd
 * is already an fd in UML's own process table, so no translation
 * is needed.
 */
int kvm_mm_map(struct mm_id *id, unsigned long virt, unsigned long len,
	       int prot, int phys_fd, u64 offset)
{
	(void)id;

	return os_map_memory((void *)virt, phys_fd, offset, len,
			     prot & UM_PROT_READ, prot & UM_PROT_WRITE,
			     prot & UM_PROT_EXEC);
}

int kvm_mm_unmap(struct mm_id *id, unsigned long virt, unsigned long len)
{
	(void)id;

	return os_unmap_memory((void *)virt, len);
}
