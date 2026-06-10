// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend per-VM context lifecycle.
 *
 * UML runs as one host process, so the KVM backend owns one file-scope
 * struct kvm_v2_vm for the lifetime of the UML kernel. This file creates
 * and destroys the VM fd, registers the giant physmem memslot, and ties
 * together the deferred trampoline, descriptor, and page-table helpers.
 *
 * CPUID is installed from the vCPU path. KVM_SET_CPUID2 is a vCPU ioctl,
 * and the supported-CPUID query needs allocations that are not available
 * during init_backend().
 *
 * KVM_SET_TSS_ADDR and KVM_SET_IDENTITY_MAP_ADDR are VM ioctls and must
 * be issued before any vCPU or memslot exists, so they belong in
 * kvm_v2_vm_create().
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <as-layout.h>		/* physmem_size, uml_physmem */
#include <os.h>

#include <asm/trace/um_backend.h>

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

/*
 * KVM Intel unrestricted-guest plumbing addresses. Values mirror the
 * defaults qemu uses (qemu/target/i386/kvm/kvm.c): three pages of
 * TSS at 0xfffbd000-0xfffbffff and the identity-map PT at 0xfffbc000,
 * just below the BIOS region.
 */
#define KVM_V2_TSS_ADDR		0xfffbd000UL
#define KVM_V2_IDENTITY_ADDR	0xfffbc000UL

static struct kvm_v2_vm vm = {
	.kvm_fd              = -1,
	.vm_fd               = -1,
	.physmem_memslot_id  = -1,
};

/*
 * Install the physmem identity-offset memslot.
 *
 * kvm_v2_load_cr3 writes __pa(active_mm->pgd) to CR3.
 * Under UML __pa(kva) = kva - uml_physmem, so the GPA is an offset
 * inside physmem (in [0, physmem_size)), not a host VA. KVM's TDP walks
 * the pgd at this GPA and needs a memslot covering this range. The
 * region hooks do not install per-mapping KVM slots; slot 0 covers the
 * pgd, PUD/PMD/PTE pages, and trampoline pages that live in physmem.
 *
 * One slot covers everything in physmem: PT chain pages allocated from
 * buddy come from physmem; alloc_page -> page_address -> __pa = offset
 * in [0, physmem_size); the memslot
 * translates back to userspace_addr = uml_physmem + offset = the
 * original kva. KVM fault-in resolves through current->mm (the
 * spawner) which has uml_physmem mapped from the very early boot path.
 *
 * Guest pgd PTE values reference UML-physical addresses (offsets in
 * physmem). KVM's GVA->GPA walk follows the guest pgd to those
 * physmem-offset GPAs; the physmem memslot translates them to host
 * pages.
 *
 * Idempotent: physmem_memslot_id >= 0 means the slot is installed. The
 * install is reachable from VM creation and from the late-install retry,
 * so the slot id is the single source of truth for teardown.
 *
 * Returns 0 on success / already-installed, -EAGAIN if uml_physmem /
 * physmem_size aren't yet populated (vm_create runs from init_backend
 * which fires before linux_main finishes setting those globals), or a
 * negative ioctl errno on KVM_SET_USER_MEMORY_REGION failure.
 */
static int kvm_v2_physmem_globals_ready(void)
{
	if (!uml_physmem || !physmem_size) {
		/*
		 * vm_create runs from init_backend() before linux_main populates
		 * uml_physmem / physmem_size. Log once and let the late retry path
		 * install the slot after the globals are stable.
		 */
		pr_debug("um: kvm-v2 physmem_memslot: deferred (uml_physmem=%#lx physmem_size=%#llx not ready)\n",
			 uml_physmem, physmem_size);
		return -EAGAIN;
	}

	return 0;
}

static int kvm_v2_add_physmem_memslot(struct kvm_v2_vm *vmctx)
{
	int slot_id;

	slot_id = kvm_v2_memslot_add(vmctx, 0, uml_physmem, physmem_size, 0);
	if (slot_id < 0) {
		pr_err("um: kvm-v2 physmem_memslot: memslot_add failed (%d)\n",
		       slot_id);
	}

	return slot_id;
}

static int kvm_v2_set_physmem_region(struct kvm_v2_vm *vmctx, int slot_id)
{
	struct kvm_userspace_memory_region kr = {
		.slot		 = (u32)slot_id,
		.flags		 = 0,
		.guest_phys_addr = 0,
		.memory_size	 = physmem_size,
		.userspace_addr	 = uml_physmem,
	};
	int rc;

	rc = os_ioctl_generic(vmctx->vm_fd, KVM_SET_USER_MEMORY_REGION,
			      (unsigned long)&kr);
	if (rc < 0) {
		pr_err("um: kvm-v2 physmem_memslot: KVM_SET_USER_MEMORY_REGION(slot=%d hva=%#lx size=%#llx) failed (%d)\n",
		       slot_id, uml_physmem, physmem_size, rc);
		kvm_v2_memslot_del(vmctx, (u32)slot_id);
		return rc;
	}

	return 0;
}

static void kvm_v2_publish_physmem_memslot(struct kvm_v2_vm *vmctx,
					   int slot_id)
{
	vmctx->physmem_memslot_id = slot_id;
	pr_debug("um: kvm-v2 physmem_memslot: slot=%d gpa=0 hva=%#lx size=%#llx\n",
		 slot_id, uml_physmem, physmem_size);
	trace_um_backend_kvm_v2_physmem_memslot_install(slot_id, uml_physmem,
							physmem_size);
}

int kvm_v2_physmem_memslot_install(struct kvm_v2_vm *vmctx)
{
	int slot_id, rc;

	if (!vmctx || vmctx->vm_fd < 0)
		return -EINVAL;

	if (vmctx->physmem_memslot_id >= 0) {
		/* Already installed: idempotent short-circuit. */
		return 0;
	}

	rc = kvm_v2_physmem_globals_ready();
	if (rc)
		return rc;

	slot_id = kvm_v2_add_physmem_memslot(vmctx);
	if (slot_id < 0)
		return slot_id;

	rc = kvm_v2_set_physmem_region(vmctx, slot_id);
	if (rc)
		return rc;

	kvm_v2_publish_physmem_memslot(vmctx, slot_id);
	return 0;
}

static int kvm_v2_create_vm_fd(int kvm_fd)
{
	/*
	 * machine-type 0 = default x86_64 long-mode VM. UML address spaces
	 * are isolated via CR3 switch on context_switch, not separate VMs.
	 */
	int vm_fd = os_ioctl_generic(kvm_fd, KVM_CREATE_VM, 0);

	if (vm_fd < 0) {
		pr_err("um: kvm-v2 vm_create: KVM_CREATE_VM failed (%d)\n",
		       vm_fd);
	}
	return vm_fd;
}

static int kvm_v2_vm_install_required_ioctls(int vm_fd)
{
	__u64 ident = KVM_V2_IDENTITY_ADDR;
	int rc;

	/*
	 * Order matters: TSS_ADDR + IDENTITY_MAP_ADDR must be set
	 * before any vCPU exists or any memslot is registered, because
	 * KVM Intel rejects them once the VM has run state attached.
	 */
	rc = os_ioctl_generic(vm_fd, KVM_SET_TSS_ADDR, KVM_V2_TSS_ADDR);
	if (rc < 0) {
		pr_err("um: kvm-v2 vm_create: KVM_SET_TSS_ADDR(%#lx) failed (%d)\n",
		       KVM_V2_TSS_ADDR, rc);
		return rc;
	}

	rc = os_ioctl_generic(vm_fd, KVM_SET_IDENTITY_MAP_ADDR,
			      (unsigned long)&ident);
	if (rc < 0) {
		pr_err("um: kvm-v2 vm_create: KVM_SET_IDENTITY_MAP_ADDR(%#llx) failed (%d)\n",
		       ident, rc);
		return rc;
	}

	return 0;
}

static void kvm_v2_vm_enable_optional_caps(int vm_fd)
{
#ifdef CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH
	/*
	 * APERF/MPERF MSR passthrough is a VM capability that must be
	 * enabled before any vCPU exists. If the host lacks
	 * X86_FEATURE_APERFMPERF, KVM rejects the ioctl and the guest keeps
	 * KVM's default zero-counter behavior. See
	 * Documentation/virt/uml/aperf-mperf.rst.
	 */
	if (kvm_v2_aperfmperf_enabled()) {
		struct kvm_enable_cap cap = {
			.cap     = KVM_CAP_X86_DISABLE_EXITS,
			.args[0] = KVM_X86_DISABLE_EXITS_APERFMPERF,
		};
		int erc;

		erc = os_ioctl_generic(vm_fd, KVM_ENABLE_CAP,
				       (unsigned long)&cap);
		kvm_v2_aperfmperf_record_ioctl(erc);
		if (erc < 0)
			pr_warn("um: kvm-v2: APERF/MPERF passthrough unavailable (%d); guest MSRs read zero\n",
				erc);
		else
			pr_info("um: kvm-v2: APERF/MPERF MSR passthrough enabled\n");
	}
#endif
}

static void kvm_v2_vm_reset_runtime_state(void)
{
	vm.cpuid = NULL;
	INIT_LIST_HEAD(&vm.memslots);
	bitmap_zero(vm.memslot_bitmap, KVM_V2_MAX_USER_MEM_SLOTS);
	spin_lock_init(&vm.lock);

	vm.trampoline_page = NULL;
	vm.trampoline_gpa = 0;
	vm.physmem_memslot_id = -1;
	vm.trampoline_pud_kva = NULL;
	vm.trampoline_pmd_kva = NULL;
	vm.trampoline_pte_kva = NULL;
	vm.trampoline_pud_gpa = 0;

	vm.idt_kva = NULL;
	vm.idt_gpa = 0;
	vm.handlers_kva = NULL;
	vm.handlers_gpa = 0;
	vm.gdt_kva = NULL;
	vm.gdt_gpa = 0;
}

static void kvm_v2_vm_init_state(int kvm_fd, int vm_fd, u64 caps)
{
	kvm_v2_vm_reset_runtime_state();
	vm.kvm_fd = kvm_fd;
	vm.vm_fd = vm_fd;
	vm.caps = caps;
}

static int kvm_v2_vm_install_deferred_state(void)
{
	int rc;

	/*
	 * Install the giant physmem identity-offset memslot before the
	 * trampoline alloc. KVM's TDP needs a memslot covering the physmem
	 * range so __pa(pgd) and __pa(trampoline_kva) resolve.
	 *
	 * This can defer at vm_create time, exactly like the trampoline alloc
	 * below: linux_main() runs init_backend() before it sets
	 * uml_physmem / physmem_size. The helper returns -EAGAIN in that
	 * window; the subsys_initcall lazy retry in syscall_trap.c completes
	 * the install once the globals are populated. Other negative errnos
	 * unwind via err_close_vm.
	 *
	 * Order vs TSS_ADDR/IDENTITY_MAP_ADDR (above): KVM Intel rejects
	 * those two ioctls once any memslot exists, so memslot install
	 * must follow them. Order vs trampoline alloc (below): the trampoline
	 * page must be in the slot before any guest walk through the
	 * kernel-half PT chain.
	 */
	rc = kvm_v2_physmem_memslot_install(&vm);
	if (rc && rc != -EAGAIN) {
		pr_err("um: kvm-v2 vm_create: physmem_memslot_install failed (%d)\n",
		       rc);
		return rc;
	}

	/*
	 * Allocate the LSTAR trampoline page and write the initial trap+sysretq
	 * bytes. This can defer because the buddy allocator may not
	 * be up at init_backend time. The helper logs deferral; late init
	 * retries once the buddy is available.
	 */
	rc = kvm_v2_trampoline_alloc_and_install(&vm);
	if (rc && rc != -ENOMEM) {
		pr_err("um: kvm-v2 vm_create: trampoline_alloc_and_install failed (%d)\n",
		       rc);
		return rc;
	}

	return 0;
}

static void kvm_v2_vm_log_create_result(void)
{
	pr_debug("um: kvm-v2 vm_create: vm_fd=%d caps=%#llx tss=%#lx ident=%#lx physmem=%s trampoline=%s memslots=%s\n",
		 vm.vm_fd, vm.caps, KVM_V2_TSS_ADDR, KVM_V2_IDENTITY_ADDR,
		 vm.physmem_memslot_id >= 0 ? "installed" : "deferred",
		 vm.trampoline_page ? "installed" : "deferred",
		 vm.physmem_memslot_id >= 0 ? "primed" : "empty");
}

static void kvm_v2_vm_reset_state(void)
{
	kvm_v2_vm_reset_runtime_state();
	vm.kvm_fd = -1;
	vm.vm_fd = -1;
	vm.caps = 0;
}

static void kvm_v2_vm_drain_memslots(void)
{
	struct kvm_v2_memslot *m, *tmp;

	/*
	 * Registered slots tear down through the same record path. No
	 * KVM_SET_USER_MEMORY_REGION(size=0) is needed here: closing the VM fd
	 * tears down all kernel-side slots.
	 */
	list_for_each_entry_safe(m, tmp, &vm.memslots, list)
		kvm_v2_memslot_del(&vm, m->slot_id);
	vm.physmem_memslot_id = -1;
}

static void kvm_v2_vm_free_runtime_pages(void)
{
	/*
	 * Free in reverse of install: descriptors, kernel-half chain, then the
	 * trampoline page. Descriptor teardown touches PTE[1..3], so the
	 * kernel-half page-table chain must still be valid while it runs.
	 */
	kvm_v2_exception_free(&vm);
	kvm_v2_kernel_half_free(&vm);
	kvm_v2_trampoline_free(&vm);
}

static void kvm_v2_vm_free_allocations(void)
{
	kvm_v2_vm_drain_memslots();
	kvm_v2_vm_free_runtime_pages();
	kfree(vm.cpuid);
	vm.cpuid = NULL;
}

static void kvm_v2_vm_close_fds(bool close_kvm_fd)
{
	if (vm.vm_fd >= 0)
		os_close_file(vm.vm_fd);
	if (close_kvm_fd && vm.kvm_fd >= 0)
		os_close_file(vm.kvm_fd);
}

static void kvm_v2_vm_abort_create(void)
{
	kvm_v2_vm_free_allocations();
	kvm_v2_vm_close_fds(false);

	/*
	 * Do not close vm.kvm_fd here. kvm_v2_init() still owns /dev/kvm
	 * on a create error and closes it through its err_close path.
	 */
	kvm_v2_vm_reset_state();
}

int kvm_v2_vm_create(int kvm_fd, u64 caps)
{
	int vm_fd, rc;

	if (vm.vm_fd >= 0) {
		pr_warn("um: kvm-v2 vm_create: already created (vm_fd=%d)\n",
			vm.vm_fd);
		return -EBUSY;
	}

	vm_fd = kvm_v2_create_vm_fd(kvm_fd);
	if (vm_fd < 0)
		return vm_fd;

	rc = kvm_v2_vm_install_required_ioctls(vm_fd);
	if (rc)
		goto err_close_vm;

	kvm_v2_vm_enable_optional_caps(vm_fd);
	kvm_v2_vm_init_state(kvm_fd, vm_fd, caps);

	rc = kvm_v2_vm_install_deferred_state();
	if (rc)
		goto err_abort_create;

	kvm_v2_vm_log_create_result();
	return 0;

err_abort_create:
	kvm_v2_vm_abort_create();
	return rc;

err_close_vm:
	os_close_file(vm_fd);
	return rc;
}

void kvm_v2_vm_destroy(void)
{
	if (vm.vm_fd < 0)
		return;

	kvm_v2_vm_free_allocations();

	pr_debug("um: kvm-v2 vm_destroy: closing vm_fd=%d kvm_fd=%d\n",
		 vm.vm_fd, vm.kvm_fd);

	/*
	 * Close VM fd before /dev/kvm fd: the VM fd is a child of the
	 * KVM fd in the kernel-side reference graph, so closing in the
	 * other order would leak the VM until the parent fd's release
	 * runs.
	 */
	kvm_v2_vm_close_fds(true);
	kvm_v2_vm_reset_state();
}

struct kvm_v2_vm *kvm_v2_vm_get(void)
{
	return vm.vm_fd >= 0 ? &vm : NULL;
}
