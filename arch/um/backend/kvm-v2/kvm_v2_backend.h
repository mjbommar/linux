/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal header for arch/um/backend/kvm-v2/.
 *
 * Holds the v2-private declarations shared between init.c and ops.c
 * (and, in later phases, context.c / vcpu.c / memslot.c per memo 26
 * §A.2+). The ops table struct itself lives in <backend.h>; this
 * header is just the v2-only glue.
 */
#ifndef __ARCH_UM_BACKEND_KVM_V2_H
#define __ARCH_UM_BACKEND_KVM_V2_H

#include <linux/bitmap.h>
#include <linux/bitops.h>
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
 * our copy to SHRT_MAX so the bitmap matches what KVM will actually
 * accept from KVM_SET_USER_MEMORY_REGION; if a future host bumps the
 * limit we just leave headroom unused. Memo 26 §B.1 phrases this as
 * "typically 32768" — SHRT_MAX is 32767, which is the precise value.
 */
#define KVM_V2_MAX_USER_MEM_SLOTS	32767

/*
 * Per-UML-kernel-invocation VM context (memo 26 §A.2). Single instance
 * lives in context.c; init.c reaches it via kvm_v2_vm_get() once
 * kvm_v2_vm_create() has succeeded. memslots is populated by Phase B
 * (KVM_SET_USER_MEMORY_REGION); cpuid is the curated CPUID2 buffer
 * that A.3 builds + installs on its placeholder vCPU (deferred out of
 * A.2 because the buddy allocator isn't up at init_backend time —
 * see context.c file-scope comment).
 */
struct kvm_v2_vm {
	int			kvm_fd;
	int			vm_fd;
	u64			caps;
	struct kvm_cpuid2	*cpuid;
	struct list_head	memslots;
	/*
	 * Per-VM bitmap of allocated memslot ids (Phase B.1). One bit per
	 * KVM user memslot; cleared on free. The bitmap and the memslots
	 * list are both protected by `lock`. Phase D's per-mm-worker model
	 * may move both fields onto the worker context — for now per-VM is
	 * the natural scope because there is one VM per UML invocation
	 * (decisions-log D57 / context.c rationale).
	 */
	unsigned long		memslot_bitmap[BITS_TO_LONGS(KVM_V2_MAX_USER_MEM_SLOTS)];
	spinlock_t		lock;
	/*
	 * Phase D.1: LSTAR trampoline page (memo 26 §D.1, syscall_trap.c).
	 * `trampoline_page` is the host kernel VA of a single page from
	 * the buddy allocator; `trampoline_gpa` is __pa(trampoline_page),
	 * which is the GPA the guest's PML4[508] entry (installed in D.4)
	 * walks down to. The 5 LSTAR bytes (out + sysretq) live at
	 * trampoline_page + KVM_V2_TRAMPOLINE_LSTAR_OFFSET (= 0x40); D.4
	 * programs MSR_LSTAR to KVM_V2_LSTAR_GVA so guest SYSCALL lands
	 * here. NULL until kvm_v2_trampoline_alloc_and_install succeeds —
	 * vm_create attempts the install but the buddy allocator may not
	 * be up at init_backend time (memo 26 §D.0a's lesson), so a lazy
	 * retry path may be required before D.4 can program LSTAR. v1
	 * mirror: kvm-v1-archive/thread.c:602-605 (kvm_bootstrap_page +
	 * kvm_bootstrap_gpa file-scope statics).
	 */
	void			*trampoline_page;
	phys_addr_t		trampoline_gpa;
	/*
	 * Phase D.4b-pre: physmem identity-offset memslot id (memo 26
	 * §D.4 D.4b-pre). One giant slot at gpa=0 / hva=uml_physmem /
	 * size=physmem_size, mirroring v1's kvm_ensure_memslot at
	 * kvm-v1-archive/lifecycle.c:613-648. Without this slot KVM TDP
	 * has no memslot covering the pgd/PT-chain/trampoline GPAs (all
	 * of which are __pa(kva) = kva - uml_physmem, in
	 * [0, physmem_size)) — the per-region memslots from Phase B.2 sit
	 * at host-VA-valued GPAs (in user-half VA space, far outside
	 * [0, physmem_size)) and don't cover the physmem range. Sentinel
	 * -1 means "not installed yet" (vm_create's eager attempt may
	 * have deferred because uml_physmem / physmem_size aren't set
	 * until later in linux_main; lazy retry from D.1's
	 * trampoline_late_install initcall picks it up). Non-negative
	 * value is the kvm_v2_memslot_add-allocated slot id; the existing
	 * memslot list teardown in vm_destroy frees it.
	 */
	int			physmem_memslot_id;
	/*
	 * Phase D.4b: PT chain for the kernel-half PML4[448] install
	 * (memo 26 §D.4 D.4b). The trampoline at GVA KVM_V2_LSTAR_GVA
	 * (0xffffe00000000040) requires a guest PUD/PMD/PTE walk from
	 * PML4[448] down to the trampoline page. These pages are
	 * VM-lifetime; freed in vm_destroy after all mms have been torn
	 * down. Living in physmem so they sit inside D.4b-pre's physmem
	 * identity-offset memslot — alloc_page → page_address →
	 * __pa(kva) is in [0, physmem_size), which the physmem memslot
	 * translates back to the original kva via userspace_addr =
	 * uml_physmem + offset. Mirrors v1's kvm_shadow_map_page pattern
	 * at kvm-v1-archive/thread.c:2287-2365 but writes REAL page-table
	 * entries — TDP walks them natively, no shadow-PT machinery.
	 *
	 * Storing only the PUD GPA in the struct is enough; the PMD/PTE
	 * pages are kept by their KVAs for free-time. The PUD GPA is
	 * what gets written to swapper_pg_dir[448] and init_mm.pgd[448]
	 * to seed kernel-half propagation through pgd_alloc's memcpy at
	 * arch/um/kernel/mem.c:149-157. NULL-valued KVAs mean "not
	 * installed yet" (D.4b-pre + D.1 must complete first; lazy retry
	 * from D.1's trampoline_late_install initcall lands the
	 * kernel-half install once the trampoline GPA is known).
	 */
	void			*trampoline_pud_kva;	/* PUD page; entry [0] = pmd_pa */
	void			*trampoline_pmd_kva;	/* PMD page; entry [0] = pte_pa */
	void			*trampoline_pte_kva;	/* PTE page; entry [0] = trampoline_gpa */
	phys_addr_t		trampoline_pud_gpa;	/* __pa(trampoline_pud_kva) — written to PML4[448] */
};

/*
 * In-memory memslot record (Phase B.1). One per registered region;
 * lives on `vm->memslots` until kvm_v2_memslot_del() removes it.
 *
 * Identity-map invariant: B.2's KVM_SET_USER_MEMORY_REGION will pass
 * userspace_addr == guest_phys_addr (host VA == GPA), so host_va and
 * gpa are stored separately to leave room for v2 to drop the identity
 * map later without changing the in-memory shape.
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
 * Per memo 26 §A.1: HOT ops (vcpu_run, mm_region_added, mm_region_removed,
 * context_switch, read_clock_ns) cannot be NULL — validate_hot_ops()
 * panics — and the dispatch macro does not synthesize -ENOSYS for cold
 * ops either. Phase A.1 ships an ops table whose every entry delegates
 * to the seccomp backend; A.2+ replaces ops one at a time as the
 * per-VM context, vCPU pool, memslot allocator, etc. land. In the
 * meantime selecting `backend=force=kvm-v2` is observably equivalent
 * to seccomp at the dispatch layer, with the addition of the v2 init
 * tracepoint and the registered ops table that later phases will mutate.
 */
extern const struct um_backend_ops um_backend_kvm_v2_ops;

/* v2 lifecycle ops — defined in init.c. */
int  kvm_v2_probe(void);
int  kvm_v2_init(const struct um_backend_args *args);
void kvm_v2_shutdown(void);

/* Per-VM context lifecycle — defined in context.c (memo 26 §A.2). */
int  kvm_v2_vm_create(int kvm_fd, u64 caps);
void kvm_v2_vm_destroy(void);
struct kvm_v2_vm *kvm_v2_vm_get(void);

/*
 * Phase D.4b-pre: physmem identity-offset memslot install (memo 26 §D.4).
 * Idempotent — re-invocation after a successful install short-circuits.
 * Returns 0 on success / already-installed, -EAGAIN if uml_physmem /
 * physmem_size aren't set yet (caller treats like trampoline alloc's
 * pre-buddy -ENOMEM: log + defer to the lazy retry path), or any other
 * negative errno from KVM_SET_USER_MEMORY_REGION on a hard failure.
 *
 * vm_create's eager attempt may hit the -EAGAIN branch because
 * init_backend() runs before linux_main() finishes populating
 * uml_physmem / physmem_size (see arch/um/kernel/um_arch.c:372 — the
 * init_backend call — vs lines 392/399 where the globals are set).
 * The subsys_initcall lazy retry in syscall_trap.c picks the install up
 * once the globals are stable.
 *
 * Defined in context.c.
 */
int  kvm_v2_physmem_memslot_install(struct kvm_v2_vm *vm);

/*
 * Phase D.4b: install the per-VM kernel-half PT chain at PML4[448] so
 * the LSTAR trampoline GVA (KVM_V2_LSTAR_GVA = 0xffffe00000000040) is
 * reachable from any guest CR3 once D.5 flips .vcpu_run.
 *
 * Allocates 3 pages (PUD/PMD/PTE) from buddy, writes the chain with
 * _KERNPG_TABLE non-leaf flags + (_PAGE_PRESENT | _PAGE_ACCESSED) leaf
 * flags (RO + kernel-only), seeds swapper_pg_dir[448] AND
 * init_mm.pgd[448] with pud_pa | _KERNPG_TABLE so UML's existing
 * kernel-half copy in pgd_alloc (arch/um/kernel/mem.c:149-157)
 * propagates the entry into every future mm. Idempotent — re-invocation
 * after a successful install short-circuits via the trampoline_pud_kva
 * sentinel.
 *
 * Prerequisites: vm->trampoline_gpa must be set (D.1's trampoline alloc
 * must have completed), and the physmem memslot must cover
 * [0, physmem_size) (D.4b-pre).
 *
 * Returns 0 on success / already-installed; -EINVAL on prerequisites
 * unmet; -ENOMEM if alloc_page returns NULL (caller treats like the
 * trampoline alloc's pre-buddy -ENOMEM: log + defer to the lazy retry
 * path); other negative errno on hard failure.
 *
 * Defined in syscall_trap.c (alongside the trampoline install — both
 * sides of the LSTAR install live in the same TU because the PT
 * chain's leaf entry references the trampoline GPA).
 */
int  kvm_v2_kernel_half_install(struct kvm_v2_vm *vm);

/*
 * Phase D.4b symmetric teardown — free the PUD/PMD/PTE pages and clear
 * swapper_pg_dir[448] + init_mm.pgd[448] so any concurrent mm operation
 * sees zero rather than dangling. Called from kvm_v2_vm_destroy. Safe
 * on a never-installed VM (NULL pud_kva → no-op).
 */
void kvm_v2_kernel_half_free(struct kvm_v2_vm *vm);

/*
 * Per-host-CPU vCPU pool member (memo 26 §C.1). One element per
 * possible host CPU (`nr_cpu_ids`); the array itself is a static
 * NR_CPUS-sized slab so allocation lands cleanly during init_backend()
 * before the buddy allocator is up (vcpu.c documents the constraint).
 *
 * `cpu` records the host CPU index this vCPU is pinned to so Phase
 * C.2's task→vCPU dispatch can pick the right entry. Pthread + state
 * machinery is C.2 territory — at C.1 each vCPU is just a fd + the
 * mmap'd kvm_run; nothing runs them yet (ops.c still delegates
 * .vcpu_run to the seccomp backend).
 *
 * `cpuid_primed` (memo 26 §D.0a): sticky one-shot guard for the lazy
 * KVM_SET_CPUID2 install. The eager install at vcpu_create_one ran
 * during init_backend() before the buddy allocator is up — kzalloc
 * returned NULL and the curated mask never landed (boot log:
 * "cpuid kzalloc(10248) failed"). D.0a moves the install to first
 * KVM_RUN and uses this flag as the per-vCPU "already installed"
 * predicate. v1 archive deferred CPUID identically (kvm_ensure_
 * cpuid_done at kvm-v1-archive/lifecycle.c:411-547).
 */
struct kvm_v2_vcpu {
	int   vcpu_fd;
	void *kvm_run;
	u32   kvm_run_size;
	int   cpu;
	bool  cpuid_primed;
};

int  kvm_v2_vcpu_create(struct kvm_v2_vm *vm);
void kvm_v2_vcpu_destroy(void);

/*
 * Per-CPU accessor for Phase C.2's dispatcher. Returns the vCPU pinned
 * to host CPU `cpu` or NULL if `cpu` is out of range / the pool isn't
 * initialised. Phase C.1 exports the accessor so C.2's vcpu_run helper
 * lands without further header churn; today it has no callers.
 */
struct kvm_v2_vcpu *kvm_v2_vcpu_get(int cpu);

/*
 * Phase B.5: load guest CR3. Caller passes the target vCPU + __pa(pgd).
 * The vcpu argument is plumbed for Phase C.2's per-CPU dispatch — C.1
 * generalises the helper from the A.3 single-vcpu static so the
 * dispatcher can pick the right pool entry. No production caller yet
 * (ops.c still delegates .vcpu_run); the SREGS read/write plumbing
 * lands here for review.
 */
int  kvm_v2_load_cr3(struct kvm_v2_vcpu *vcpu, unsigned long pgd);

/*
 * Phase C.2: KVM_RUN dispatcher. Picks the per-host-CPU vCPU,
 * loads CR3 + fs.base + gs.base + GPRs from `regs`, issues KVM_RUN,
 * marshals the exit GPRs back, and dispatches by exit_reason. Phase
 * C.2 lands the helper UNREFERENCED — ops.c still routes
 * `.vcpu_run` through seccomp_vcpu_run; Phase D flips the pointer.
 *
 * The helper expects to run on a kernel stack (it calls
 * preempt_disable / preempt_enable around the per-CPU vCPU pick).
 * If the pool isn't initialised it falls back to seccomp_vcpu_run
 * so callers tolerating the v2 path before init_backend completes
 * still make progress.
 *
 * Exit-reason coverage at C.2 is intentionally minimal — only the
 * fatal classes (HLT / FAIL_ENTRY / INTERNAL_ERROR / SHUTDOWN) and
 * a default panic. HYPERCALL handling is Phase D; IO / MMIO /
 * exception classes land in Phase E.
 */
void kvm_v2_vcpu_run(struct uml_pt_regs *regs);

/*
 * Phase C.3 marshal helper (vcpu.c). Copy uml_pt_regs.gp[] into a
 * struct kvm_regs (the on-mmap kvm_run->s.regs.regs target). Used by
 * vcpu.c's pre-KVM_RUN marshal-in AND by syscall_trap.c's D.3
 * marshal-out (after handle_syscall returns, regs->gp[HOST_AX] holds
 * the syscall return value; the marshal copies it back into the mmap
 * so the next KVM_RUN delivers it to user via the trampoline's
 * sysretq). RFLAGS.bit1 (reserved-must-be-1) is OR'd in defensively.
 */
void kvm_v2_marshal_to_kvm_regs(struct kvm_regs *dst,
				const struct uml_pt_regs *src);

/*
 * Phase D.3: per-task FPU capture on context-switch-out + the
 * .context_switch op wrapper that invokes it before delegating to
 * seccomp_context_switch. Defined in vcpu.c (alongside the C.4
 * fork-time capture so both sides of the snapshot pair live in the
 * same TU); declared here so ops.c can reference the wrapper without
 * a duplicate prototype.
 *
 * kvm_v2_fpu_capture_for_switch_out snapshots the outgoing task's
 * per-CPU vCPU FPU state into from->thread.arch.kvm_v2.fpu so the
 * task's next first-run installs the snapshot via
 * kvm_v2_fpu_install_on_first_run. Failure is non-fatal — the task
 * migrates without an FPU snapshot and the destination first-run
 * falls back to architectural reset values.
 *
 * kvm_v2_context_switch is the .context_switch op pointer; it calls
 * the FPU capture helper then delegates to seccomp_context_switch
 * (which does the actual switch_threads jmp_buf swap).
 */
void kvm_v2_fpu_capture_for_switch_out(struct task_struct *from);
void kvm_v2_context_switch(struct task_struct *from, struct task_struct *to);

/*
 * Memslot allocator + lookup (memo 26 §B.1, defined in memslot.c).
 *
 * Phase B.1 only manages the in-memory list and the slot-id bitmap;
 * KVM_SET_USER_MEMORY_REGION is wired in B.2. The functions below all
 * take `vm->lock` internally — callers must NOT hold it.
 */
int  kvm_v2_memslot_alloc_id(struct kvm_v2_vm *vm);
void kvm_v2_memslot_free_id(struct kvm_v2_vm *vm, u32 slot_id);
int  kvm_v2_memslot_add(struct kvm_v2_vm *vm, u64 gpa, u64 host_va,
			u64 size, u32 flags);
void kvm_v2_memslot_del(struct kvm_v2_vm *vm, u32 slot_id);
struct kvm_v2_memslot *kvm_v2_memslot_lookup(struct kvm_v2_vm *vm, u64 gpa);

/*
 * Phase B.2 + B.3 (region.c): mm_region_added / mm_region_removed
 * ops — issue KVM_SET_USER_MEMORY_REGION add / delete for each VA
 * range the mm-arbiter surfaces. Replace the corresponding seccomp_*
 * pointers in the v2 ops table; both internally also call the
 * seccomp_* version (dual-side wiring) until Phase D's KVM_RUN path
 * stops needing the stub child.
 */
int  kvm_v2_mm_region_added(struct mm_struct *mm,
			    const struct um_memory_region *region);
int  kvm_v2_mm_region_removed(struct mm_struct *mm,
			      const struct um_memory_region *region);

#endif /* __ARCH_UM_BACKEND_KVM_V2_H */
