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

#include <as-layout.h>		/* physmem_size, uml_physmem */
#include <os.h>

#include <asm/trace/um_backend.h>

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

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
	.kvm_fd              = -1,
	.vm_fd               = -1,
	.physmem_memslot_id  = -1,
};

/*
 * D.4b-pre: install the giant physmem identity-offset memslot. Mirrors
 * v1's kvm_ensure_memslot() at kvm-v1-archive/lifecycle.c:613-648 (the
 * v1 implementation v2 inherited the design from but missed wiring up).
 *
 * Why this matters: kvm_v2_load_cr3 writes __pa(active_mm->pgd) to CR3.
 * Under UML __pa(kva) = kva - uml_physmem, so the GPA is an offset
 * inside physmem (in [0, physmem_size)), not a host VA. KVM's TDP walks
 * the pgd at this GPA — and it needs a memslot covering this range.
 * Phase B.2's per-region memslots use guest_phys_addr = host_va =
 * region->va (in user-half VA space, far outside [0, physmem_size)),
 * so they don't cover the pgd / PUD/PMD/PTE / trampoline pages — all of
 * which live in physmem. One slot covers everything in physmem: PT
 * chain pages allocated from buddy come from physmem; alloc_page →
 * page_address → __pa = offset in [0, physmem_size); the memslot
 * translates back to userspace_addr = uml_physmem + offset = the
 * original kva. KVM fault-in resolves through current->mm (the
 * spawner) which has uml_physmem mapped from the very early boot path.
 *
 * Guest pgd PTE values reference UML-physical addresses (offsets in
 * physmem). KVM's GVA→GPA walk follows the guest pgd to those
 * physmem-offset GPAs; the physmem memslot translates them to host
 * pages.
 *
 * The per-region memslots from Phase B.2 stay (don't conflict with
 * this slot — different GPA range). Whether they're redundant or
 * needed for protection-bit enforcement is a separate Phase B audit
 * deferred to Phase H. D.4b-pre is the minimum to make D.5 work.
 *
 * Idempotent: a successful prior install short-circuits via
 * physmem_memslot_id >= 0 — necessary because the install is reachable
 * from two sites (eager vm_create attempt + lazy retry from D.1's
 * trampoline_late_install initcall). v1 used a file-static `registered`
 * bool for the same purpose (lifecycle.c:615); v2 stores the slot id on
 * struct kvm_v2_vm so the idempotence flag and the destroyer's view of
 * the slot are the same field.
 *
 * Returns 0 on success / already-installed, -EAGAIN if uml_physmem /
 * physmem_size aren't yet populated (vm_create runs from init_backend
 * which fires before linux_main finishes setting those globals — see
 * arch/um/kernel/um_arch.c:372 vs 392/399; v1 mirrored the same defer
 * pattern via `if (!uml_physmem || !physmem_size) return -EAGAIN`),
 * or a negative ioctl errno on hard failure.
 */
int kvm_v2_physmem_memslot_install(struct kvm_v2_vm *vmctx)
{
	struct kvm_userspace_memory_region kr;
	int slot_id, rc;

	if (!vmctx || vmctx->vm_fd < 0)
		return -EINVAL;

	if (vmctx->physmem_memslot_id >= 0) {
		/* Already installed — idempotent short-circuit. */
		return 0;
	}

	if (!uml_physmem || !physmem_size) {
		/*
		 * vm_create runs from init_backend() before linux_main
		 * populates uml_physmem / physmem_size. Mirror v1's
		 * lifecycle.c:623-627 defer: log once and let the lazy
		 * retry path (subsys_initcall in syscall_trap.c) pick it
		 * up once the globals are stable.
		 */
		pr_info("um: kvm-v2 physmem_memslot: deferred (uml_physmem=%#lx physmem_size=%#llx not yet populated; lazy retry will land it)\n",
			uml_physmem, physmem_size);
		return -EAGAIN;
	}

	slot_id = kvm_v2_memslot_add(vmctx, 0 /* gpa */,
				     uml_physmem /* hva */,
				     physmem_size, 0 /* flags */);
	if (slot_id < 0) {
		pr_err("um: kvm-v2 physmem_memslot: memslot_add failed (%d)\n",
		       slot_id);
		return slot_id;
	}

	kr = (struct kvm_userspace_memory_region){
		.slot		 = (u32)slot_id,
		.flags		 = 0,
		.guest_phys_addr = 0,
		.memory_size	 = physmem_size,
		.userspace_addr	 = uml_physmem,
	};

	rc = os_ioctl_generic(vmctx->vm_fd, KVM_SET_USER_MEMORY_REGION,
			      (unsigned long)&kr);
	if (rc < 0) {
		pr_err("um: kvm-v2 physmem_memslot: KVM_SET_USER_MEMORY_REGION(slot=%d hva=%#lx size=%#llx) failed (%d)\n",
		       slot_id, uml_physmem, physmem_size, rc);
		kvm_v2_memslot_del(vmctx, (u32)slot_id);
		return rc;
	}

	vmctx->physmem_memslot_id = slot_id;
	pr_info("um: kvm-v2 physmem_memslot: slot=%d gpa=0 hva=%#lx size=%#llx\n",
		slot_id, uml_physmem, physmem_size);
	trace_um_backend_kvm_v2_physmem_memslot_install(slot_id, uml_physmem,
							physmem_size);
	return 0;
}

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

#ifdef CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH
	/*
	 * APERF/MPERF MSR passthrough (host-counter visibility).
	 *
	 * KVM_CAP_X86_DISABLE_EXITS / KVM_X86_DISABLE_EXITS_APERFMPERF
	 * (bit 4) tells KVM to skip the IA32_APERF (0xE7) / IA32_MPERF
	 * (0xE8) rdmsr intercept and let the guest read the host
	 * counters directly.  Without it KVM returns zero — the
	 * regression Anderson hit with QEMU + libvirt, which plumb
	 * HLT/MWAIT/PAUSE/CSTATE via -overcommit cpu-pm=on but not
	 * APERFMPERF.  See Documentation/virt/uml/aperf-mperf.rst.
	 *
	 * Strict ordering: kvm/x86.c::kvm_vm_ioctl_enable_cap rejects
	 * the cap with -EINVAL if kvm->created_vcpus is non-zero (the
	 * "no vCPUs may exist yet" check).  vm_create runs before
	 * kvm_v2_vcpu_create — this is the only valid window.
	 *
	 * Host requirement: boot_cpu_has(X86_FEATURE_APERFMPERF).
	 * kvm_get_allowed_disable_exits() masks the bit off when the
	 * host CPU lacks the feature; the ioctl then returns -EINVAL
	 * and we log + continue (non-fatal — the guest just sees zero
	 * counters, matching the pre-passthrough behavior).
	 */
	if (kvm_v2_aperfmperf_enabled()) {
		struct kvm_enable_cap cap = {
			.cap     = KVM_CAP_X86_DISABLE_EXITS,
			.args[0] = KVM_X86_DISABLE_EXITS_APERFMPERF,
		};
		int erc;

		erc = os_ioctl_generic(vm_fd, KVM_ENABLE_CAP,
				       (unsigned long)&cap);
		if (erc < 0)
			pr_warn("um: kvm-v2 vm_create: APERFMPERF passthrough rejected (%d) — host may lack X86_FEATURE_APERFMPERF; guest rdmsr(0xE7/0xE8) will continue to read zero\n",
				erc);
		else
			pr_info("um: kvm-v2 vm_create: APERF/MPERF MSR passthrough enabled (guest rdmsr(0xE7/0xE8) reads host counters)\n");
	}
#endif

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
	vm.trampoline_page = NULL;	/* D.1: kvm_v2_trampoline_alloc_and_install
					 * fills these in below (best-effort —
					 * buddy may not be up; lazy retry from
					 * D.4/D.5 covers that case). */
	vm.trampoline_gpa  = 0;
	vm.physmem_memslot_id = -1;	/* D.4b-pre: install attempted just below;
					 * lazy retry from trampoline_late_install
					 * picks up an init_backend-time defer once
					 * uml_physmem / physmem_size are set. */
	vm.trampoline_pud_kva = NULL;	/* D.4b: kernel-half PT chain — installed
					 * by the subsys_initcall lazy retry once
					 * the trampoline alloc above lands; no
					 * eager attempt at vm_create because the
					 * chain's leaf PTE references the
					 * trampoline GPA which itself defers
					 * pre-buddy. */
	vm.trampoline_pmd_kva = NULL;
	vm.trampoline_pte_kva = NULL;
	vm.trampoline_pud_gpa = 0;
	/*
	 * Phase E.1 (memo 26 §E.1): IDT + handler stubs + GDT pages —
	 * installed by the subsys_initcall lazy retry once D.4b's PT
	 * chain is up. No eager attempt at vm_create because (a) buddy
	 * isn't up yet and (b) the chain's PTE table doesn't exist yet
	 * (we plug into PTE[1..3] of trampoline_pte_kva). Sentinel NULL
	 * means "not installed yet"; kvm_v2_exception_install short-
	 * circuits on NULL idt_kva so the late retry is safe.
	 */
	vm.idt_kva       = NULL;
	vm.idt_gpa       = 0;
	vm.handlers_kva  = NULL;
	vm.handlers_gpa  = 0;
	vm.gdt_kva       = NULL;
	vm.gdt_gpa       = 0;

	/*
	 * Phase D.4b-pre (memo 26 §D.4): install the giant physmem
	 * identity-offset memslot BEFORE the D.1 trampoline alloc — once
	 * D.5 flips .vcpu_run, KVM's TDP needs a memslot covering the
	 * physmem range so `__pa(pgd)` and `__pa(trampoline_kva)` resolve.
	 * Mirrors v1's kvm_ensure_memslot at kvm-v1-archive/lifecycle.c:
	 * 613-648.
	 *
	 * Best-effort at vm_create time, exactly like the trampoline alloc
	 * below: linux_main() runs init_backend() (this path) BEFORE it
	 * sets uml_physmem / physmem_size (arch/um/kernel/um_arch.c:372
	 * vs 392/399). The helper returns -EAGAIN in that window; the
	 * subsys_initcall lazy retry in syscall_trap.c picks the install
	 * up once the globals are populated. -EAGAIN is non-fatal here
	 * for the same reason the trampoline's pre-buddy -ENOMEM is —
	 * deferral is the expected boot-time path. Other negative errnos
	 * (KVM_SET_USER_MEMORY_REGION rejection, allocator failure) are
	 * fatal and unwind via err_close_vm.
	 *
	 * Order vs TSS_ADDR/IDENTITY_MAP_ADDR (above): KVM Intel rejects
	 * those two ioctls once any memslot exists, so memslot install
	 * MUST follow them. Order vs trampoline alloc (below): when D.4b
	 * later wires the PML4[448] PT chain off this memslot, the
	 * trampoline page must be in the slot before any guest walk —
	 * keeping memslot first matches that future invariant and v1's
	 * ensure-memslot-then-bootstrap order at lifecycle.c:613-648.
	 */
	rc = kvm_v2_physmem_memslot_install(&vm);
	if (rc && rc != -EAGAIN) {
		pr_err("um: kvm-v2 vm_create: physmem_memslot_install failed (%d)\n",
		       rc);
		goto err_close_vm;
	}

	/*
	 * Phase D.1 (memo 26 §D.1): allocate the LSTAR trampoline page
	 * and write the 5 trap+sysretq bytes. Best-effort at this point —
	 * D.0a documented that the buddy allocator is not yet up at
	 * init_backend time, so alloc_page returns NULL and the install
	 * defers. The helper logs the deferral; D.4 (MSR_LSTAR
	 * programming) will retry once the buddy is up. Keeping the call
	 * here matches memo 26 §D.1's "Hook the alloc helper into
	 * kvm_v2_vm_create" intent and gives the trace event the earliest
	 * possible firing site.
	 */
	rc = kvm_v2_trampoline_alloc_and_install(&vm);
	if (rc && rc != -ENOMEM) {
		pr_err("um: kvm-v2 vm_create: trampoline_alloc_and_install failed (%d)\n",
		       rc);
		goto err_close_vm;
	}

	pr_info("um: kvm-v2 vm_create: vm_fd=%d caps=%#llx tss=%#lx ident=%#lx physmem_memslot=%s trampoline=%s (memslots %s; A.3 attaches vCPU + installs CPUID)\n",
		vm.vm_fd, vm.caps, KVM_V2_TSS_ADDR, KVM_V2_IDENTITY_ADDR,
		vm.physmem_memslot_id >= 0 ? "installed" : "deferred",
		vm.trampoline_page ? "installed" : "deferred",
		vm.physmem_memslot_id >= 0 ? "primed" : "empty");
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
	 * Drain the memslot list. B.1 ships the allocator/list/lookup;
	 * B.2's mm_region_added populates per-region entries; D.4b-pre's
	 * kvm_v2_physmem_memslot_install adds the giant physmem slot.
	 * All three categories sit on vm.memslots and tear down via the
	 * same kvm_v2_memslot_del call — no per-category special-casing
	 * needed.
	 *
	 * No KVM_SET_USER_MEMORY_REGION(size=0) here — that's B.3's
	 * responsibility once mm_region_removed wires it. At
	 * vm_destroy time the VM fd itself is about to close, which
	 * tears down all slots in the kernel anyway, so a per-slot
	 * delete ioctl would be redundant.
	 */
	list_for_each_entry_safe(m, tmp, &vm.memslots, list)
		kvm_v2_memslot_del(&vm, m->slot_id);
	vm.physmem_memslot_id = -1;	/* D.4b-pre: drained above; reset
					 * sentinel so a future re-init's
					 * idempotence check doesn't see a
					 * stale slot id. */

	/*
	 * Phase E.1: free the IDT + handler stubs + GDT pages. Must run
	 * BEFORE kvm_v2_kernel_half_free — the helper writes
	 * trampoline_pte_kva[1..3] = 0 (clearing PTE entries that the
	 * chain owns), so the chain pages must still be valid memory at
	 * this point. Safe on a never-installed VM (NULL idt_kva → no-op).
	 *
	 * Order vs install: install was memslot → trampoline → chain → E.1
	 * (writing PTE[1..3] of the chain). Free goes in reverse: E.1 →
	 * chain → trampoline → memslot. Mirrors the "free in reverse-of-
	 * install" pattern across this teardown.
	 */
	kvm_v2_exception_free(&vm);

	/*
	 * Phase D.4b: free the kernel-half PT chain (PUD/PMD/PTE pages)
	 * + clear swapper_pg_dir[448] / init_mm.pgd[448]. Must run BEFORE
	 * kvm_v2_trampoline_free below — the chain's leaf PTE references
	 * the trampoline GPA, so freeing the chain first keeps "freeing
	 * a referencing structure before the referent" symmetric with the
	 * install order (memslot → trampoline → chain). Safe on a
	 * never-installed VM (helper short-circuits on NULL pud_kva) —
	 * covers the boot path where the late_install initcall hadn't run
	 * yet at shutdown time.
	 */
	kvm_v2_kernel_half_free(&vm);

	/*
	 * Phase D.1: free the LSTAR trampoline page. Safe on a
	 * never-installed VM (helper short-circuits on NULL page) — covers
	 * the boot path where vm_create deferred the install and no later
	 * caller retried before shutdown.
	 */
	kvm_v2_trampoline_free(&vm);

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
