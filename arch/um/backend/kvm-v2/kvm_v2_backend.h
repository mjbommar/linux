/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal header for arch/um/backend/kvm-v2/.
 *
 * Holds declarations shared by the private KVM backend translation units.
 * The backend contract and dispatch API live in <backend.h>; this header
 * contains KVM implementation details and the KVM ops-table singleton
 * selected by the backend arbiter.
 */
#ifndef __ARCH_UM_BACKEND_KVM_V2_H
#define __ARCH_UM_BACKEND_KVM_V2_H

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/kvm.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <backend.h>

struct kvm_cpuid2;
struct kvm_regs;
struct mm_struct;
struct task_struct;
struct uml_pt_regs;
struct um_memory_region;

/*
 * Per-VM memslot id ceiling. The host-side KVM definition
 * (linux/kvm_host.h: KVM_MEM_SLOTS_NUM = SHRT_MAX, KVM_USER_MEM_SLOTS =
 * KVM_MEM_SLOTS_NUM - KVM_INTERNAL_MEM_SLOTS, with internal slots == 0
 * on x86) is host-only and not exported through uapi/linux/kvm.h. Pin
 * this copy to SHRT_MAX so the bitmap matches what KVM will actually
 * accept from KVM_SET_USER_MEMORY_REGION. If a newer host raises the
 * limit, the extra slots remain unused.
 */
#define KVM_V2_MAX_USER_MEM_SLOTS	32767

/*
 * Per-dispatch TLB generation lag at which kvm_v2_load_user_sregs()
 * forces a full KVM_SET_SREGS ioctl in addition to the cross-task gate.
 * The full ioctl drops KVM's per-vCPU prev_roots[] cache through
 * kvm_mmu_reset_context. A low threshold prevents obsolete translations
 * from surviving repeated remote invalidations while still allowing the
 * cheap sync-regs path for the common case.
 */
#define KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD	3

/*
 * Per-UML-kernel-invocation VM context. A single instance lives in
 * context.c and is available through kvm_v2_vm_get() after
 * kvm_v2_vm_create() succeeds. @memslots tracks registered KVM memory
 * slots; @cpuid holds the curated CPUID2 buffer installed lazily on
 * each vCPU once allocation is safe.
 */
struct kvm_v2_vm {
	int			kvm_fd;
	int			vm_fd;
	u64			caps;
	struct kvm_cpuid2	*cpuid;
	struct list_head	memslots;
	/*
	 * Per-VM bitmap of allocated memslot ids. One bit is kept for each
	 * KVM user memslot and cleared on free. The bitmap and memslot list
	 * are protected by @lock.
	 */
	unsigned long		memslot_bitmap[BITS_TO_LONGS(KVM_V2_MAX_USER_MEM_SLOTS)];
	/* Protects @memslots and @memslot_bitmap. */
	spinlock_t		lock;
	/*
	 * LSTAR trampoline page. @trampoline_page is the host kernel VA of
	 * a buddy-allocated page; @trampoline_gpa is the physical address
	 * exposed to KVM as the guest physical address. The trampoline body
	 * lives at KVM_V2_TRAMPOLINE_LSTAR_OFFSET and is the target of the
	 * guest MSR_LSTAR. The field stays NULL until allocation succeeds;
	 * early VM creation can run before the buddy allocator is ready, so
	 * callers must tolerate a retry.
	 */
	void			*trampoline_page;
	phys_addr_t		trampoline_gpa;
	/*
	 * Memslot id for the UML physmem identity-offset mapping. The slot
	 * maps gpa=0 to hva=uml_physmem for physmem_size bytes so KVM can
	 * translate page-table, descriptor-table, trampoline, IST, TSS, and
	 * gadget-state pages allocated from UML physmem. A value of -1 means
	 * the slot has not been installed yet; non-negative values are
	 * allocated memslot ids released by the normal memslot teardown.
	 */
	int			physmem_memslot_id;
	/*
	 * Page-table chain for the kernel-half PML4[448] install. The
	 * trampoline at KVM_V2_LSTAR_GVA requires a guest PUD/PMD/PTE walk
	 * from PML4[448] down to the trampoline page. These VM-lifetime
	 * pages live in UML physmem, so their __pa() values are covered by
	 * the physmem memslot above.
	 *
	 * Storing only the PUD GPA in the struct is enough; the PMD/PTE
	 * pages are kept by their KVAs for free-time. The PUD GPA is
	 * written to swapper_pg_dir[448] and init_mm.pgd[448] to seed
	 * kernel-half propagation through pgd_alloc(). NULL-valued KVAs mean
	 * the chain is not installed yet; the late-install path fills it once
	 * both the physmem memslot and trampoline GPA are available.
	 */
	void			*trampoline_pud_kva;	/* PUD page; entry [0] = pmd_pa */
	void			*trampoline_pmd_kva;	/* PMD page; entry [0] = pte_pa */
	void			*trampoline_pte_kva;	/* PTE page; entry [0] = trampoline_gpa */
	phys_addr_t		trampoline_pud_gpa;
	/*
	 * IDT, exception handler stubs, and GDT pages. The IDT, handler
	 * table, and GDT are separate pages because a 256-entry long-mode
	 * IDT consumes a full 4 KB page.
	 *
	 * Layout (all under PML4[448]/PUD[0]/PMD[0], at PTE[1..3]):
	 *   idt_*       -- 256 x 16-byte gate descriptors. Vector 14
	 *                 (#PF), 13 (#GP), 6 (#UD), 0 (#DE), 4 (#OF)
	 *                 point at handler stubs in handlers_kva; vector
	 *                 3 (#BP) gates with DPL=3 for user int3. Other
	 *                 vectors point at a panic stub (port
	 *                 UM_KVM_TRAP_PANIC = 0xf8).
	 *   handlers_*  -- 64-byte slots holding the out %al, $port
	 *                 handler stubs. The #PF stub also captures CR2
	 *                 in the IST page before exiting. Other stubs
	 *                 are short and leave the rest of their slot zero.
	 *   gdt_*       -- 8 x 8-byte entries (slot [0] null, [1] kern
	 *                 CS, [2] kern DS, [3] STAR-base padding, [4]
	 *                 user DS @0x23 DPL=3, [5] user CS @0x2b DPL=3,
	 *                 [6..7] TSS descriptor).
	 *
	 * VM-lifetime; freed in vm_destroy via kvm_v2_exception_free()
	 * before kvm_v2_kernel_half_free() so PTE[1..3] dereferences stay
	 * valid until the PT chain itself drops.
	 */
	void			*idt_kva;
	phys_addr_t		idt_gpa;
	void			*handlers_kva;
	phys_addr_t		handlers_gpa;
	void			*gdt_kva;
	phys_addr_t		gdt_gpa;
};

/*
 * In-memory memslot record. One exists per registered KVM memory slot
 * and lives on @vm->memslots until kvm_v2_memslot_del() removes it.
 *
 * @gpa and @host_va are intentionally separate: the physmem slot maps
 * guest physical offset 0 to the UML physmem host mapping, and other slot
 * users may need different GPA/HVA relationships.
 */
struct kvm_v2_memslot {
	struct list_head	list;
	u64			gpa;
	u64			host_va;
	u64			size;
	u32			slot_id;
	u32			flags;	/* KVM_MEM_READONLY etc. */
};

/*
 * Required backend ops cannot be NULL: validate_required_ops() panics
 * for missing entries, and the dispatch macro does not synthesize
 * -ENOSYS for optional entries. The KVM backend may delegate individual
 * operations to seccomp helpers, but the table must always be complete.
 */
extern const struct um_backend_ops um_backend_kvm_v2_ops;

/* v2 lifecycle ops -- defined in init.c. */
int  kvm_v2_probe(void);
int  kvm_v2_init(const struct um_backend_args *args);
void kvm_v2_shutdown(void);

/* Per-VM context lifecycle, defined in context.c. */
int  kvm_v2_vm_create(int kvm_fd, u64 caps);
void kvm_v2_vm_destroy(void);
struct kvm_v2_vm *kvm_v2_vm_get(void);

/*
 * APERF/MPERF MSR passthrough toggle. Defined in aperfmperf.c when
 * CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y; call sites are
 * compiled out otherwise. See aperfmperf.c and
 * Documentation/virt/uml/aperf-mperf.rst.
 */
#ifdef CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH
bool kvm_v2_aperfmperf_enabled(void);
void kvm_v2_aperfmperf_record_ioctl(int rc);
/*
 * True iff vm_create issued KVM_ENABLE_CAP AND KVM accepted it.
 * The h_aperfmperf gadget body consumes this via the per-vCPU
 * KVM_V2_GADGET_OFF_APERF_CAP byte programmed by exception.c at
 * gadget-state install time.  When false, the gadget falls back
 * to the host trap path so rdmsr is NOT executed at guest CPL=0
 * (KVM would deliver a #GP injection without the cap).
 */
bool kvm_v2_aperfmperf_cap_active(void);
#endif

/*
 * Physmem identity-offset memslot install.
 * Idempotent -- re-invocation after a successful install short-circuits.
 * Returns 0 on success / already-installed, -EAGAIN if uml_physmem /
 * physmem_size are not initialized yet and the caller should defer to the
 * lazy retry path, or any other negative errno from
 * KVM_SET_USER_MEMORY_REGION on ioctl failure.
 *
 * vm_create's eager attempt may hit the -EAGAIN branch because
 * init_backend() runs before linux_main() finishes populating
 * uml_physmem / physmem_size. The subsys_initcall lazy retry in
 * syscall_trap.c completes the install once the globals are stable.
 *
 * Defined in context.c.
 */
int  kvm_v2_physmem_memslot_install(struct kvm_v2_vm *vm);

/*
 * Install the per-VM kernel-half PT chain at PML4[448] so the LSTAR
 * trampoline GVA is reachable from any guest CR3 used by the KVM path.
 *
 * Allocates 3 pages (PUD/PMD/PTE) from buddy, writes the chain with x86
 * hardware page-table bits (P|RW|A for non-leaf entries, P|A for the
 * read-only supervisor leaf), and seeds swapper_pg_dir[448] plus
 * init_mm.pgd[448] so pgd_alloc() propagates the entry into every
 * subsequent mm. Idempotent re-invocation after a successful install
 * short-circuits via the trampoline_pud_kva sentinel.
 *
 * Prerequisites: vm->trampoline_gpa must be set and the physmem memslot
 * must cover [0, physmem_size).
 *
 * Returns 0 on success / already-installed; -EINVAL when prerequisites are
 * unmet; -ENOMEM if alloc_page returns NULL and the caller should defer to
 * the lazy retry path; other negative errno on ioctl failure.
 *
 * Defined in syscall_trap.c (alongside the trampoline install -- both
 * sides of the LSTAR install live in the same TU because the PT
 * chain's leaf entry references the trampoline GPA).
 */
int  kvm_v2_kernel_half_install(struct kvm_v2_vm *vm);

/*
 * Symmetric teardown: free the PUD/PMD/PTE pages and clear
 * swapper_pg_dir[448] + init_mm.pgd[448] so any concurrent mm operation
 * sees zero rather than dangling. Called from kvm_v2_vm_destroy. Safe
 * on a never-installed VM (NULL pud_kva -> no-op).
 */
void kvm_v2_kernel_half_free(struct kvm_v2_vm *vm);

/*
 * Per-host-CPU vCPU pool member. One element is populated per possible
 * host CPU (nr_cpu_ids) up to KVM_V2_MAX_VCPUS; the backing array is
 * static so allocation lands cleanly during init_backend() before the
 * buddy allocator is up (vcpu.c documents the constraint).
 *
 * @cpu records the host CPU index this vCPU is pinned to so dispatch can
 * pick the matching pool entry. @cpuid_primed is a sticky guard for the
 * lazy KVM_SET_CPUID2 install; CPUID programming can require allocation,
 * so the first KVM_RUN path performs it after early init constraints no
 * longer apply.
 */
struct kvm_v2_vcpu {
	int   vcpu_fd;
	void *kvm_run;
	u32   kvm_run_size;
	int   cpu;
	bool  cpuid_primed;
	/*
	 * Snapshot of the KVM sync-regs mmap taken immediately after KVM_RUN
	 * returns and before UML unblocks host signals. The vCPU pool member is
	 * exclusively owned under migrate_disable() while this snapshot is used,
	 * so it can live here instead of on the dispatcher stack.
	 */
	struct kvm_regs run_regs;
	struct kvm_sregs run_sregs;
	u32 run_exit_reason;
	/*
	 * Per-vCPU IST stack and TSS for exception delivery. IST1 is loaded
	 * by every KVM exception gate. Without a valid per-vCPU TSS, the
	 * first guest exception would use a zero IST1 RSP, causing a nested
	 * delivery failure and a triple fault.
	 *
	 * The TSS must be per-vCPU: TR selects one active TSS per vCPU, and
	 * sharing a TSS would either share an IST stack across vCPUs or
	 * require rewriting IST1 on every dispatch.
	 *
	 * Allocated from buddy via __get_free_page() as one IST page and one
	 * TSS page per pool member. Pages live in physmem so their __pa()
	 * resolves through the physmem identity-offset memslot. VM-lifetime;
	 * freed before kvm_v2_kernel_half_free() because the per-vCPU PTE
	 * writes use trampoline_pte_kva from the kernel-half chain.
	 */
	void	    *ist_stack_kva;
	phys_addr_t  ist_stack_gpa;
	u64	     ist_stack_top_gva;
	void	    *tss_kva;
	phys_addr_t  tss_gpa;
	u64	     tss_gva;

	/*
	 * Cross-vCPU TLB-flush kick deduplication. Set from 0 to 1 before
	 * sending an IPI and reset by the kicked vCPU in load_user_sregs().
	 * At most one IPI is in flight per vCPU.
	 */
	atomic_t kick_pending;

	/*
	 * current_mm is set in load_user_sregs() to the mm being dispatched on
	 * this vCPU. The kicker uses it with mm->context.tlb_gen_seen_by[cpu]
	 * to send IPIs only to vCPUs running this mm and only while that CPU's
	 * view of this mm is stale.
	 *
	 * Set as a plain pointer (not RCU). The kicker tolerates old values:
	 * an unnecessary IPI is bounded by kick_pending deduplication.
	 */
	struct mm_struct *current_mm;

	/*
	 * Captured tlb_gen lag at the start of each dispatch:
	 * mm->context.tlb_gen -
	 * mm->context.tlb_gen_seen_by[cpu] before the seen slot is updated. The
	 * cross_task gate at the end of load_user_sregs() consumes this:
	 * when lag >= KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD, dispatch
	 * treats KVM's per-vCPU prev_roots[] cache for this mm's CR3 as out
	 * of date after repeated remote invalidations and issues the heavy
	 * KVM_SET_SREGS ioctl that drops prev_roots[] via
	 * __set_sregs2 -> kvm_mmu_reset_context. Plain field -- only ever
	 * read/written under this vCPU's dispatch thread.
	 */
	u64 last_dispatch_tlb_lag;

	/*
	 * CR2 ownership guard. Same-task re-entry must preserve hardware CR2
	 * because KVM may have queued a page fault for delivery before an
	 * asynchronous exit. Clearing CR2 in that window can lose the fault
	 * address before the in-guest handler stub reaches the host trap.
	 *
	 * Cross-task and cross-mm transitions still clear CR2 for isolation:
	 * a new task or freshly exec'd mm must not observe prior fault state
	 * from the previous owner of this vCPU.
	 */
	struct task_struct *last_task;
	struct mm_struct   *last_mm;

	/*
	 * Predicates the post-vmexit KVM_GET_FPU so it can be skipped
	 * when the vCPU's guest FPU is bit-identical to the per-task
	 * iotrap_fpu snapshot. The semantic is a per-vCPU dirty epoch,
	 * not the per-dispatch CR0.TS value.
	 *
	 * fpu_dirty=true => vCPU's guest_fpu may have diverged from
	 * fpu_owner_task's iotrap_fpu. Set on:
	 *   - vcpu_create_one (initial -- force the first GET).
	 *   - cross-task arrival (fpu_owner_task != current in
	 *     load_user_sregs) -- a different task may have run.
	 *   - post-vmexit sregs.cr0 & X86_CR0_TS == 0 -- guest used FPU.
	 *   - kvm_v2_handle_io_nm clearing CR0.TS (next dispatch will
	 *     execute the user FPU instruction the handler is unblocking).
	 *   - kvm_v2_fpu_install_on_first_run after a fresh KVM_SET_FPU
	 *     from fpu_valid, followed by recapturing the installed
	 *     snapshot back into iotrap_fpu so the per-task slot stays
	 *     authoritative.
	 *
	 * fpu_owner_task records which task fpu_dirty=false is
	 * relative to. Skip the GET only when both fpu_dirty=false
	 * and fpu_owner_task == current. Updated after every successful
	 * KVM_SET_FPU (pre-run install of iotrap_fpu) and KVM_GET_FPU
	 * (post-vmexit capture).
	 */
	bool		    fpu_dirty;
	struct task_struct *fpu_owner_task;

	/*
	 * Per-vCPU gadget state page. A 4 KB page is mapped at
	 * KVM_V2_GADGET_STATE_GVA(cpu) via
	 * PTE[KVM_V2_GADGET_BASE_SLOT + cpu] of the trampoline_pte_kva
	 * chain. MSR_KERNEL_GS_BASE for this vCPU is programmed to that
	 * GVA so the LSTAR fast-syscall gadget's
	 * swapgs ; mov %gs:OFFSET, ... reads from this vCPU's page
	 * exclusively. load_user_sregs refreshes the per-task ID fields,
	 * CPU id, and optional APERF/MPERF enable byte before each KVM_RUN.
	 *
	 * Different tasks can run on different vCPUs simultaneously, so a
	 * shared state page would race. Per-vCPU state confines writes to the
	 * host CPU thread that owns this vCPU; the gadget runs on the same
	 * vCPU, so writer and reader are exclusive in time without a lock.
	 *
	 * VM-lifetime; freed before kvm_v2_kernel_half_free() because the
	 * PTE write goes through trampoline_pte_kva from the kernel-half
	 * chain.
	 */
	void	    *gadget_state_kva;
	phys_addr_t  gadget_state_gpa;
	u64	     gadget_state_gva;

	/*
	 * Per-vCPU dispatch-path counters. kvm_v2_load_user_sregs() chooses
	 * between a full KVM_SET_SREGS ioctl, which forces KVM to rebuild
	 * MMU context, and the cheaper sync-regs dirty-bit path. These
	 * counters expose that split for debugfs and teardown logs.
	 *
	 * Plain u64 (not atomic): kvm_v2_load_user_sregs is only ever
	 * called from this vCPU's dispatch thread under migrate_disable(),
	 * so there is no concurrent writer. The destroy-side read is
	 * after kvm_v2_vcpu_destroy_one has serialised against all
	 * dispatch (the VM is tearing down).
	 */
	u64 dispatch_heavy_count;
	u64 dispatch_cheap_count;
};

int  kvm_v2_vcpu_create(struct kvm_v2_vm *vm);
void kvm_v2_vcpu_destroy(void);

/*
 * Per-CPU accessor for the dispatcher. Returns the vCPU pinned to host
 * CPU @cpu, or NULL if @cpu is out of range or the pool is not
 * initialised.
 */
struct kvm_v2_vcpu *kvm_v2_vcpu_get(int cpu);

/*
 * Install IDT/GDT bases and the TR cache in SREGS for one pool member.
 * Called after the descriptor-table pages and per-vCPU TSS have been
 * populated. Caller responsibility: invoke
 * kvm_v2_install_per_vcpu_ist_tss() first so @vcpu->tss_gva is valid.
 *
 * Defined in vcpu.c alongside the baseline SREGS install so both
 * SREGS update paths share the same translation unit.
 */
int kvm_v2_install_descriptors_sregs(struct kvm_v2_vm *vm,
				     struct kvm_v2_vcpu *vcpu);

/*
 * Allocate and install the per-vCPU IST stack and TSS pages. Called from
 * kvm_v2_exception_install() for every pool member before
 * kvm_v2_install_descriptors_sregs(), so SREGS.tr points at the freshly
 * installed TSS body.
 *
 * Allocates two zeroed pages, writes the TSS body's IST1 field, installs
 * PTE entries at PTE[KVM_V2_IST_BASE_SLOT + cpu*2 .. +1] of
 * @trampoline_pte_kva, and records the IST/TSS host and guest addresses
 * on the vCPU.
 *
 * Idempotent at the per-vCPU level -- re-invocation when ist_stack_kva
 * is already non-NULL short-circuits. The exception_install caller
 * is itself idempotent on vm->idt_kva, so a re-run from the late-
 * install path is safe end-to-end.
 *
 * Defined in exception.c alongside the IDT/GDT install for symmetric
 * teardown.
 */
int kvm_v2_install_per_vcpu_ist_tss(struct kvm_v2_vm *vm,
				    struct kvm_v2_vcpu *vcpu, int cpu);

/*
 * Symmetric per-vCPU teardown: clear PTE[KVM_V2_IST_BASE_SLOT + cpu*2 .. +1],
 * free the IST stack and TSS pages, and NULL the vcpu fields. Called from
 * kvm_v2_exception_free() over every pool member before PTE[1..3] and the
 * IDT/handlers/GDT pages are freed. Safe on a never-installed vCPU.
 */
void kvm_v2_exception_free_per_vcpu(struct kvm_v2_vm *vm,
				    struct kvm_v2_vcpu *vcpu, int cpu);

/*
 * Allocate the per-vCPU gadget state page and install the PTE entry at
 * PTE[KVM_V2_GADGET_BASE_SLOT + cpu] of the trampoline_pte_kva chain.
 * Called from kvm_v2_exception_install() alongside
 * kvm_v2_install_per_vcpu_ist_tss(). Stores the page addresses on the
 * vCPU for the load_user_sregs() refresh path.
 *
 * Idempotent at the per-vCPU level -- re-invocation when
 * gadget_state_kva is already non-NULL short-circuits.
 *
 * Returns 0 on success / already-installed; -EINVAL on prerequisites
 * unmet (vm->trampoline_pte_kva NULL); -ENOMEM if __get_free_page
 * returns NULL.
 *
 * Defined in exception.c.
 */
int kvm_v2_install_per_vcpu_gadget_state(struct kvm_v2_vm *vm,
					 struct kvm_v2_vcpu *vcpu, int cpu);

/*
 * Symmetric teardown -- clear PTE[KVM_V2_GADGET_BASE_SLOT + cpu], free
 * the page, NULL the vcpu fields. Called from kvm_v2_exception_free
 * over every pool member alongside kvm_v2_exception_free_per_vcpu.
 * Safe on a never-installed vCPU (NULL gadget_state_kva -> no-op).
 */
void kvm_v2_exception_free_per_vcpu_gadget_state(struct kvm_v2_vm *vm,
						 struct kvm_v2_vcpu *vcpu,
						 int cpu);

/*
 * Load guest CR3 for @vcpu from __pa(pgd).
 */
int  kvm_v2_load_cr3(struct kvm_v2_vcpu *vcpu, unsigned long pgd);

/*
 * KVM_RUN dispatcher. Picks the per-host-CPU vCPU, loads CR3, FS/GS
 * bases, and GPRs from @regs, issues KVM_RUN, marshals exit state back,
 * and dispatches by exit_reason.
 *
 * The helper pins the current task with migrate_disable() while it owns
 * a per-host-CPU vCPU. If the pool is not initialised it falls back to
 * seccomp_vcpu_run() after re-enabling migration.
 *
 * Unsupported exit reasons terminate the guest; supported reasons are
 * handled by the syscall and exception helpers below.
 */
void kvm_v2_vcpu_run(struct uml_pt_regs *regs);

/*
 * Marshal helper in vcpu.c. Copy uml_pt_regs.gp[] into a struct kvm_regs
 * (the on-mmap kvm_run->s.regs.regs target). Used before KVM_RUN and by
 * exception handlers that must write post-signal GPR state back into KVM.
 *
 * The syscall path deliberately does not call this helper after
 * handle_syscall(): a blocking syscall can schedule away and another UML
 * task can reuse the shared per-host-CPU kvm_run mmap before the sleeping
 * syscall resumes. Syscall return state stays in per-task regs until the
 * next outer vcpu_run iteration marshals it into the selected vCPU.
 * RFLAGS.bit1 is reserved-must-be-1 and is set during marshal.
 */
void kvm_v2_marshal_to_kvm_regs(struct kvm_regs *dst,
				const struct uml_pt_regs *src);

/*
 * Reverse marshal: struct kvm_regs -> uml_pt_regs.gp[]. Called after
 * KVM_RUN returns so UML's syscall / fault / signal dispatch sees
 * the guest's post-exit GPRs. HOST_ORIG_AX is intentionally NOT
 * written here -- that's an UML entry-path convention the syscall
 * dispatcher arranges once it knows the bucket. Declared here so
 * the marshal-shape KUnit suite can call it directly; the live caller
 * is kvm_v2_vcpu_run's marshal-out after KVM_RUN returns.
 */
void kvm_v2_marshal_from_kvm_regs(struct uml_pt_regs *dst,
				  const struct kvm_regs *src);

/*
 * Symmetric read-back of sregs.fs.base / gs.base into
 * gp[HOST_FS_BASE/GS_BASE]. Called after every KVM_RUN exit
 * (including EINTR) alongside kvm_v2_marshal_from_kvm_regs. This keeps
 * arch_prctl-updated FS/GS bases round-tripping through the per-task UML
 * register state across task switches.
 */
void kvm_v2_marshal_sregs_back(struct uml_pt_regs *dst,
			       const struct kvm_sregs *src);

/*
 * Per-task IST frame restore. Called from kvm_v2_vcpu_run() just before
 * KVM_RUN to rewrite the per-vCPU IST stack from the current task's
 * snapshot. This prevents one UML task from consuming another task's
 * saved exception frame when multiple tasks share one host-CPU vCPU.
 * No-op when current->thread.arch.kvm_v2.ist_pending is false.
 *
 * Defined in syscall_trap.c next to ist_frame_read/write.
 */
void kvm_v2_ist_frame_restore_pending(struct kvm_v2_vcpu *vcpu);

/*
 * Snapshot the raw IDT-pushed exception frame from the IST stack into
 * current's per-task ist_frame[] storage as-is. Used when EINTR catches
 * the vCPU after hardware IDT delivery but before the in-guest handler
 * stub executes. Without this snapshot, another task on the same
 * per-host-CPU vCPU could overwrite the shared IST stack before this
 * task resumes.
 *
 * Sets ist_pending=true so the next dispatch's
 * kvm_v2_ist_frame_restore_pending() reinstates the IST-stack
 * contents.
 */
void kvm_v2_ist_frame_snapshot_raw(struct kvm_v2_vcpu *vcpu);

/*
 * Cross-vCPU guest-TLB kick. Called from arch/um/kernel/tlb.c after a
 * successful drain. Iterates online CPUs excluding self and sends
 * IPI_SIGNAL so each remote KVM_RUN exits with -EINTR; the next
 * dispatch's CR4.PGE toggle flushes the local guest TLB. SMP-only.
 */
void kvm_v2_tlb_kick_others(struct mm_struct *mm);

/*
 * Handle a #PF inline during the EINTR path when the vCPU exited after
 * IDT delivery but before the handler stub executed. The handler reads
 * the fresh IDT frame from this vCPU's IST stack, dispatches the UML page
 * fault path, snapshots any post-handler frame state, and resumes at the
 * user RIP directly. This avoids resuming a stub whose saved stack pointer
 * may refer to a different vCPU's IST page after migration.
 *
 * Returns 0 always; signature mirrors kvm_v2_handle_io_pf.
 */
int kvm_v2_handle_pf_eintr_inline(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu,
				  u64 cr2);

/*
 * Inline #NM handler for the EINTR-mid-NM-stub case. The implementation
 * documents the frame repair sequence.
 */
int kvm_v2_handle_nm_eintr_inline(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu);

/*
 * Per-task FPU capture on context-switch-out and the .context_switch op
 * wrapper that invokes it before delegating to seccomp_context_switch.
 * Defined in vcpu.c so capture and install logic stay together; declared
 * here for ops.c.
 *
 * kvm_v2_fpu_capture_for_switch_out snapshots the outgoing task's
 * per-CPU vCPU FPU state into from->thread.arch.kvm_v2.fpu so the
 * task's next KVM dispatch installs the snapshot via
 * kvm_v2_fpu_install_on_first_run. Failure does not abort the switch:
 * the task migrates without an FPU snapshot and the destination vCPU
 * falls back to architectural reset values.
 *
 * kvm_v2_context_switch is the .context_switch op pointer; it calls
 * the FPU capture helper then delegates to seccomp_context_switch
 * (which does the actual switch_threads jmp_buf swap).
 */
void kvm_v2_fpu_capture_for_switch_out(struct task_struct *from);
void kvm_v2_context_switch(struct task_struct *from, struct task_struct *to);

/*
 * Memslot record management, defined in memslot.c. The helpers take
 * @vm->lock internally; callers must not hold it. KVM ioctls are issued
 * by callers after kvm_v2_memslot_add() returns a slot id.
 */
int  kvm_v2_memslot_add(struct kvm_v2_vm *vm, u64 gpa, u64 host_va,
			u64 size, u32 flags);
void kvm_v2_memslot_del(struct kvm_v2_vm *vm, u32 slot_id);

/*
 * mm_region_added/mm_region_removed ops in region.c. The giant physmem
 * memslot installed by context.c covers guest page-table walks, so the
 * per-region hooks serialize the mm update and leave KVM memslots
 * unchanged.
 */
int  kvm_v2_mm_region_added(struct mm_struct *mm,
			    const struct um_memory_region *region);
int  kvm_v2_mm_region_removed(struct mm_struct *mm,
			      const struct um_memory_region *region);

#endif /* __ARCH_UM_BACKEND_KVM_V2_H */
