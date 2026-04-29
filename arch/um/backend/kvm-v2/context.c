// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase A.2: per-VM context lifecycle.
 *
 * Per memo 26 §A.2. One struct kvm_v2_vm per UML kernel invocation
 * (UML is single-process, so a file-scope static is correct here —
 * matches the v1 archive's kvm_ctx singleton). Phase B.x populates
 * the memslots list; A.3 attaches the placeholder vCPU.
 *
 * Why CPUID is NOT installed here despite memo 26 §A.2's prose:
 *   KVM_SET_CPUID2 is a vCPU ioctl — it can only be issued against
 *   an fd from KVM_CREATE_VCPU, and even KVM_GET_SUPPORTED_CPUID
 *   (which is a KVM-fd ioctl and could run here) needs a kzalloc
 *   buffer. kvm_v2_init runs from init_backend() during linux_main,
 *   BEFORE mm_init() brings the buddy allocator up — kzalloc returns
 *   NULL here. v1 archive's lifecycle.c documents the same
 *   constraint (see kvm_ensure_cpuid_done's "Deferred out of
 *   kvm_init()" comment) and defers CPUID entirely to first KVM_RUN.
 *   v2 takes the same approach: A.3 owns CPUID query + curate +
 *   install on its placeholder vCPU. A.2 only declares the field
 *   on struct kvm_v2_vm; the curated-mask logic lives where it
 *   actually runs.
 *
 * Why TSS_ADDR + IDENTITY_MAP_ADDR live here:
 *   KVM Intel's unrestricted-guest mode requires both before the VM
 *   is runnable; both are VM ioctls. v1 ran on an older kernel that
 *   didn't trip the requirement (its sregs setup avoided it via
 *   long-mode-from-the-start), but memo 26 §A.2 calls them out
 *   explicitly because Phase B's TDP path will go through real-mode
 *   bring-up before the guest enters long mode.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <os.h>

#include "kvm_v2_backend.h"

/*
 * KVM Intel unrestricted-guest plumbing addresses. Values mirror the
 * defaults qemu uses (qemu/target/i386/kvm/kvm.c) — three pages of
 * TSS at 0xfffbd000-0xfffbffff and the identity-map PT at 0xfffbc000,
 * tucked just below the BIOS region so they don't collide with any
 * realistic guest memslot.
 */
#define KVM_V2_TSS_ADDR		0xfffbd000UL
#define KVM_V2_IDENTITY_ADDR	0xfffbc000UL

static struct kvm_v2_vm vm = {
	.kvm_fd = -1,
	.vm_fd  = -1,
};

int kvm_v2_vm_create(int kvm_fd, u64 caps)
{
	int vm_fd, rc;

	if (vm.vm_fd >= 0) {
		pr_warn("um: kvm-v2 vm_create: already created (vm_fd=%d)\n",
			vm.vm_fd);
		return -EBUSY;
	}

	/*
	 * machine-type 0 = default x86_64 long-mode VM (matches v1
	 * archive's choice; per-process, shared across every UML guest
	 * mm — separate UML address spaces are isolated via CR3 switch
	 * on context_switch, not separate VMs. Decisions-log D57.)
	 */
	vm_fd = os_ioctl_generic(kvm_fd, KVM_CREATE_VM, 0);
	if (vm_fd < 0) {
		pr_err("um: kvm-v2 vm_create: KVM_CREATE_VM failed (%d)\n",
		       vm_fd);
		return vm_fd;
	}

	/*
	 * Order matters: TSS_ADDR + IDENTITY_MAP_ADDR must be set
	 * before any vCPU exists or any memslot is registered, because
	 * KVM Intel rejects them once the VM has run state attached.
	 */
	rc = os_ioctl_generic(vm_fd, KVM_SET_TSS_ADDR, KVM_V2_TSS_ADDR);
	if (rc < 0) {
		pr_err("um: kvm-v2 vm_create: KVM_SET_TSS_ADDR(%#lx) failed (%d)\n",
		       KVM_V2_TSS_ADDR, rc);
		goto err_close_vm;
	}

	{
		__u64 ident = KVM_V2_IDENTITY_ADDR;

		rc = os_ioctl_generic(vm_fd, KVM_SET_IDENTITY_MAP_ADDR,
				      (unsigned long)&ident);
		if (rc < 0) {
			pr_err("um: kvm-v2 vm_create: KVM_SET_IDENTITY_MAP_ADDR(%#llx) failed (%d)\n",
			       ident, rc);
			goto err_close_vm;
		}
	}

	vm.kvm_fd = kvm_fd;
	vm.vm_fd  = vm_fd;
	vm.caps   = caps;
	vm.cpuid  = NULL;	/* A.3 owns CPUID query + install. */
	INIT_LIST_HEAD(&vm.memslots);
	/*
	 * Memslot id bitmap (Phase B.1). Zero on create — even though
	 * the static is zero-initialised at boot, explicit clear keeps
	 * the create path correct if a future shutdown→re-init flow
	 * lands without re-zeroing the static.
	 */
	bitmap_zero(vm.memslot_bitmap, KVM_V2_MAX_USER_MEM_SLOTS);
	spin_lock_init(&vm.lock);

	pr_info("um: kvm-v2 vm_create: vm_fd=%d caps=%#llx tss=%#lx ident=%#lx (memslots empty; A.3 attaches vCPU + installs CPUID)\n",
		vm.vm_fd, vm.caps, KVM_V2_TSS_ADDR, KVM_V2_IDENTITY_ADDR);
	return 0;

err_close_vm:
	os_close_file(vm_fd);
	return rc;
}

void kvm_v2_vm_destroy(void)
{
	struct kvm_v2_memslot *m, *tmp;

	if (vm.vm_fd < 0)
		return;

	/*
	 * Drain the memslot list. B.1 ships the allocator/list/lookup
	 * but does not yet populate the list (B.2's mm_region_added
	 * wiring is the first caller of kvm_v2_memslot_add). The drain
	 * is the correct teardown order regardless: once B.2 lands,
	 * vm_destroy must free every outstanding entry, and the loop
	 * keeps that responsibility on this destroy path rather than
	 * spreading it across B.2-B.6 each time a new caller is added.
	 *
	 * No KVM_SET_USER_MEMORY_REGION(size=0) here — that's B.3's
	 * responsibility once mm_region_removed wires it. At
	 * vm_destroy time the VM fd itself is about to close, which
	 * tears down all slots in the kernel anyway, so a per-slot
	 * delete ioctl would be redundant.
	 */
	list_for_each_entry_safe(m, tmp, &vm.memslots, list)
		kvm_v2_memslot_del(&vm, m->slot_id);

	kfree(vm.cpuid);
	vm.cpuid = NULL;

	pr_info("um: kvm-v2 vm_destroy: closing vm_fd=%d kvm_fd=%d\n",
		vm.vm_fd, vm.kvm_fd);

	/*
	 * Close VM fd before /dev/kvm fd: the VM fd is a child of the
	 * KVM fd in the kernel-side reference graph, so closing in the
	 * other order would leak the VM until the parent fd's release
	 * runs.
	 */
	os_close_file(vm.vm_fd);
	if (vm.kvm_fd >= 0)
		os_close_file(vm.kvm_fd);
	vm.vm_fd  = -1;
	vm.kvm_fd = -1;
	vm.caps   = 0;
}

struct kvm_v2_vm *kvm_v2_vm_get(void)
{
	return vm.vm_fd >= 0 ? &vm : NULL;
}
