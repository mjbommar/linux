// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 KVM syscall and exception trap support.
 *
 * The LSTAR trampoline puts an out %al, $0xf4 instruction at the guest
 * SYSCALL target. KVM reports that instruction as KVM_EXIT_IO, letting the
 * host dispatcher extract the syscall number, propagate the post-SYSCALL
 * RIP/RFLAGS from RCX/R11 back into UML's per-task register state, and
 * call the common handle_syscall() path.
 *
 * Syscall return state stays in per-task regs. The next outer vcpu_run()
 * iteration copies those regs into whichever per-host-CPU vCPU is selected
 * for the task. Do not use the shared kvm_run mmap after handle_syscall()
 * returns: blocking syscalls can schedule, and another task may have reused
 * that per-CPU vCPU before the sleeping syscall resumes.
 *
 * The trampoline bytes and host-side decoder live together because they
 * are two halves of the same ABI: the byte at offset 0x40 issues the IO
 * trap, and the dispatcher at the bottom of this file consumes it.
 *
 * Fallback LSTAR trampoline:
 *
 *   out %al, $0xf4   ; e6 f4 -- KVM_EXIT_IO trap (port 0xf4 = SYSCALL)
 *   sysretq          ; 48 0f 07 -- drops to CPL=3, RIP=RCX, RFLAGS=R11
 *
 * Mechanism:
 *   - Guest at CPL=3 issues SYSCALL with NR in RAX.
 *   - SYSCALL switches to CPL=0 and loads RIP from MSR_LSTAR. FMASK clears
 *     IF so interrupts stay masked across the trampoline.
 *   - Trampoline runs at CPL=0: out %al, $0xf4 is a kernel-priv I/O
 *     instruction, executes without fault, traps to host with
 *     kvm_run->exit_reason = KVM_EXIT_IO and kvm_run->io.port = 0xf4.
 *   - KVM_EXIT_IO dispatch reads the port, marshals
 *     kvm_run->s.regs.regs into uml_pt_regs (sync-regs, no
 *     KVM_GET_REGS), calls into arch/um/kernel/skas/syscall.c's
 *     handle_syscall, then returns to the outer per-trap loop.
 *   - The next KVM_RUN entry rebuilds the vCPU state from per-task
 *     regs. For normal syscall returns that means RIP is the user
 *     continuation from RCX and RFLAGS is the user flags from R11.
 *
 * VM creation can run before page allocation is available, so trampoline
 * allocation may defer until the subsys_initcall late-install helper.
 */

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/kvm.h>		/* struct kvm_run, KVM_EXIT_IO */
#include <linux/mm.h>
#include <linux/mm_types.h>	/* init_mm */
#include <linux/pgtable.h>	/* pgd_index */
#include <linux/printk.h>
#include <linux/sched/signal.h>	/* force_sig */
#include <linux/set_memory.h>
#include <linux/signal.h>	/* clear_siginfo, kernel_siginfo_t */
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thread_info.h>	/* read_thread_flags, _TIF_WORK_MASK */
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <uapi/linux/time_types.h>
#include <uapi/asm-generic/siginfo.h>	/* ILL_ILLOPN, FPE_INTOVF, SEGV_MAPERR */

#include <asm/unistd.h>
#include <asm/page.h>
#include <asm/processor-flags.h>	/* X86_CR0_TS */
#include <asm/trace/um_backend.h>

#include <kern_util.h>		/* segv_handler, relay_signal */
#include <os.h>			/* os_drop_caching */
#include <skas.h>		/* handle_syscall */
#include <sysdep/ptrace.h>	/* uml_pt_regs, HOST_AX/CX/IP/EFLAGS/R11, UPT_SYSCALL_NR */
#include <sysdep/ptrace_user.h>	/* PT_SYSCALL_NR */

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

/*
 * KVM-private x86 page-table bits used for the trampoline kernel-half
 * mapping. UML's software pgtable bits are not suitable for these entries.
 */
#define KVM_V2_X86_PTE_PRESENT		BIT_ULL(0)
#define KVM_V2_X86_PTE_RW		BIT_ULL(1)
#define KVM_V2_X86_PTE_ACCESSED	BIT_ULL(5)
#define KVM_V2_X86_KERN_NONLEAF	(KVM_V2_X86_PTE_PRESENT | \
					 KVM_V2_X86_PTE_RW | \
					 KVM_V2_X86_PTE_ACCESSED)
#define KVM_V2_X86_KERN_LEAF_RO	(KVM_V2_X86_PTE_PRESENT | \
					 KVM_V2_X86_PTE_ACCESSED)

#define KVM_V2_KERNEL_PML4_SLOT		448

/*
 * Fast syscall gadget body, assembled from lstar_gadget.S. The fallback
 * and gadget bytes are linker-defined .rodata symbols copied into the
 * trampoline page at runtime.
 *
 * The gadget keeps common read-only syscalls in the guest and falls back
 * to the KVM_EXIT_IO path for anything else. Its detailed dispatch layout
 * belongs in the assembly source; this file only enforces page-fit and
 * install-order invariants.
 */

static size_t kvm_v2_lstar_body_budget(void)
{
	return PAGE_SIZE - KVM_V2_TRAMPOLINE_LSTAR_OFFSET;
}

static void kvm_v2_check_lstar_body_size(const char *name, size_t len)
{
	const size_t budget = kvm_v2_lstar_body_budget();

	if (len > budget)
		panic("kvm-v2: %s LSTAR body overflow len=%zu budget=%zu off=%#x",
		      name, len, budget, KVM_V2_TRAMPOLINE_LSTAR_OFFSET);
}

static void kvm_v2_write_lstar_body(void *kva, const u8 *body, size_t len,
				    const char *name)
{
	u8 *dst = (u8 *)kva + KVM_V2_TRAMPOLINE_LSTAR_OFFSET;

	kvm_v2_check_lstar_body_size(name, len);
	memcpy(dst, body, len);

	/*
	 * Read back the installed bytes and panic on mismatch. A corrupt LSTAR
	 * target sends the guest to arbitrary CPL=0 code, so trampoline
	 * corruption must fail closed.
	 */
	if (memcmp(dst, body, len) != 0)
		panic("kvm-v2: %s LSTAR mismatch kva=%p off=%#x len=%zu got=%02x %02x %02x %02x %02x",
		      name, kva, KVM_V2_TRAMPOLINE_LSTAR_OFFSET, len,
		      dst[0], dst[1], dst[2], dst[3], dst[4]);
}

static struct page *kvm_v2_trampoline_alloc_page(void)
{
	struct page *page;

	/*
	 * GFP_KERNEL is valid here because both the VM-create and late-install
	 * callers run in process context. Keep the page zero-filled so
	 * unpopulated trampoline regions are deterministic.
	 */
	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		/*
		 * Early VM creation can run before page allocation is available.
		 * Return -ENOMEM quietly; the late-install path retries after
		 * mm_init.
		 */
		pr_debug("um: kvm-v2 trampoline_install: page allocation failed; deferring to late install\n");
	}
	return page;
}

static void kvm_v2_trampoline_install_fallback(void *kva)
{
	const size_t fb_len = kvm_v2_lstar_fallback_end -
			      kvm_v2_lstar_fallback_start;
	const size_t gd_len = kvm_v2_lstar_gadget_end -
			      kvm_v2_lstar_gadget_start;
	/*
	 * Install the LSTAR body in two stages. First write the 5-byte
	 * fallback body. kvm_v2_exception_install() maps all per-vCPU state
	 * pages and upgrades the body to the stay-in-guest gadget.
	 *
	 * The gadget reads its per-vCPU state via %gs. Installing gadget bytes
	 * before those pages are mapped would make the first user syscall fault
	 * inside the trampoline. If exception installation fails partway
	 * through, the fallback remains in place.
	 */
	kvm_v2_check_lstar_body_size("gadget", gd_len);
	kvm_v2_write_lstar_body(kva, kvm_v2_lstar_fallback_start,
				fb_len, "fallback");
}

static void kvm_v2_trampoline_protect_ro(void *kva)
{
	int rc;

	/*
	 * Drop host-side write privilege when the architecture supports
	 * changing page permissions. The guest mapping is supervisor-only,
	 * so CPL=3 code cannot write the trampoline page either way.
	 */
	rc = set_memory_ro((unsigned long)kva, 1);
	if (rc)
		pr_warn("um: kvm-v2 trampoline_install: set_memory_ro(%p) returned %d\n",
			kva, rc);
}

static void kvm_v2_trampoline_publish(struct kvm_v2_vm *vm, void *kva,
				      phys_addr_t gpa)
{
	vm->trampoline_page = kva;
	vm->trampoline_gpa  = gpa;

	pr_debug("um: kvm-v2 trampoline_install: kva=%p gpa=%pa gva=%#llx lstar_off=%#x\n",
		 kva, &gpa, (u64)KVM_V2_LSTAR_GVA,
		 KVM_V2_TRAMPOLINE_LSTAR_OFFSET);
	trace_um_backend_kvm_v2_trampoline_install((u64)gpa,
						   (u64)KVM_V2_LSTAR_GVA);
}

int kvm_v2_trampoline_alloc_and_install(struct kvm_v2_vm *vm)
{
	struct page *page;
	void *kva;
	phys_addr_t gpa;

	if (!vm)
		return -EINVAL;

	/*
	 * Idempotent: a successful prior install short-circuits. The
	 * vm_create attempt may have run before page allocation was available;
	 * the late-install path calls back after mm_init, and this guard keeps a
	 * multiply-installed trampoline impossible.
	 */
	if (vm->trampoline_page)
		return 0;

	page = kvm_v2_trampoline_alloc_page();
	if (!page)
		return -ENOMEM;

	kva = page_address(page);
	gpa = __pa(kva);

	kvm_v2_trampoline_install_fallback(kva);
	kvm_v2_trampoline_protect_ro(kva);
	kvm_v2_trampoline_publish(vm, kva, gpa);
	return 0;
}

static void kvm_v2_kernel_half_free_pages(void *pud_kva, void *pmd_kva,
					  void *pte_kva)
{
	if (pte_kva)
		free_page((unsigned long)pte_kva);
	if (pmd_kva)
		free_page((unsigned long)pmd_kva);
	if (pud_kva)
		free_page((unsigned long)pud_kva);
}

static int kvm_v2_kernel_half_alloc_pages(void **pud_kva, void **pmd_kva,
					  void **pte_kva)
{
	/*
	 * GFP_KERNEL because subsys_initcall runs in process context. __GFP_ZERO
	 * is essential: only entry [0] is populated in each table; the other
	 * entries must stay not-present so unrelated GVAs do not walk into
	 * garbage.
	 */
	*pud_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	*pmd_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	*pte_kva = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!*pud_kva || !*pmd_kva || !*pte_kva) {
		pr_debug("um: kvm-v2 kernel_half_install: page allocation failed; deferring\n");
		kvm_v2_kernel_half_free_pages(*pud_kva, *pmd_kva, *pte_kva);
		*pud_kva = NULL;
		*pmd_kva = NULL;
		*pte_kva = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void kvm_v2_kernel_half_populate_chain(struct kvm_v2_vm *vm,
					      void *pud_kva, void *pmd_kva,
					      void *pte_kva,
					      phys_addr_t pmd_pa,
					      phys_addr_t pte_pa)
{
	/*
	 * This is a KVM-private page-table chain, not a UML-managed pgtable.
	 * Write the minimal hardware flags directly: non-leaf entries are
	 * present, writable, and accessed; the trampoline leaf is present and
	 * accessed but read-only.
	 */
	((u64 *)pud_kva)[0] = (u64)(pmd_pa | KVM_V2_X86_KERN_NONLEAF);
	((u64 *)pmd_kva)[0] = (u64)(pte_pa | KVM_V2_X86_KERN_NONLEAF);
	((u64 *)pte_kva)[0] =
		(u64)(vm->trampoline_gpa | KVM_V2_X86_KERN_LEAF_RO);
}

static unsigned long kvm_v2_kernel_half_publish_pml4(phys_addr_t pud_pa)
{
	unsigned long entry;

	/*
	 * Seed swapper_pg_dir with the same hardware-format entry used above.
	 * UML's pgd_alloc copies the kernel half from swapper into subsequent
	 * mm page directories, so the mapping propagates without a per-mm hook.
	 * Use direct assignment so the stored value is exactly the one copied
	 * by pgd_alloc.
	 */
	entry = (unsigned long)(pud_pa | KVM_V2_X86_KERN_NONLEAF);
	swapper_pg_dir[KVM_V2_KERNEL_PML4_SLOT] = __pgd(entry);

	/*
	 * Patch init_mm.pgd explicitly. swapper_pg_dir[] is the source
	 * pgd_alloc copies from for subsequent mms; init_mm already exists at
	 * this point and its pgd was populated before swapper was updated, so
	 * it cannot pick the entry up via the copy path. UML does not export
	 * mm_list, so init_mm is handled directly.
	 */
	if (init_mm.pgd)
		init_mm.pgd[KVM_V2_KERNEL_PML4_SLOT] = __pgd(entry);
	else
		pr_warn("um: kvm-v2 kernel_half_install: init_mm.pgd is NULL; only swapper_pg_dir[%d] seeded\n",
			KVM_V2_KERNEL_PML4_SLOT);

	return entry;
}

static void kvm_v2_kernel_half_record_chain(struct kvm_v2_vm *vm,
					    void *pud_kva, void *pmd_kva,
					    void *pte_kva, phys_addr_t pud_pa)
{
	/*
	 * Stash the chain on the VM struct for free-time. PUD GPA goes on
	 * the struct so introspection callers can confirm the entry value
	 * without re-walking swapper.
	 */
	vm->trampoline_pud_kva = pud_kva;
	vm->trampoline_pmd_kva = pmd_kva;
	vm->trampoline_pte_kva = pte_kva;
	vm->trampoline_pud_gpa = pud_pa;
}

struct kvm_v2_kernel_half_chain {
	void *pud_kva;
	void *pmd_kva;
	void *pte_kva;
	phys_addr_t pud_pa;
	phys_addr_t pmd_pa;
	phys_addr_t pte_pa;
	unsigned long pml4_entry;
};

static int kvm_v2_kernel_half_validate_install(struct kvm_v2_vm *vm)
{
	if (!vm)
		return -EINVAL;

	/*
	 * Idempotent short-circuit: a successful prior install means all
	 * three KVAs are non-NULL and the kernel PML4 slot already carries
	 * the install. Re-running the helper would leak the old PT pages and
	 * silently overwrite the PML4 entries with a chain that has the same
	 * layout but different physical pages. Any task pgd that already
	 * copied the kernel half would retain the old chain.
	 */
	if (vm->trampoline_pud_kva)
		return 1;

	/*
	 * Requires an installed trampoline because the leaf PTE needs its GPA.
	 * The physmem memslot install is not checked here; this helper only
	 * builds the chain, and KVM validates memslot coverage when the vCPU
	 * enters.
	 */
	if (!vm->trampoline_page || !vm->trampoline_gpa) {
		pr_err("um: kvm-v2 kernel_half_install: trampoline missing page=%p gpa=%pa\n",
		       vm->trampoline_page, &vm->trampoline_gpa);
		return -EINVAL;
	}

	/*
	 * KVM_V2_TRAMPOLINE_GVA and KVM_V2_LSTAR_GVA must resolve through
	 * the same PML4 entry. Check before allocating so a bad constant
	 * cannot partly install a chain in the wrong slot.
	 */
	BUILD_BUG_ON(pgd_index((unsigned long)KVM_V2_LSTAR_GVA) !=
		     KVM_V2_KERNEL_PML4_SLOT);

	return 0;
}

static int kvm_v2_kernel_half_prepare_chain(struct kvm_v2_kernel_half_chain *c)
{
	int rc;

	rc = kvm_v2_kernel_half_alloc_pages(&c->pud_kva, &c->pmd_kva,
					    &c->pte_kva);
	if (rc)
		return rc;

	c->pud_pa = __pa(c->pud_kva);
	c->pmd_pa = __pa(c->pmd_kva);
	c->pte_pa = __pa(c->pte_kva);
	return 0;
}

static void kvm_v2_kernel_half_publish_chain(struct kvm_v2_vm *vm,
					     struct kvm_v2_kernel_half_chain *c)
{
	kvm_v2_kernel_half_populate_chain(vm, c->pud_kva, c->pmd_kva,
					  c->pte_kva, c->pmd_pa, c->pte_pa);
	c->pml4_entry = kvm_v2_kernel_half_publish_pml4(c->pud_pa);
	kvm_v2_kernel_half_record_chain(vm, c->pud_kva, c->pmd_kva,
					c->pte_kva, c->pud_pa);
}

static void kvm_v2_kernel_half_trace_install(struct kvm_v2_vm *vm,
					     struct kvm_v2_kernel_half_chain *c)
{
	pr_debug("um: kvm-v2 kernel_half_install: pud=%pa pmd=%pa pte=%pa tramp=%pa gva=%#llx entry=%#lx\n",
		 &c->pud_pa, &c->pmd_pa, &c->pte_pa, &vm->trampoline_gpa,
		 (u64)KVM_V2_LSTAR_GVA, c->pml4_entry);
	trace_um_backend_kvm_v2_pml4_install((u64)c->pud_pa,
					     (u64)KVM_V2_LSTAR_GVA);
}

/*
 * Install the per-VM kernel-half page-table chain at PML4[448] so
 * KVM_V2_LSTAR_GVA is reachable from any guest CR3 used by the KVM path.
 *
 * The chain maps only the trampoline page:
 *   PML4[448] -> PUD[0] -> PMD[0] -> PTE[0]
 *
 * Non-leaf entries use the hardware flags expected by UML's pgtable
 * validators: present, writable, and accessed. The leaf is present and
 * accessed but read-only and supervisor-only.
 *
 * swapper_pg_dir[KVM_V2_KERNEL_PML4_SLOT] seeds subsequent mms through
 * pgd_alloc(); init_mm.pgd[KVM_V2_KERNEL_PML4_SLOT] is patched directly
 * because init_mm already exists before this install runs.
 * BUILD_BUG_ON() enforces the expected PML4 slot for KVM_V2_LSTAR_GVA.
 *
 * The helper is idempotent. One chain is shared by the whole VM and freed
 * from kvm_v2_kernel_half_free().
 */
int kvm_v2_kernel_half_install(struct kvm_v2_vm *vm)
{
	struct kvm_v2_kernel_half_chain chain = {};
	int rc;

	rc = kvm_v2_kernel_half_validate_install(vm);
	if (rc > 0)
		return 0;
	if (rc)
		return rc;

	rc = kvm_v2_kernel_half_prepare_chain(&chain);
	if (rc)
		return rc;

	kvm_v2_kernel_half_publish_chain(vm, &chain);
	kvm_v2_kernel_half_trace_install(vm, &chain);
	return 0;
}

void kvm_v2_kernel_half_free(struct kvm_v2_vm *vm)
{
	if (!vm || !vm->trampoline_pud_kva)
		return;

	/*
	 * Clear the kernel PML4 slot before freeing the pages so any
	 * concurrent mm operation sees zero (= not present) rather than
	 * dangling. At vm_destroy time on shutdown, no mms should be operating
	 * against these entries.
	 *
	 * Already-allocated mms retain their copied pgd[448] entry. That is
	 * acceptable at VM teardown because no further KVM_RUN will use those
	 * roots. A VM reinitialisation path would need to revoke the entry
	 * from all live mms before freeing the chain.
	 */
	swapper_pg_dir[KVM_V2_KERNEL_PML4_SLOT] = __pgd(0);
	if (init_mm.pgd)
		init_mm.pgd[KVM_V2_KERNEL_PML4_SLOT] = __pgd(0);

	kvm_v2_kernel_half_free_pages(vm->trampoline_pud_kva,
				      vm->trampoline_pmd_kva,
				      vm->trampoline_pte_kva);
	vm->trampoline_pte_kva = NULL;
	vm->trampoline_pmd_kva = NULL;
	vm->trampoline_pud_kva = NULL;
	vm->trampoline_pud_gpa = 0;
}

static bool kvm_v2_late_install_trampoline_page(struct kvm_v2_vm *vm)
{
	/*
	 * Finish any deferred physmem memslot install from this initcall.
	 * vm_create may have returned -EAGAIN because uml_physmem /
	 * physmem_size were not set yet. subsys_initcall fires after
	 * linux_main() completes, so the globals are stable here.
	 * The helper is idempotent through the physmem_memslot_id sentinel, so a
	 * successful eager install short-circuits. Failure is reported by the
	 * helper; the trampoline retry below still runs.
	 */
	(void)kvm_v2_physmem_memslot_install(vm);

	if (vm->trampoline_page)
		return true;

	if (kvm_v2_trampoline_alloc_and_install(vm)) {
		/*
		 * Without the trampoline page, the kernel-half install has no
		 * leaf GPA to point at. A subsequent caller may retry once
		 * allocation is available. Do not propagate the error from the
		 * initcall; delegated fallback operations can still keep boot
		 * moving while KVM run support is unavailable.
		 */
		return false;
	}
	return true;
}

static int kvm_v2_late_install_kernel_half(struct kvm_v2_vm *vm)
{
	int rc;

	/*
	 * Install the kernel-half PT chain at PML4[448] after the trampoline
	 * allocation, because the leaf PTE references vm->trampoline_gpa.
	 * Idempotent: the helper short-circuits if it already ran.
	 *
	 * Without this chain the trampoline VA is unreachable from any guest
	 * CR3 walk and KVM_RUN cannot dispatch syscalls safely.
	 */
	rc = kvm_v2_kernel_half_install(vm);
	if (rc)
		pr_err("um: kvm-v2 kernel_half_install: failed (%d); KVM dispatch cannot run without the kernel-half PT chain\n",
		       rc);
	return rc;
}

static int kvm_v2_late_install_exceptions(struct kvm_v2_vm *vm)
{
	int rc;

	/*
	 * Install IDT, handler stubs, and GDT in
	 * PML4[448]/PUD[0]/PMD[0]/PTE[1..3], then re-issue KVM_SET_SREGS for
	 * every pool member to point idt.base/gdt.base at the new GVAs.
	 * Idempotent re-runs short-circuit via vm->idt_kva.
	 *
	 * Without these pages, any guest exception can cascade to a triple
	 * fault.
	 */
	rc = kvm_v2_exception_install(vm);
	if (rc)
		pr_err("um: kvm-v2 exception_install: failed (%d); KVM dispatch cannot run without IDT/GDT pages\n",
		       rc);
	return rc;
}

/*
 * Late-install path. vm_create can run before page allocation and physmem
 * globals are ready; this initcall runs after mm_init and completes the
 * per-VM install.
 *
 * The trampoline is per-VM rather than per-vCPU, so an initcall is the
 * natural retry point. CPUID remains per-vCPU and lazy-installs from the
 * dispatcher.
 *
 * Skip on non-v2 boots -- the VM context is NULL when seccomp is
 * selected, and an initcall that ran unconditionally would be
 * misleading in dmesg.
 */
static int __init kvm_v2_trampoline_late_install(void)
{
	struct kvm_v2_vm *vm = kvm_v2_vm_get();
	int rc;

	if (!vm)
		return 0;	/* KVM v2 backend is not active. */

	if (!kvm_v2_late_install_trampoline_page(vm))
		return 0;

	rc = kvm_v2_late_install_kernel_half(vm);
	if (rc)
		return rc;

	return kvm_v2_late_install_exceptions(vm);
}
subsys_initcall(kvm_v2_trampoline_late_install);

/*
 * Upgrade the LSTAR body in place from the 5-byte fallback
 * (out + sysretq) to the stay-in-guest gadget. The caller guarantees
 * every per-vCPU gadget state page is mapped before this runs, so the
 * gadget's mov %gs:OFF, %eax always finds backing.
 *
 * Idempotent -- a second invocation re-writes the same bytes and the
 * readback still passes. No locking required because
 * kvm_v2_exception_install() runs once per VM at subsys_initcall, before
 * any user task exists.
 *
 * If the upgrade is skipped, the LSTAR stays at the 5-byte fallback and
 * SYSCALLs continue through the slow KVM_EXIT_IO route.
 */
int kvm_v2_trampoline_upgrade_to_gadget(struct kvm_v2_vm *vm)
{
	void *kva;
	const u8 *body = kvm_v2_lstar_gadget_start;
	const size_t body_len = kvm_v2_lstar_gadget_end -
				kvm_v2_lstar_gadget_start;

	if (!vm || !vm->trampoline_page) {
		pr_warn("um: kvm-v2 trampoline_upgrade: vm or trampoline_page NULL -- leaving 5-byte fallback in place\n");
		return -EINVAL;
	}

	kva = vm->trampoline_page;

	kvm_v2_write_lstar_body(kva, body, body_len, "gadget");

	pr_debug("um: kvm-v2 trampoline_upgrade: gadget installed len=%zu off=%#x\n",
		 body_len, KVM_V2_TRAMPOLINE_LSTAR_OFFSET);
	return 0;
}

void kvm_v2_trampoline_free(struct kvm_v2_vm *vm)
{
	int rc;

	if (!vm || !vm->trampoline_page)
		return;

	/*
	 * Restore writable protection before freeing so the page returns to
	 * the allocator in the expected state.
	 */
	rc = set_memory_rw((unsigned long)vm->trampoline_page, 1);
	if (rc)
		pr_warn("um: kvm-v2 trampoline_free: set_memory_rw(%p) returned %d\n",
			vm->trampoline_page, rc);

	free_page((unsigned long)vm->trampoline_page);
	vm->trampoline_page = NULL;
	vm->trampoline_gpa  = 0;
}

/*
 * IST-frame parser and per-class dispatchers.
 *
 * Each in-guest IDT handler stub issues
 * out %al, $port after the CPU pushed the standard long-mode iretq
 * frame onto the per-vCPU IST stack. Layout per Intel SDM Vol.3
 * section 6.13 ("Error Code") and section 6.14 ("Exception and Interrupt Handling
 * in 64-bit Mode"):
 *
 *   ist_stack_top - 8:    user SS
 *   ist_stack_top - 16:   user RSP
 *   ist_stack_top - 24:   user RFLAGS
 *   ist_stack_top - 32:   user CS
 *   ist_stack_top - 40:   user RIP
 *   ist_stack_top - 48:   error_code  (only for vectors that push one)
 *
 * Vectors WITH error code (SDM Vol.3 section 6.13 "Error Code"):
 *   #DF (8), #TS (10), #NP (11), #SS (12), #GP (13), #PF (14),
 *   #AC (17), #SX (30).
 * Vectors WITHOUT error code:
 *   #DE (0), #DB (1), #BP (3), #OF (4), #BR (5), #UD (6), #NM (7),
 *   #MF (16), #XM (19).
 *
 * #PF and #GP push error code; #UD, #DE, and #OF do not. The frame
 * lives in host kernel virtual memory (vcpu->ist_stack_kva is the kva
 * of the allocated IST page; it is regular kernel memory, so no
 * KVM ioctl is needed to read it). After the host dispatcher consumes
 * the frame and runs UML's existing trap.c handler, mutated regs are
 * marshalled back into the IST frame so the in-guest
 * iretq pops the post-handler RIP/RSP/RFLAGS -- the marshal-back is
 * how a signal-delivered RIP (do_signal stashed a handler VA into
 * regs->gp[HOST_IP]) lands as the next user-mode instruction.
 *
 * Handler stubs must not execute mov %cr2, %rax: doing so would
 * clobber user RAX before the host captures vCPU state on KVM_EXIT_IO.
 * The host reads CR2 from sync-regs sregs.cr2 instead.
 */

/*
 * Read the IRETQ frame from the IST stack page. top is the host
 * kernel VA of the byte one past the highest-numbered byte the CPU
 * could push (i.e. ist_stack_kva + PAGE_SIZE -- stacks grow down so
 * the first push lands at top - 8). has_error_code selects the
 * 48-byte (with EC) vs 40-byte (without EC) frame layout.
 *
 * The read range needs no bounds check: the page is 4096 bytes and the
 * helper reads at most 48 bytes from the top, well within the allocation.
 */
struct kvm_v2_ist_frame {
	u64  user_rip;
	u64  user_cs;
	u64  user_rflags;
	u64  user_rsp;
	u64  user_ss;
	u64  error_code;	/* valid only when has_error_code */
	bool has_error_code;
};

static void kvm_v2_ist_frame_read(struct kvm_v2_vcpu *vcpu,
				  struct kvm_v2_ist_frame *f,
				  bool has_error_code)
{
	u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;

	/*
	 * Per Intel SDM Vol.3 section 6.14.5, the long-mode IDT-pushed frame
	 * (with error code) lays out from bottom-of-frame upward:
	 *   +0:  error_code (only present when the vector pushes one)
	 *   +8:  RIP
	 *   +16: CS  (sign-extended u16 + 6 padding bytes)
	 *   +24: RFLAGS
	 *   +32: RSP
	 *   +40: SS  (sign-extended u16 + 6 padding bytes)
	 * Without error code, RIP is at the bottom and the rest shifts
	 * down by 8.
	 *
	 * Use a fixed 40-byte RIP-anchored frame and read the error code
	 * eight bytes below it when present. With an error code, RIP is not
	 * at the bottom of the full frame; treating the full 48 bytes as a
	 * single RIP-anchored object shifts every field by one slot.
	 */
	f->has_error_code = has_error_code;
	if (has_error_code)
		f->error_code = *(u64 *)(top - 48);
	else
		f->error_code = 0;

	f->user_rip    = *(u64 *)(top - 40 +  0);
	f->user_cs     = *(u64 *)(top - 40 +  8);
	f->user_rflags = *(u64 *)(top - 40 + 16);
	f->user_rsp    = *(u64 *)(top - 40 + 24);
	f->user_ss     = *(u64 *)(top - 40 + 32);
}

/*
 * Marshal the post-handler user state back into the IST frame consumed by
 * the stub's iretq. Only RIP, RFLAGS, and RSP are rewritten; CS and SS keep
 * the same user CPL, and the error code is popped and discarded by the stub.
 */
static void kvm_v2_ist_frame_write(struct kvm_v2_vcpu *vcpu,
				   const struct uml_pt_regs *regs,
				   bool has_error_code)
{
	u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;
	u64 cs, ss;
	struct arch_thread *a = &current->thread.arch;

	/*
	 * The IST iret frame returns to CPL=3, so its RIP slot must be a
	 * user address. If the caller supplies a handler-page RIP, leave the
	 * current frame intact and warn.
	 */
	if (regs->gp[HOST_IP] >= KVM_V2_HANDLERS_GVA &&
	    regs->gp[HOST_IP] < KVM_V2_HANDLERS_GVA + 0x1000) {
		pr_warn_ratelimited("um: kvm-v2: refusing handler RIP in IST return frame pid=%d comm=%s ip=%lx sp=%lx\n",
				    current->pid, current->comm,
				    regs->gp[HOST_IP],
				    regs->gp[HOST_SP]);
		return;
	}

	/*
	 * Layout matches kvm_v2_ist_frame_read: RIP-anchored frame
	 * starts at top - 40 regardless of whether the vector pushed
	 * an error code. The error code (when present) lives at top - 48
	 * and is popped and discarded by the in-guest iretq; leave that
	 * slot unchanged.
	 */
	*(u64 *)(top - 40 +  0) = regs->gp[HOST_IP];      /* RIP */
	cs = *(u64 *)(top - 40 +  8);                     /* CS unchanged */
	*(u64 *)(top - 40 + 16) = regs->gp[HOST_EFLAGS];  /* RFLAGS */
	*(u64 *)(top - 40 + 24) = regs->gp[HOST_SP];      /* RSP */
	ss = *(u64 *)(top - 40 + 32);                     /* SS unchanged */

	/*
	 * Snapshot the IST frame into per-task storage so this task can restore
	 * its own frame before the next KVM_RUN, even if another UML task reused
	 * the same vCPU and clobbered the shared IST stack in between.
	 *
	 * Without this, multi-thread workloads where two tasks sharing one
	 * mm both take #PFs alternately can end up with one task's iretq
	 * popping the other task's CS/RIP.
	 */
	a->kvm_v2.ist_frame[0] = (has_error_code ? *(u64 *)(top - 48) : 0);
	a->kvm_v2.ist_frame[1] = regs->gp[HOST_IP];
	a->kvm_v2.ist_frame[2] = cs;
	a->kvm_v2.ist_frame[3] = regs->gp[HOST_EFLAGS];
	a->kvm_v2.ist_frame[4] = regs->gp[HOST_SP];
	a->kvm_v2.ist_frame[5] = ss;
	a->kvm_v2.ist_pending = true;
}

/*
 * Restore per-task IST frame snapshot into the current vCPU's IST
 * stack. Called from kvm_v2_vcpu_run before KVM_RUN re-entry for
 * tasks that have a pending IST frame queued (i.e., last exit was
 * an exception class delivered via IDT IST, not a SYSCALL trap).
 *
 * Defends against the cross-task IST clobber described above.
 */
void kvm_v2_ist_frame_restore_pending(struct kvm_v2_vcpu *vcpu)
{
	struct arch_thread *a = &current->thread.arch;
	u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;

	if (!a->kvm_v2.ist_pending)
		return;

	/*
	 * Restore only user return frames. A pending frame with a handler-page
	 * RIP or non-user CS is stale or inherited state; drop it rather than
	 * replaying a kernel-half return as userspace.
	 */
	if ((a->kvm_v2.ist_frame[1] >= KVM_V2_HANDLERS_GVA &&
	     a->kvm_v2.ist_frame[1] <  KVM_V2_HANDLERS_GVA + 0x1000) ||
	    (a->kvm_v2.ist_frame[2] & 3) != 3) {
		a->kvm_v2.ist_pending = false;
		return;
	}

	*(u64 *)(top - 48)      = a->kvm_v2.ist_frame[0]; /* error_code */
	*(u64 *)(top - 40 +  0) = a->kvm_v2.ist_frame[1]; /* RIP */
	*(u64 *)(top - 40 +  8) = a->kvm_v2.ist_frame[2]; /* CS */
	*(u64 *)(top - 40 + 16) = a->kvm_v2.ist_frame[3]; /* RFLAGS */
	*(u64 *)(top - 40 + 24) = a->kvm_v2.ist_frame[4]; /* RSP */
	*(u64 *)(top - 40 + 32) = a->kvm_v2.ist_frame[5]; /* SS */

	a->kvm_v2.ist_pending = false;
}

/*
 * Snapshot the IDT-pushed exception frame verbatim from the IST stack
 * into per-task arch_thread storage. This is the raw counterpart to
 * kvm_v2_ist_frame_write(): kvm_v2_ist_frame_write() stores the
 * UML-mutated return frame, while this helper preserves the hardware
 * frame for asynchronous exits that arrive before the handler stub runs.
 *
 * This helper stores the error-code layout. #PF and #GP use that layout;
 * no-error-code vectors leave ist_frame[0] unused on restore.
 */
void kvm_v2_ist_frame_snapshot_raw(struct kvm_v2_vcpu *vcpu)
{
	struct arch_thread *a = &current->thread.arch;
	u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;
	u64 rip = *(u64 *)(top - 40 +  0);
	u64 cs  = *(u64 *)(top - 40 +  8);

	/*
	 * The IDT-pushed iretq frame must describe user state. Reject
	 * handler-page RIPs and non-user CS values so stale stub-internal
	 * frames cannot be replayed as userspace returns.
	 */
	if ((rip >= KVM_V2_HANDLERS_GVA &&
	     rip <  KVM_V2_HANDLERS_GVA + 0x1000) ||
	    (cs & 3) != 3) {
		a->kvm_v2.ist_pending = false;
		return;
	}

	a->kvm_v2.ist_frame[0] = *(u64 *)(top - 48);          /* error_code */
	a->kvm_v2.ist_frame[1] = rip;                         /* RIP */
	a->kvm_v2.ist_frame[2] = cs;                          /* CS */
	a->kvm_v2.ist_frame[3] = *(u64 *)(top - 40 + 16);     /* RFLAGS */
	a->kvm_v2.ist_frame[4] = *(u64 *)(top - 40 + 24);     /* RSP */
	a->kvm_v2.ist_frame[5] = *(u64 *)(top - 40 + 32);     /* SS */
	a->kvm_v2.ist_pending = true;
}

/*
 * Inline #NM handling for asynchronous exits in the #NM stub. Replaying the
 * stub can make iretq consume a stale IST frame after another task has used
 * the same per-host-CPU vCPU, so restore the user frame and clear CR0.TS
 * directly.
 */
int kvm_v2_handle_nm_eintr_inline(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	/* #NM has no error code. */
	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	/*
	 * Emulate clts: clear CR0.TS so the next KVM_RUN re-enters with
	 * TS=0. The user's faulting FP instruction will succeed on retry at
	 * frame.user_rip.
	 */
	run->s.regs.sregs.cr0 &= ~X86_CR0_TS;
	run->kvm_dirty_regs |= KVM_SYNC_X86_SREGS;

	/*
	 * One-shot bypass: kvm_v2_load_user_sregs at the
	 * next dispatch would otherwise unconditionally re-arm TS,
	 * undoing the clear above. The flag tells it to skip the
	 * arming + clear the existing TS bit. Pairs with the same
	 * mechanism in kvm_v2_handle_io_nm().
	 */
	current->thread.arch.kvm_v2.nm_ts_bypass = true;

	/*
	 * Do not call kvm_v2_ist_frame_write(): the next dispatch marshals
	 * user_rip directly into KVM state instead of preparing a stub iretq.
	 */

	return 0;
}

/*
 * Inline #PF handling for asynchronous exits during IDT delivery. The CPU has
 * pushed the frame, but the handler stub has not completed. Handle the fault
 * against the current vCPU's IST page so a subsequent dispatch does not
 * replay a stub frame against a different per-vCPU IST mapping.
 */
int kvm_v2_handle_pf_eintr_inline(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu,
				  u64 cr2)
{
	struct kvm_v2_ist_frame frame;
	u64 stub_rip_at_eintr = regs->gp[HOST_IP];
	u64 pf_stub_start = KVM_V2_HANDLERS_GVA +
			    KVM_V2_HANDLER_SLOT_PF *
			    KVM_V2_HANDLER_SLOT_STRIDE;
	u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;

	kvm_v2_ist_frame_read(vcpu, &frame, true /* has_error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	/*
	 * The #PF stub saves user RAX at IST top-56 before loading CR2 into
	 * RAX. If an asynchronous exit lands after that load and before the
	 * pop, sync-regs reports RAX as CR2. When the stub has executed the
	 * push, restore saved user RAX before marshalling registers back to KVM.
	 */
	if (stub_rip_at_eintr > pf_stub_start)
		regs->gp[HOST_AX] = *(u64 *)(top - 56);

	regs->faultinfo.error_code = (int)frame.error_code;
	regs->faultinfo.cr2        = cr2;
	regs->faultinfo.trap_no    = 14;

	trace_um_backend_kvm_v2_iotrap_pf(cr2,
					  frame.error_code,
					  frame.user_rip);

	segv_handler(SIGSEGV, NULL, regs, NULL);
	interrupt_end();

	kvm_v2_ist_frame_write(vcpu, regs, true /* has_error_code */);
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

	return 0;
}

static void kvm_v2_ist_frame_to_regs(struct uml_pt_regs *regs,
				     const struct kvm_v2_ist_frame *frame,
				     int trap_no, int error_code, u64 cr2);
static void kvm_v2_ist_writeback(struct uml_pt_regs *regs,
				 struct kvm_run *run,
				 struct kvm_v2_vcpu *vcpu,
				 bool has_error_code);

/*
 * #PF (vector 14): page-fault dispatcher. CR2 comes from sync-regs sregs;
 * error_code and user RIP/RSP/RFLAGS come from the IST frame. This path
 * enters UML's normal segv_handler(), matching the seccomp backend SIGSEGV
 * path.
 *
 * Set regs->is_user before dispatch. The faulting access happened at
 * guest CPL=3 even though the in-guest handler stub runs at CPL=0 on
 * the IST stack. UML's segv_handler uses UPT_IS_USER(regs) to choose
 * the user-fault path for user-range CR2 values.
 */
static u64 kvm_v2_pf_fault_address(struct kvm_v2_vcpu *vcpu,
				   const struct kvm_v2_ist_frame *frame,
				   u64 sync_cr2)
{
	u64 cr2 = sync_cr2;
	u64 captured_cr2 = *(u64 *)((u8 *)vcpu->ist_stack_kva +
				    PAGE_SIZE - 64);
	u64 captured_rdx = *(u64 *)((u8 *)vcpu->ist_stack_kva +
				    PAGE_SIZE - 72);

	/*
	 * The #PF stub captures CR2 before the out vmexit. Prefer it when
	 * sync-regs CR2 is zero so we do not report a false NULL fault.
	 */
	if (cr2 == 0 && captured_cr2 != 0)
		cr2 = captured_cr2;

	/*
	 * If CR2 is still unavailable, use RDX as a conservative fallback for
	 * write faults where it holds a plausible userspace pointer.
	 */
	if (cr2 == 0 && (frame->error_code & 0x2) && captured_rdx > 0x10000)
		cr2 = captured_rdx;

	return cr2;
}

static void kvm_v2_pf_warn_frame(const struct kvm_v2_ist_frame *frame, u64 cr2)
{
	if (frame->user_rip >= KVM_V2_TRAMPOLINE_GVA &&
	    frame->user_rip < KVM_V2_TRAMPOLINE_GVA + 0x4000)
		pr_warn_ratelimited("um: kvm-v2 kernel-half-user-rip user_rip=%llx err=%llx pid=%d comm=%s sp=%llx\n",
				    (unsigned long long)frame->user_rip,
				    (unsigned long long)frame->error_code,
				    current->pid, current->comm,
				    (unsigned long long)frame->user_rsp);

	/*
	 * #PF error code bit 3 (RSV) means the page-table walk encountered a
	 * reserved bit. Log the first failures without flooding.
	 */
	if (frame->error_code & 0x8) {
		static int rsv_warn_count;

		if (rsv_warn_count < 5) {
			rsv_warn_count++;
			pr_warn_ratelimited("um: kvm-v2 pf-rsv[%d] cr2=%llx user_rip=%llx err=%llx pid=%d comm=%s\n",
					    rsv_warn_count,
					    (unsigned long long)cr2,
					    (unsigned long long)frame->user_rip,
					    (unsigned long long)frame->error_code,
					    current->pid, current->comm);
		}
	}
}

static int kvm_v2_handle_io_pf(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;
	u64 cr2;

	kvm_v2_ist_frame_read(vcpu, &frame, true /* has_error_code */);

	/*
	 * KVM populates s.regs.sregs.cr2 on every KVM_EXIT_IO when
	 * KVM_SYNC_X86_SREGS is in kvm_valid_regs. No separate
	 * KVM_GET_SREGS ioctl is needed.
	 */
	cr2 = run->s.regs.sregs.cr2;

	cr2 = kvm_v2_pf_fault_address(vcpu, &frame, cr2);
	kvm_v2_pf_warn_frame(&frame, cr2);

	kvm_v2_ist_frame_to_regs(regs, &frame, 14, (int)frame.error_code, cr2);

	trace_um_backend_kvm_v2_iotrap_pf(cr2,
					  frame.error_code,
					  frame.user_rip);

	/*
	 * Dispatch into UML's existing handler. segv_handler ignores its
	 * unused siginfo argument, and the signal routing table maps SIGSEGV
	 * here, so call it directly.
	 */
	segv_handler(SIGSEGV, NULL, regs, NULL);

	/*
	 * Drain pending signal and scheduler work before marshalling regs back
	 * to the IST frame. If segv_handler() queued a terminating
	 * SIGSEGV/SIGBUS, interrupt_end() installs the signal frame or starts
	 * task exit before the in-guest iretq returns to userspace.
	 */
	interrupt_end();

	kvm_v2_ist_writeback(regs, run, vcpu, true /* has_error_code */);

	return 0;
}

static void kvm_v2_ist_frame_to_regs(struct uml_pt_regs *regs,
				     const struct kvm_v2_ist_frame *frame,
				     int trap_no, int error_code, u64 cr2)
{
	regs->gp[HOST_IP]     = frame->user_rip;
	regs->gp[HOST_SP]     = frame->user_rsp;
	regs->gp[HOST_EFLAGS] = frame->user_rflags;
	regs->is_user         = 1;

	regs->faultinfo.error_code = error_code;
	regs->faultinfo.cr2        = cr2;
	regs->faultinfo.trap_no    = trap_no;
}

static void kvm_v2_ist_writeback(struct uml_pt_regs *regs,
				 struct kvm_run *run,
				 struct kvm_v2_vcpu *vcpu,
				 bool has_error_code)
{
	kvm_v2_ist_frame_write(vcpu, regs, has_error_code);
	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;
}

/*
 * #GP (vector 13): general-protection-fault dispatcher. Same shape
 * as #PF (CPU pushes error code) except trap_no=13 and there's no
 * cr2 to read. Dispatches as SIGSEGV via segv_handler;
 * SEGV_IS_FIXABLE returns false (trap_no != 14), so segv_handler's
 * user-fault path continues to bad_segv and force_sig_fault(SIGSEGV).
 */
static int kvm_v2_handle_io_gp(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, true /* has_error_code */);

	kvm_v2_ist_frame_to_regs(regs, &frame, 13, (int)frame.error_code, 0);

	trace_um_backend_kvm_v2_iotrap_gp(frame.error_code, frame.user_rip);

	/*
	 * SIGSEGV via segv_handler: same as #PF dispatch, but trap_no != 14
	 * sends the fault through bad_segv and force_sig_fault().
	 */
	segv_handler(SIGSEGV, NULL, regs, NULL);

	/* Drain pending signal/scheduler work -- see #PF handler comment. */
	interrupt_end();

	kvm_v2_ist_writeback(regs, run, vcpu, true /* has_error_code */);

	return 0;
}

/*
 * Common helper for #UD/#DE/#OF, vectors without error code that dispatch
 * through relay_signal().
 * relay_signal dereferences si->si_code and si->si_errno, so build
 * a stack-allocated kernel_siginfo with sensible si_code (which selects
 * SIL_FAULT layout in siginfo_layout, routing to force_sig_fault with
 * FAULT_ADDRESS(faultinfo) = cr2 = 0. The trap_no field disambiguates
 * the actual cause for downstream code that examines
 * current->thread.arch.faultinfo).
 *
 * The si_codes chosen (ILL_ILLOPN / FPE_INTOVF / SEGV_MAPERR) are
 * conservative defaults. UML's relay_signal forwards them verbatim
 * through force_sig_fault, so userspace handlers reading siginfo see
 * a plausible cause.
 */
static void kvm_v2_dispatch_relay(struct uml_pt_regs *regs, int sig,
				  int si_code)
{
	kernel_siginfo_t info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_code  = si_code;
	info.si_errno = 0;
	relay_signal(sig, (struct siginfo *)&info, regs, NULL);
}

/*
 * #UD (vector 6): undefined-opcode dispatcher. No error code.
 * Delivers SIGILL via relay_signal. UML's userspace handler sees
 * si_code = ILL_ILLOPN.
 */
static int kvm_v2_handle_io_ud(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	kvm_v2_ist_frame_to_regs(regs, &frame, 6, 0, 0);

	trace_um_backend_kvm_v2_iotrap_ud(frame.user_rip);

	kvm_v2_dispatch_relay(regs, SIGILL, ILL_ILLOPN);

	/* Drain pending signal/scheduler work -- see #PF handler comment. */
	interrupt_end();

	kvm_v2_ist_writeback(regs, run, vcpu, false /* no error_code */);

	return 0;
}

/*
 * #DE (vector 0): divide-error dispatcher. No error code.
 * Delivers SIGFPE via relay_signal with si_code=FPE_INTOVF.
 */
static int kvm_v2_handle_io_de(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	kvm_v2_ist_frame_to_regs(regs, &frame, 0, 0, 0);

	trace_um_backend_kvm_v2_iotrap_de(frame.user_rip);

	kvm_v2_dispatch_relay(regs, SIGFPE, FPE_INTOVF);

	/* Drain pending signal/scheduler work -- see #PF handler comment. */
	interrupt_end();

	kvm_v2_ist_writeback(regs, run, vcpu, false /* no error_code */);

	return 0;
}

/*
 * #OF (vector 4): overflow dispatcher. No error code. Triggered by
 * the into instruction. It is rare in long mode, where into is
 * invalid, but keep the IDT gate for completeness. Delivers SIGSEGV via
 * relay_signal.
 */
static int kvm_v2_handle_io_of(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	kvm_v2_ist_frame_to_regs(regs, &frame, 4, 0, 0);

	trace_um_backend_kvm_v2_iotrap_of(frame.user_rip);

	/*
	 * Use relay_signal rather than segv_handler: the latter expects
	 * trap_no=14 for the SEGV_IS_FIXABLE branch, and #OF has trap_no=4.
	 * Going through relay_signal lands the signal directly without the
	 * fault-fix detour.
	 */
	kvm_v2_dispatch_relay(regs, SIGSEGV, SEGV_MAPERR);

	/* Drain pending signal/scheduler work -- see #PF handler comment. */
	interrupt_end();

	kvm_v2_ist_writeback(regs, run, vcpu, false /* no error_code */);

	return 0;
}

/*
 * #NM (vec 7) host-side handler.
 *
 * The in-guest stub out %al, $UM_KVM_TRAP_NM ; iretq traps to here
 * via KVM_EXIT_IO:
 *   1. Read the IDT-pushed iretq frame (no error_code) from IST.
 *   2. Marshal user state into regs (HOST_IP/SP/EFLAGS).
 *   3. Clear sregs.cr0.TS = host-side clts emulation.
 *   4. Set arch_thread.kvm_v2.nm_ts_bypass = true so the next
 *      kvm_v2_load_user_sregs skips the unconditional TS arming and
 *      clears the existing TS bit. Without that, the user's FP
 *      instruction would fault again on retry.
 *   5. Drain pending signal/scheduler work (interrupt_end).
 *   6. Marshal regs back into kvm_run->s.regs.regs.
 *   7. Return; next KVM_RUN re-enters at frame.user_rip with TS=0.
 *
 * The in-guest iretq tail is unreachable: user state is marshalled
 * directly via SYNC_REGS, bypassing any iretq from IST. This eliminates
 * the stale-IST iretq surface.
 */
static int kvm_v2_handle_io_nm(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	regs->gp[HOST_IP]     = frame.user_rip;
	regs->gp[HOST_SP]     = frame.user_rsp;
	regs->gp[HOST_EFLAGS] = frame.user_rflags;
	regs->is_user         = 1;

	/* Host-side clts emulation. */
	run->s.regs.sregs.cr0 &= ~X86_CR0_TS;
	run->kvm_dirty_regs   |= KVM_SYNC_X86_SREGS;

	/*
	 * One-shot bypass: load_user_sregs at the next dispatch will see this
	 * flag, skip the TS re-arm, and clear the flag.
	 */
	current->thread.arch.kvm_v2.nm_ts_bypass = true;

	/*
	 * The next KVM_RUN will execute the user FPU instruction that this
	 * #NM handler is unblocking. That touches the vCPU's guest FPU, so
	 * force the post-vmexit GET regardless of the post-run TS readback.
	 */
	vcpu->fpu_dirty = true;

	/* Drain pending signal and scheduler work before guest re-entry. */
	interrupt_end();

	kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
	run->kvm_dirty_regs   |= KVM_SYNC_X86_REGS;

	return 0;
}

/*
 * Default-stub dispatcher. Any vector without a dedicated handler points
 * at the panic port. The firing vector may or may not have pushed an
 * error code; the no-error-code layout is enough to report the user RIP
 * and deliver SIGSEGV.
 *
 * Hitting this path means either an unimplemented vector fired or the
 * IDT gate points at the wrong stub.
 */
static int kvm_v2_handle_io_panic(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);

	trace_um_backend_kvm_v2_iotrap_panic(run->io.port, frame.user_rip);

	/*
	 * Deliver SIGSEGV to the faulting process instead of panicking
	 * the entire kernel. The panic stub fires for any IDT vector
	 * without a dedicated handler (#DB, #AC, #XM, etc.). These are
	 * user-mode faults that should kill the offending process, not the
	 * kernel. Follow the #GP handler pattern.
	 */
	pr_warn_ratelimited("kvm-v2: unhandled vec port=%#x cpu=%d pid=%d run=%#llx rip=%#llx\n",
			    run->io.port, vcpu->cpu, current->pid,
			    (u64)run->s.regs.regs.rip, frame.user_rip);

	kvm_v2_ist_frame_to_regs(regs, &frame, 0, 0, 0);

	segv_handler(SIGSEGV, NULL, regs, NULL);

	interrupt_end();

	kvm_v2_ist_writeback(regs, run, vcpu, false /* no error code */);

	return 0;
}

static int kvm_v2_handle_io_relay(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu,
				  bool has_error_code, int trap_no,
				  int sig, int si_code)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, has_error_code);
	kvm_v2_ist_frame_to_regs(regs, &frame, trap_no,
				 has_error_code ? (int)frame.error_code : 0,
				 0);
	kvm_v2_dispatch_relay(regs, sig, si_code);
	interrupt_end();
	kvm_v2_ist_writeback(regs, run, vcpu, has_error_code);

	return 0;
}

static unsigned long kvm_v2_prepare_syscall_regs(struct uml_pt_regs *regs)
{
	unsigned long syscall_nr;

	/* KVM sync regs are already marshalled; RAX holds the syscall NR. */
	syscall_nr = regs->gp[HOST_AX];

	PT_SYSCALL_NR(regs->gp) = syscall_nr;
	regs->is_user = 1;

	/*
	 * Post-SYSCALL semantics: user RIP arrives in RCX, user RFLAGS in
	 * R11. Copy them into UML's canonical slots before handle_syscall()
	 * and signal delivery inspect the user frame.
	 */
	regs->gp[HOST_IP]     = regs->gp[HOST_CX];
	regs->gp[HOST_EFLAGS] = regs->gp[HOST_R11];

	/*
	 * Default matching the seccomp backend pattern. handle_syscall()
	 * re-reads PT_SYSCALL_NR into UPT_SYSCALL_NR at entry, so this
	 * assignment is overwritten immediately.
	 */
	UPT_SYSCALL_NR(regs) = -1;

	return syscall_nr;
}

static void kvm_v2_run_syscall_unpinned(struct uml_pt_regs *regs)
{
	/*
	 * handle_syscall() can schedule. Drop the migration pin because this
	 * path touches only per-task regs; the caller re-fetches vcpu/run
	 * before using them again.
	 */
	migrate_enable();
	handle_syscall(regs);
	migrate_disable();
}

static void kvm_v2_drain_syscall_work(struct uml_pt_regs *regs)
{
	long ret;

	/*
	 * Drain pending signal and scheduler work after the syscall returns.
	 * This must run before PT_SYSCALL_NR is cleared because do_signal()
	 * uses that slot to decide whether -ERESTART* handling applies.
	 */
	ret = (long)regs->gp[HOST_AX];
	if (unlikely((ret <= -512 && ret >= -516) ||
		     (read_thread_flags() & _TIF_WORK_MASK)))
		interrupt_end();
}

static void kvm_v2_clear_syscall_nr(struct uml_pt_regs *regs)
{
	/*
	 * Clear after signal/restart handling. The offset guard protects
	 * layouts where the syscall-number and return-value slots alias.
	 */
	if (PT_SYSCALL_NR_OFFSET != PT_SYSCALL_RET_OFFSET)
		PT_SYSCALL_NR(regs->gp) = -1;
}

#ifdef CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL
static unsigned int kvm_v2_record_payload_arg(unsigned long syscall_nr)
{
	switch (syscall_nr) {
#ifdef __NR_clock_gettime
	case __NR_clock_gettime:
		return 1;
#endif
	case __NR_getcwd:
	case __NR_uname:
		return 0;
	default:
		return 6;
	}
}

static unsigned long kvm_v2_record_payload_ptr(const struct uml_pt_regs *regs,
					       unsigned int arg)
{
	switch (arg) {
	case 0:
		return regs->gp[HOST_DI];
	case 1:
		return regs->gp[HOST_SI];
	case 2:
		return regs->gp[HOST_DX];
	case 3:
		return regs->gp[HOST_R10];
	case 4:
		return regs->gp[HOST_R8];
	case 5:
		return regs->gp[HOST_R9];
	default:
		return 0;
	}
}

static size_t kvm_v2_record_payload_len(unsigned long syscall_nr, long ret)
{
	switch (syscall_nr) {
#ifdef __NR_clock_gettime
	case __NR_clock_gettime:
		if (ret < 0)
			return 0;
		return sizeof(struct __kernel_timespec);
#endif
	case __NR_getcwd:
		if (ret <= 0 || ret > KVM_V2_RECORD_MAX_PAYLOAD)
			return 0;
		return (size_t)ret;
	case __NR_uname:
		if (ret < 0)
			return 0;
		return sizeof(struct new_utsname);
	default:
		return 0;
	}
}

static size_t kvm_v2_record_payload_max_len(unsigned long syscall_nr,
					    const struct uml_pt_regs *regs)
{
	switch (syscall_nr) {
#ifdef __NR_clock_gettime
	case __NR_clock_gettime:
		return sizeof(struct __kernel_timespec);
#endif
	case __NR_getcwd:
		return min_t(size_t, regs->gp[HOST_SI],
			     KVM_V2_RECORD_MAX_PAYLOAD);
	case __NR_uname:
		return sizeof(struct new_utsname);
	default:
		return 0;
	}
}

static int kvm_v2_record_replay_payload(struct kvm_v2_record *rec,
					struct uml_pt_regs *regs,
					unsigned long syscall_nr,
					long *served_ret)
{
	void *payload;
	unsigned int arg;
	unsigned long user_ptr;
	size_t max_len;
	size_t payload_len = 0;
	int rc;

	if (!kvm_v2_record_syscall_has_payload(syscall_nr))
		return 0;

	arg = kvm_v2_record_payload_arg(syscall_nr);
	user_ptr = kvm_v2_record_payload_ptr(regs, arg);
	if (!user_ptr)
		return 0;

	max_len = kvm_v2_record_payload_max_len(syscall_nr, regs);
	if (!max_len)
		return 0;

	payload = kmalloc(max_len, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;

	rc = kvm_v2_record_consume_syscall_payload(rec, syscall_nr, regs,
						   served_ret, payload,
						   max_len, &payload_len);
	if (rc <= 0)
		goto out_free;

	if (*served_ret >= 0 && payload_len &&
	    copy_to_user((void __user *)user_ptr, payload, payload_len))
		rc = -EFAULT;

out_free:
	kfree(payload);
	return rc;
}

static bool kvm_v2_try_replay_syscall(struct uml_pt_regs *regs,
				      unsigned long syscall_nr)
{
	struct kvm_v2_record *rec;
	long served_ret = 0;
	int rc;

	if (!static_branch_unlikely(&um_kvm_v2_record_enabled))
		return false;

	rec = kvm_v2_record_active();
	if (!rec || rec->state != KVM_V2_RECORD_REPLAYING)
		return false;
	rc = kvm_v2_record_check_strict_syscall(rec, syscall_nr);
	if (rc < 0) {
		pr_info_ratelimited("kvm-v2 record: strict replay unsupported nr=%lu\n",
				    syscall_nr);
		force_sig(SIGSEGV);
		return true;
	}

	rc = kvm_v2_record_replay_payload(rec, regs, syscall_nr, &served_ret);
	if (!rc)
		rc = kvm_v2_record_consume_syscall(rec, syscall_nr, &served_ret);
	if (rc > 0) {
		regs->gp[HOST_AX] = (unsigned long)served_ret;
		kvm_v2_record_finish_replay_if_complete(rec);
		return true;
	}

	if (rc < 0 && rec->strict_replay) {
		kvm_v2_record_note_replay_failure(rec, syscall_nr, rc);
		pr_info_ratelimited("kvm-v2 record: strict replay divergence nr=%lu rc=%d entries_replayed=%llu\n",
				    syscall_nr, rc, rec->entries_replayed);
		force_sig(SIGSEGV);
		return true;
	}

	return false;
}

static bool kvm_v2_record_observe_payload(struct kvm_v2_record *rec,
					  struct uml_pt_regs *regs,
					  unsigned long syscall_nr)
{
	void *payload;
	unsigned int arg;
	unsigned long user_ptr;
	size_t payload_len;
	long ret = (long)regs->gp[HOST_AX];
	bool observed = false;

	payload_len = kvm_v2_record_payload_len(syscall_nr, ret);
	if (!payload_len)
		return false;

	arg = kvm_v2_record_payload_arg(syscall_nr);
	user_ptr = kvm_v2_record_payload_ptr(regs, arg);
	if (!user_ptr)
		return false;

	payload = kmalloc(payload_len, GFP_KERNEL);
	if (!payload)
		return false;

	if (copy_from_user(payload, (void __user *)user_ptr, payload_len))
		goto out_free;

	observed = kvm_v2_record_observe_syscall_payload(rec, syscall_nr, ret,
							 regs, arg, payload,
							 payload_len) > 0;

out_free:
	kfree(payload);
	return observed;
}

static void kvm_v2_observe_syscall(struct uml_pt_regs *regs,
				   unsigned long syscall_nr)
{
	struct kvm_v2_record *rec;

	if (!static_branch_unlikely(&um_kvm_v2_record_enabled))
		return;

	rec = kvm_v2_record_active();
	if (rec && !kvm_v2_record_observe_payload(rec, regs, syscall_nr))
		kvm_v2_record_observe_syscall(rec, syscall_nr,
					      (long)regs->gp[HOST_AX], regs);
}
#else
static bool kvm_v2_try_replay_syscall(struct uml_pt_regs *regs,
				      unsigned long syscall_nr)
{
	(void)regs;
	(void)syscall_nr;

	return false;
}

static void kvm_v2_observe_syscall(struct uml_pt_regs *regs,
				   unsigned long syscall_nr)
{
	(void)regs;
	(void)syscall_nr;
}
#endif

static int kvm_v2_handle_io_syscall(struct uml_pt_regs *regs, u16 io_port)
{
	unsigned long syscall_nr;

	syscall_nr = kvm_v2_prepare_syscall_regs(regs);
	trace_um_backend_kvm_v2_iotrap_syscall_enter(io_port, syscall_nr);

	if (kvm_v2_try_replay_syscall(regs, syscall_nr))
		goto out_clear;

	kvm_v2_run_syscall_unpinned(regs);
	kvm_v2_drain_syscall_work(regs);
	kvm_v2_observe_syscall(regs, syscall_nr);

	/*
	 * Do not marshal into kvm_run here: a sleeping syscall may have let
	 * another task reuse the same per-CPU vCPU mmap.
	 */
out_clear:
	kvm_v2_clear_syscall_nr(regs);
	trace_um_backend_kvm_v2_iotrap_syscall_exit(io_port, regs->gp[HOST_AX]);
	return 0;
}

static int kvm_v2_handle_io_ss(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, true);
	kvm_v2_ist_frame_to_regs(regs, &frame, 12, (int)frame.error_code, 0);
	segv_handler(SIGSEGV, NULL, regs, NULL);
	interrupt_end();
	kvm_v2_ist_writeback(regs, run, vcpu, true);
	return 0;
}

static int kvm_v2_handle_io_df(struct uml_pt_regs *regs,
			       struct kvm_run *run,
			       struct kvm_v2_vcpu *vcpu)
{
	struct kvm_v2_ist_frame frame;

	kvm_v2_ist_frame_read(vcpu, &frame, true);
	kvm_v2_ist_frame_to_regs(regs, &frame, 8, 0, 0);
	pr_warn_ratelimited("kvm-v2: #DF double fault cpu=%d pid=%d rip=%#llx\n",
			    vcpu->cpu, current->pid, frame.user_rip);
	segv_handler(SIGSEGV, NULL, regs, NULL);
	interrupt_end();
	kvm_v2_ist_writeback(regs, run, vcpu, true);
	return 0;
}

int kvm_v2_handle_io_trap(struct uml_pt_regs *regs,
			  struct kvm_run *run,
			  struct kvm_v2_vcpu *vcpu)
{
	if (!vcpu)
		return -EINVAL;

	/*
	 * Dispatch table for the per-class exception ports emitted by the
	 * IDT handler stubs. SYSCALL, PF, GP, UD, DE, OF, and NM have
	 * dedicated handlers; BP, DB, and AC share the relay helper;
	 * panic and unrecognised ports fall to the panic helper.
	 */
	switch (run->io.port) {
	case UM_KVM_TRAP_SYSCALL:
		return kvm_v2_handle_io_syscall(regs, run->io.port);
	case UM_KVM_TRAP_PF:
		return kvm_v2_handle_io_pf(regs, run, vcpu);
	case UM_KVM_TRAP_GP:
		return kvm_v2_handle_io_gp(regs, run, vcpu);
	case UM_KVM_TRAP_UD:
		return kvm_v2_handle_io_ud(regs, run, vcpu);
	case UM_KVM_TRAP_DE:
		return kvm_v2_handle_io_de(regs, run, vcpu);
	case UM_KVM_TRAP_OF:
		return kvm_v2_handle_io_of(regs, run, vcpu);
	case UM_KVM_TRAP_NM:
		return kvm_v2_handle_io_nm(regs, run, vcpu);
	case UM_KVM_TRAP_BP:
		return kvm_v2_handle_io_relay(regs, run, vcpu, false, 3,
					      SIGTRAP, TRAP_BRKPT);
	case UM_KVM_TRAP_DB:
		return kvm_v2_handle_io_relay(regs, run, vcpu, false, 1,
					      SIGTRAP, TRAP_TRACE);
	case UM_KVM_TRAP_SS:
		return kvm_v2_handle_io_ss(regs, run, vcpu);
	case UM_KVM_TRAP_AC:
		return kvm_v2_handle_io_relay(regs, run, vcpu, true, 17,
					      SIGBUS, BUS_ADRALN);
	case UM_KVM_TRAP_DF:
		return kvm_v2_handle_io_df(regs, run, vcpu);
	case UM_KVM_TRAP_PANIC:
	default:
		return kvm_v2_handle_io_panic(regs, run, vcpu);
	}
}
