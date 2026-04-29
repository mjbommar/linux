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
