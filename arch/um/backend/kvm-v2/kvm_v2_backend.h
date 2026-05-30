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
#include <linux/jump_label.h>
#include <linux/kvm.h>
#include <linux/list.h>
#include <linux/mutex.h>
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
 * SMP-T56 / Round 4 narrowed fix (2026-05-17): per-dispatch lag
 * threshold at which kvm_v2_load_user_sregs forces a full
 * KVM_SET_SREGS ioctl (in addition to the SMP-T33 cross_task gate)
 * to drop KVM's per-vCPU prev_roots[] TDP cache. Empirically lag>=3
 * is the existing pr_emerg KVM_V2_TLB_LAG diagnostic's threshold;
 * the Round 4 unconditional-variant evidence pairs failure
 * occurrence with lag in the hundreds-to-thousands range, so 3 is
 * a deliberately conservative starting point — below the noise
 * floor of normal cross-vCPU workloads but well below the failing
 * dispatch lags. Tunable in a future round if needed.
 */
#define KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD	3

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
	/*
	 * Phase E.1: IDT + handler stubs + GDT pages (memo 26 §E.1,
	 * codex --search audit finding #1 split the v1 single-page
	 * layout into three because a 256-entry IDT fills an entire
	 * 4KB page on its own).
	 *
	 * Layout (all under PML4[448]/PUD[0]/PMD[0], at PTE[1..3]):
	 *   idt_*       — 256 × 16-byte gate descriptors. Vector 14
	 *                 (#PF), 13 (#GP), 6 (#UD), 0 (#DE), 4 (#OF)
	 *                 point at handler stubs in handlers_kva; vector
	 *                 3 (#BP) gates with DPL=3 (codex audit #3) but
	 *                 the in-guest dispatch path is via
	 *                 KVM_GUESTDBG_USE_SW_BP per memo 26 §E.3.
	 *                 Other vectors point at a panic stub (port
	 *                 UM_KVM_TRAP_PANIC = 0xf8).
	 *   handlers_*  — N × 16-byte slots, each a 4-byte
	 *                 `out %al, $port ; iretq` stub. NO `mov %cr2,
	 *                 %rax` (codex audit #4 — host reads CR2 via
	 *                 sync-regs sregs.cr2; v1 archive at thread.c:
	 *                 1297 explicitly removed the `mov` for the same
	 *                 reason: it clobbers user RAX before the host
	 *                 captures vCPU state).
	 *   gdt_*       — 8 × 8-byte entries (slot [0] null, [1] kern
	 *                 CS, [2] kern DS, [3] STAR-base padding, [4]
	 *                 user DS @0x23 DPL=3, [5] user CS @0x2b DPL=3,
	 *                 [6..7] TSS desc — body filled in E.2). v1
	 *                 reference: kvm-v1-archive/sregs.c:102-138
	 *                 (kvm_setup_harness_gdt verbatim) +
	 *                 kvm-v1-archive/thread.c:1574-1590 (TSS desc
	 *                 in slots 6-7).
	 *
	 * VM-lifetime; freed in vm_destroy via kvm_v2_exception_free
	 * BEFORE kvm_v2_kernel_half_free (so PTE[1..3] dereferences
	 * stay valid until the PT chain itself drops).
	 */
	void			*idt_kva;
	phys_addr_t		idt_gpa;
	void			*handlers_kva;
	phys_addr_t		handlers_gpa;
	void			*gdt_kva;
	phys_addr_t		gdt_gpa;
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
 * APERF/MPERF MSR passthrough toggle — defined in aperfmperf.c when
 * CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y; absent (and the
 * one call site #ifdef'd out) otherwise.  See aperfmperf.c file-scope
 * comment + Documentation/virt/uml/aperf-mperf.rst.
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
	/*
	 * Phase E.2 (memo 26 §E.2): per-vCPU IST stack + TSS for
	 * exception delivery. IST1 is loaded on every #PF/#GP/#UD/
	 * #DE/#BP/#OF gate per E.1's IDT entries (which set IST=1
	 * unconditionally — see exception.c:kvm_v2_idt_set_gate
	 * call sites). Without these the .vcpu_run flip in E.3.5
	 * would deliver the first guest exception to a TSS whose
	 * IST1 RSP is zero — which is non-canonical and causes
	 * #GP-during-delivery → #DF → triple fault.
	 *
	 * Per-vCPU is REQUIRED (codex audit independent finding):
	 * the TSS holds the IST1 RSP pointer, and only one TSS can
	 * be active per vCPU at a time (via TR). Two vCPUs sharing
	 * a TSS would either point both IST1s at the same physical
	 * stack (collision risk if both took exceptions simultaneously)
	 * or have to dynamically rewrite the TSS's IST1 on every
	 * dispatch (race-prone). Per-vCPU TSS is the clean answer.
	 *
	 * Allocated from buddy via __get_free_page (one IST page +
	 * one TSS page per pool member). Pages live in physmem so
	 * their __pa() resolves through D.4b-pre's physmem identity-
	 * offset memslot. VM-lifetime — freed in vm_destroy via
	 * kvm_v2_exception_free_per_vcpu BEFORE kvm_v2_kernel_half_free
	 * (the per-vCPU PTE writes go through trampoline_pte_kva which
	 * the chain owns — once the chain frees, those PTE writes
	 * dereference released memory).
	 *
	 * v1 archive mirror: kvm-v1-archive/thread.c:1574-1590 (TSS
	 * slot setup) + kvm-v1-archive/sregs.c (TR programming).
	 */
	void	    *ist_stack_kva;
	phys_addr_t  ist_stack_gpa;
	u64	     ist_stack_top_gva;
	void	    *tss_kva;
	phys_addr_t  tss_gpa;
	u64	     tss_gva;

	/*
	 * Phase G.2-cont (2026-05-01): cross-vCPU TLB-flush kick dedup.
	 * cmpxchg'd 0→1 before IPI; reset 0 by the kicked vCPU at
	 * load_user_sregs. At most one IPI in flight per vCPU.
	 */
	atomic_t kick_pending;

	/*
	 * Phase G.2-fix (2026-05-01): v1-archive tlb_gen pattern.
	 *
	 * last_seen_tlb_gen tracks the highest mm->context.tlb_gen
	 * this vCPU has flushed against. Updated in load_user_sregs
	 * after the CR4.PGE toggle. The kicker compares vs this to
	 * skip vCPUs already up-to-date.
	 *
	 * current_mm is set in load_user_sregs to the mm being
	 * dispatched on this vCPU. The kicker uses it to NARROW IPIs
	 * to only vCPUs running THIS mm — every other vCPU isn't
	 * affected by this mm's PTE changes.
	 *
	 * Set as plain pointer (not RCU); reads are advisory in the
	 * kicker (a stale read just means we send an unneeded IPI,
	 * which is bounded by kick_pending dedup).
	 */
	atomic64_t last_seen_tlb_gen;
	struct mm_struct *current_mm;

	/*
	 * SMP-T56 / Round 4 narrowed fix (2026-05-17): captured tlb_gen
	 * lag at the start of each dispatch — `mm->context.tlb_gen -
	 * vcpu->last_seen_tlb_gen` BEFORE last_seen is updated. The
	 * cross_task gate at the end of load_user_sregs consumes this:
	 * when lag >= KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD, KVM's
	 * per-vCPU prev_roots[] cache for this mm's CR3 is treated as
	 * highly likely stale (other vCPUs have bumped tlb_gen multiple
	 * times since we last ran this mm) and we issue the heavy
	 * KVM_SET_SREGS ioctl that drops prev_roots[] via
	 * __set_sregs2 → kvm_mmu_reset_context. Plain field — only ever
	 * read/written under this vCPU's dispatch thread.
	 */
	u64 last_dispatch_tlb_lag;

	/*
	 * SMP-T16 fix (2026-05-02): Bug A — preserve hardware-architectural
	 * CR2 across same-task re-entries. The previous unconditional
	 * sregs->cr2 = 0 in load_user_sregs zeroed CR2 on every dispatch.
	 * Combined with KVM_SYNC_X86_SREGS dirty (forced by CR4.PGE toggle),
	 * this writes vcpu->arch.cr2 = 0 and svm->vmcb->save.cr2 = 0 on
	 * every VMRUN. A SIGALRM-EINTR caught after hardware queued a #PF
	 * for delivery but before the in-stub `mov %cr2, %rax` could capture
	 * it loses the fault address. The next dispatch's load_user_sregs
	 * zeroed cr2 → re-entry with cr2=0 → user-mode re-fault delivered
	 * with hardware-set cr2=fault_va, BUT any internal NPF between
	 * IDT delivery and the stub's read clobbers cr2 from the
	 * just-set-zero vcpu->arch.cr2.
	 *
	 * Track the previous task that ran on this vCPU. Only zero cr2
	 * on cross-task transitions (necessary for cross-task isolation —
	 * a child task shouldn't see parent's residual cr2). Same-task
	 * re-entry leaves cr2 alone, so KVM's mmap-cached real fault
	 * address survives.
	 *
	 * SMP-T23 (2026-05-02): also track last_mm. The original T16
	 * gate (`last_task != current`) is silent across execve — `current`
	 * stays the same while `current->mm` is replaced — so the first
	 * dispatch of a freshly exec'd mm preserves the prior mm's stale
	 * sregs.cr2. Surface as residual `python3[N]: segfault at 0 ip
	 * <legit user ip> error 0` (cr2 set to a stale prior fault address
	 * that the new dispatch's stub then captures). Treat cross-mm
	 * the same as cross-task: zero cr2.
	 */
	struct task_struct *last_task;
	struct mm_struct   *last_mm;

	/*
	 * SMP-T55 fix (2026-05-07): per-vCPU FPU-dirty epoch flag.
	 *
	 * Predicates the post-vmexit `KVM_GET_FPU` so we can skip it when
	 * the vCPU's guest FPU is bit-identical to the per-task
	 * `iotrap_fpu` snapshot. Restores Phase H.2's lazy-FPU win that
	 * SMP-T26/T27 (commit `76b1d98b2006`) reverted to plug the
	 * cross-task XMM leak — but using the correct semantic
	 * (per-vCPU dirty epoch, not per-dispatch CR0.TS).
	 *
	 * `fpu_dirty=true` => vCPU's guest_fpu may have diverged from
	 * `fpu_owner_task`'s `iotrap_fpu`. Set on:
	 *   - vcpu_create_one (initial — force the first GET).
	 *   - cross-task arrival (`fpu_owner_task != current` in
	 *     load_user_sregs) — a different task may have run.
	 *   - post-vmexit `sregs.cr0 & X86_CR0_TS == 0` — guest used FPU.
	 *   - kvm_v2_handle_io_nm clearing CR0.TS (next dispatch will
	 *     execute the user FPU instruction the handler is unblocking).
	 *   - kvm_v2_fpu_install_on_first_run after a fresh KVM_SET_FPU
	 *     from `fpu_valid` (per memo state-audit/23 §5 gotcha #2 —
	 *     re-capture the installed snapshot back into iotrap_fpu so
	 *     the per-task slot stays authoritative).
	 *
	 * `fpu_owner_task` records which task `fpu_dirty=false` is
	 * relative to. Belt-and-suspenders against any path we missed
	 * marking dirty: skip the GET only when both `fpu_dirty=false`
	 * AND `fpu_owner_task == current`. Updated after every successful
	 * `KVM_SET_FPU` (pre-run install of iotrap_fpu) and `KVM_GET_FPU`
	 * (post-vmexit capture).
	 *
	 * Memo: state-audit/23-smp-t55-perf-regression-plan.md option (a).
	 */
	bool		    fpu_dirty;
	struct task_struct *fpu_owner_task;

	/*
	 * Phase H gadget per-vCPU state page (2026-05-04, Phase 2).
	 *
	 * 4KB page mapped at KVM_V2_GADGET_STATE_GVA(cpu) via
	 * PTE[KVM_V2_GADGET_BASE_SLOT + cpu] of the trampoline_pte_kva
	 * chain. MSR_KERNEL_GS_BASE for this vCPU is programmed to that
	 * GVA so the LSTAR getpid gadget's `swapgs ; mov %gs:OFFSET, %eax`
	 * reads from this vCPU's page exclusively. load_user_sregs
	 * refreshes task_tgid_vnr(current) at offset KVM_V2_GADGET_OFF_TGID
	 * before each KVM_RUN.
	 *
	 * Why per-vCPU (vs per-VM Phase-1 design): different tasks running
	 * on different vCPUs simultaneously would race on a single shared
	 * state page. Per-vCPU isolates writes to the host CPU pthread
	 * that owns this vCPU; the gadget runs on the same vCPU (1:1
	 * binding), so writer and reader are exclusive in time without
	 * a lock.
	 *
	 * VM-lifetime; freed in kvm_v2_exception_free_per_vcpu_gadget_state
	 * BEFORE kvm_v2_kernel_half_free (the PTE write goes through
	 * trampoline_pte_kva which the kernel-half chain owns).
	 *
	 * v1 archive mirror: kvm-v1-archive/lifecycle.c:846-883
	 * (kvm_gadget_state_alloc / _free / _refresh) — v1 had a single
	 * shared page (ncpus=1 only); v2 Phase 2 makes it per-vCPU so
	 * ncpus>1 works.
	 */
	void	    *gadget_state_kva;
	phys_addr_t  gadget_state_gpa;
	u64	     gadget_state_gva;

	/*
	 * Round 2 Django investigation (2026-05-17): same-task consecutive
	 * EINTR counter. The Django-loopback flake's captured pre-corruption
	 * window (memo §44 Round 2) is a task stuck in a KVM_RUN → EINTR
	 * loop for tens of dispatches without any vmexit (KVM_EXIT_IO /
	 * KVM_EXIT_INTR alternation that NEVER reaches port=0xf4/0xfd). The
	 * in-guest CPU has CR2 pinned at a user-half address but no IDT
	 * delivery fires — the #PF handler stub VA is presumably unreachable
	 * (TLB/TDP stale for kernel-half trampoline mapping, or IST page
	 * unmapped). The task is alive but making zero forward progress.
	 *
	 * eintr_run_task and eintr_run_count track the (task, consecutive)
	 * pair on this vCPU. Reset whenever a different task takes over,
	 * or whenever this task gets a non-EINTR KVM_RUN return
	 * (KVM_EXIT_IO etc.) — i.e., made any progress. When count crosses
	 * a configurable threshold (kvm_v2_eintr_loop_threshold), the
	 * state-trace ring is auto-frozen and a one-shot pr_emerg fires
	 * with the stuck task's pid/RIP/CR2/CR3 so post-mortem grep can
	 * pin down the moment forward-progress stopped.
	 *
	 * Threshold default: 16 (one SIGALRM tick ≈ 10ms; 16 consecutive
	 * EINTRs ≈ 160ms of no-progress stall, well above any legitimate
	 * SIGALRM-EINTR-during-vmexit blip but well below the 30-second
	 * SERVER_FAIL timeout). Settable via debugfs at boot time and via
	 * the `kvm_v2_eintr_loop_threshold=` kernel parameter.
	 */
	struct task_struct *eintr_run_task;
	unsigned int        eintr_run_count;

	/*
	 * Round 7 Branch B (2026-05-18): per-vCPU dispatch path audit.
	 *
	 * The kvm-v2 Django-loopback flake (state-audit/44 Round 4-6) is
	 * gated through kvm_v2_load_user_sregs at vcpu.c:1819: when
	 * `cross_task || lag>=KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD` we
	 * issue a heavy `KVM_SET_SREGS` ioctl (the "prev_roots[]-drop"
	 * path); otherwise the cheap `KVM_SYNC_X86_SREGS` dirty-bit path
	 * runs on the next KVM_RUN. Round 4 showed forcing heavy on every
	 * dispatch suppresses the flake; the narrowed gate retains a 1%
	 * residual that the cheap path leaks past. Both code paths
	 * ultimately invoke `__set_sregs → kvm_mmu_reset_context` in KVM,
	 * so under the "they're functionally equivalent" framing the gate
	 * should not matter for the prev_roots[]-staleness class of bug.
	 *
	 * These counters expose the empirical heavy/cheap split per vCPU
	 * so the next post-soak inspection (`pr_info` at vcpu_destroy +
	 * a ratelimited tick every 1M dispatches) confirms (a) the heavy
	 * path is firing on the order Round 4 predicted, and (b) the cheap
	 * path actually dominates dispatch volume. If a future fix forces
	 * heavy-only (or vice versa), the per-vCPU totals fall out as a
	 * single line in dmesg without re-enabling the state_trace ring
	 * (which Round 6 showed Heisenbugs away the flake).
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
 * Per-CPU accessor for Phase C.2's dispatcher. Returns the vCPU pinned
 * to host CPU `cpu` or NULL if `cpu` is out of range / the pool isn't
 * initialised. Phase C.1 exports the accessor so C.2's vcpu_run helper
 * lands without further header churn; today it has no callers.
 */
struct kvm_v2_vcpu *kvm_v2_vcpu_get(int cpu);

/*
 * Phase E.1 (memo 26 §E.1): install IDT/GDT bases in SREGS for one
 * pool member. Called from kvm_v2_exception_install (exception.c)
 * after the IDT + handler-stubs + GDT pages are populated, iterating
 * every existing pool entry to retroactively patch the descriptor-
 * table bases that vcpu_create_one's eager install couldn't fill in.
 *
 * Codex --search audit finding #5: kvm_v2_install_production_sregs ran
 * at vcpu_create before E.1's pages existed, so sregs.idt / sregs.gdt
 * stayed at the KVM_GET_SREGS defaults (zero base, zero limit). E.1
 * uses this helper as the late-patching seam — the alternative
 * (gating idt/gdt writes inside install_production_sregs on
 * vm->idt_kva != NULL) was rejected to keep one-helper-per-concern
 * symmetry with the existing CPUID / MSR / SREGS / sigmask install
 * helpers.
 *
 * Phase E.2: extended to also write sregs.tr (per-vCPU TR cache
 * pointing at the per-vCPU TSS body). Caller responsibility: invoke
 * kvm_v2_install_per_vcpu_ist_tss FIRST so vcpu->tss_gva is non-zero
 * by the time this helper runs. v1 archive mirror for the TR
 * programming: kvm-v1-archive/thread.c:2889-2898 (sregs.tr =
 * { base, limit=103, selector=KVM_BOOTSTRAP_TSS_SEL=0x30,
 *   type=11 = 64-bit busy TSS, present=1, dpl=0, s=0, g=0 }).
 *
 * Defined in vcpu.c alongside kvm_v2_install_production_sregs so both
 * SREGS install paths share the same TU.
 */
int kvm_v2_install_descriptors_sregs(struct kvm_v2_vm *vm,
				     struct kvm_v2_vcpu *vcpu);

/*
 * Phase E.2 (memo 26 §E.2): allocate + install the per-vCPU IST stack
 * and TSS pages. Called from kvm_v2_exception_install for every pool
 * member BEFORE kvm_v2_install_descriptors_sregs (so SREGS.tr's
 * base/limit point at the freshly-installed TSS body).
 *
 * Allocates two pages from buddy (`__get_free_page(GFP_KERNEL |
 * __GFP_ZERO)`), writes the TSS body's IST1 field at offset 36
 * (matching v1's archive layout at kvm-v1-archive/thread.c:1623-1627),
 * installs PTE entries at PTE[KVM_V2_IST_BASE_SLOT + cpu*2 .. +1] of
 * trampoline_pte_kva, and stashes ist_stack_kva/gpa/top_gva and
 * tss_kva/gpa/gva on the vcpu struct.
 *
 * Idempotent at the per-vCPU level — re-invocation when ist_stack_kva
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
 * Symmetric per-vCPU teardown — clear PTE[KVM_V2_IST_BASE_SLOT + cpu*2
 * .. +1], free the IST stack + TSS pages, NULL the vcpu fields. Called
 * from kvm_v2_exception_free over every pool member BEFORE it clears
 * PTE[1..3] and frees the IDT/handlers/GDT pages. Safe on a
 * never-installed vCPU (NULL ist_stack_kva → no-op).
 */
void kvm_v2_exception_free_per_vcpu(struct kvm_v2_vm *vm,
				    struct kvm_v2_vcpu *vcpu, int cpu);

/*
 * Phase H gadget Phase 2 (2026-05-04): allocate the per-vCPU gadget
 * state page + install the PTE entry at PTE[KVM_V2_GADGET_BASE_SLOT +
 * cpu] of the trampoline_pte_kva chain. Called from
 * kvm_v2_exception_install's per-vCPU loop alongside
 * kvm_v2_install_per_vcpu_ist_tss. Stores
 * vcpu->{gadget_state_kva, _gpa, _gva} for the load_user_sregs
 * refresh path.
 *
 * Idempotent at the per-vCPU level — re-invocation when
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
 * Symmetric teardown — clear PTE[KVM_V2_GADGET_BASE_SLOT + cpu], free
 * the page, NULL the vcpu fields. Called from kvm_v2_exception_free
 * over every pool member alongside kvm_v2_exception_free_per_vcpu.
 * Safe on a never-installed vCPU (NULL gadget_state_kva → no-op).
 */
void kvm_v2_exception_free_per_vcpu_gadget_state(struct kvm_v2_vm *vm,
						 struct kvm_v2_vcpu *vcpu,
						 int cpu);

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
 * vcpu.c's pre-KVM_RUN marshal-in.  The syscall path deliberately does
 * not call this helper after handle_syscall(): a blocking syscall can
 * schedule away and another UML task can reuse the shared per-host-CPU
 * kvm_run mmap before the sleeping syscall resumes.  Syscall return
 * state stays in per-task regs until the next outer vcpu_run iteration
 * marshals it into the selected vCPU. RFLAGS.bit1 (reserved-must-be-1)
 * is OR'd in defensively.
 */
void kvm_v2_marshal_to_kvm_regs(struct kvm_regs *dst,
				const struct uml_pt_regs *src);

/*
 * Reverse marshal: struct kvm_regs → uml_pt_regs.gp[]. Called after
 * KVM_RUN returns so UML's syscall / fault / signal dispatch sees
 * the guest's post-exit GPRs. HOST_ORIG_AX is intentionally NOT
 * written here — that's an UML entry-path convention the syscall
 * dispatcher arranges once it knows the bucket. Declared here so
 * the D.2 marshal-shape KUnit suite (test_marshal.c) can call it
 * directly; the production caller is kvm_v2_vcpu_run's marshal-out
 * after KVM_RUN returns.
 */
void kvm_v2_marshal_from_kvm_regs(struct uml_pt_regs *dst,
				  const struct kvm_regs *src);

/*
 * Symmetric read-back of sregs.fs.base / gs.base into
 * gp[HOST_FS_BASE/GS_BASE]. Called after every KVM_RUN exit
 * (including EINTR) alongside kvm_v2_marshal_from_kvm_regs. Closes
 * a v1→v2 round-trip regression: v1 propagated FS/GS via
 * KVM_SET_MSRS on every arch_prctl (kvm-v1-archive/thread.c:1918);
 * v2 lifted that into per-dispatch SYNC_REGS but never wired the
 * read-back side. See vcpu.c's helper comment for the full
 * rationale.
 */
void kvm_v2_marshal_sregs_back(struct uml_pt_regs *dst,
			       const struct kvm_sregs *src);

/*
 * H.1b residual fix: per-task IST frame restore. Called from
 * kvm_v2_vcpu_run just before KVM_RUN to re-write the per-vCPU IST
 * stack from current task's snapshot — defends against cross-task
 * IST stack clobber when multiple UML tasks share one per-host-CPU
 * vCPU. No-op if current->thread.arch.kvm_v2.ist_pending is false
 * (last exit was a SYSCALL or there's no pending exception frame).
 *
 * Defined in syscall_trap.c next to ist_frame_read/write.
 */
void kvm_v2_ist_frame_restore_pending(struct kvm_v2_vcpu *vcpu);

/*
 * #121-D15 fix (2026-05-01): snapshot the raw IDT-pushed exception
 * frame from the IST stack into current's per-task ist_frame[]
 * storage AS-IS (no UML-handler-side rewriting). Used by the EINTR
 * path in kvm_v2_vcpu_run when the EINTR caught the vCPU between
 * hardware IDT-delivery (CPU pushed the frame and set RIP=stub
 * start) and the in-guest stub's first instruction. Without this
 * snapshot, another UML task running on the same per-host-CPU vCPU
 * before this task resumes would push its own IDT frame to the
 * shared IST stack, clobbering this task's pre-stub frame; on
 * resume, the stub's iretq tail (which we ALSO bypass after
 * handle_io_pf, but the next stub run on resume relies on the
 * frame being intact for its correct user_rip/cs/rsp/rflags fields)
 * pops the wrong task's state.
 *
 * Sets ist_pending=true so the next dispatch's
 * kvm_v2_ist_frame_restore_pending() reinstates the IST-stack
 * contents.
 */
void kvm_v2_ist_frame_snapshot_raw(struct kvm_v2_vcpu *vcpu);

/*
 * Phase G.2 cross-vCPU guest-TLB kick. Called from
 * arch/um/kernel/tlb.c::um_tlb_sync after a successful drain.
 * Iterates online CPUs (excluding self) and pthread_sigqueue's
 * IPI_SIGNAL via os_send_ipi so each remote vCPU's KVM_RUN exits
 * with -EINTR; the next dispatch's CR4.PGE toggle flushes the
 * local guest TLB. SMP-only (no-op under CONFIG_SMP=n).
 */
void kvm_v2_tlb_kick_others(struct mm_struct *mm);

/*
 * #121-D15 SMP follow-up (2026-05-01): handle a #PF inline during
 * the EINTR path when the EINTR caught the vCPU mid-IDT-delivery
 * (RIP=stub-start, IDT frame freshly pushed to current vCPU's IST
 * stack). Instead of letting the stub re-run on next dispatch
 * (which under SMP can land on a DIFFERENT vCPU with a different
 * IST page, leaving the saved RSP pointing at the wrong page),
 * process the PF directly: read the IDT frame from THIS vCPU's
 * IST (it's still fresh — caller is preempt-disabled), set regs
 * from frame + cr2, dispatch segv_handler + interrupt_end,
 * snapshot the post-handler IST frame for next dispatch's iretq,
 * marshal regs back so the next KVM_RUN starts at user_rip
 * directly (skipping the stub).
 *
 * Returns 0 always; signature mirrors kvm_v2_handle_io_pf.
 */
int kvm_v2_handle_pf_eintr_inline(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu,
				  u64 cr2);

/*
 * SMP-T17 (2026-05-02): inline #NM handler for the EINTR-mid-NM-stub
 * case. See syscall_trap.c::kvm_v2_handle_nm_eintr_inline for the
 * full mechanism (Bug B — mt-mmap-stress wild kernel-half jump).
 */
int kvm_v2_handle_nm_eintr_inline(struct uml_pt_regs *regs,
				  struct kvm_run *run,
				  struct kvm_v2_vcpu *vcpu);

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

/*
 * Snapshot (memo 26-snapshot, Time-machine #168 Phases 1-3).
 *
 * Captures a vCPU's scalar state (registers, sregs, XSAVE area,
 * XCR0, pending-events, 7 MSRs) plus the per-VM memslot contents
 * (Phase 3) into a heap-allocated container so a later
 * kvm_v2_snapshot_restore_full() reinstates it. v1's working
 * implementation lives at kvm-v1-archive/snapshot.c (647 LoC);
 * v2's port handles three deltas:
 *
 *   1. Per-host-CPU vCPU pool — snapshot picks the pool entry whose
 *      vcpu->last_task == current (memo 26-snapshot §3.1 / §4.3).
 *   2. KVM_GET_FPU → KVM_GET_XSAVE so SMP-T57 Phase A's XCR0.YMM
 *      upper-128 is preserved (§3.2).
 *   3. New: KVM_GET_XCRS captures XCR0 itself (§3.3).
 *
 * Phase 1 implemented alloc/destroy/free + capture_regs_only +
 * restore_full(regs-only path). Phase 2 stood up the KUnit fixture
 * for vCPU priming at boot time. Phase 3 (this revision) implements
 * the full capture path:
 *
 *   - Per-VM memslot copy. The v2 layout has a single giant
 *     physmem memslot (gpa=0..physmem_size, hva=uml_physmem)
 *     installed by kvm_v2_physmem_memslot_install plus any future
 *     per-region memslots from region.c. The full capture walks
 *     vm->memslots under vm->lock and copies each slot's bytes to
 *     a kvmalloc'd buffer hung off snap->memslots[i].data.
 *   - IDT/GDT/IST/TSS/gadget-state pages all live in physmem
 *     (allocated via __get_free_page / alloc_page from the buddy
 *     allocator; UML's physmem covers all kernel pages by
 *     construction) so they're snapshotted IMPLICITLY by the
 *     physmem memslot copy. There is no separate "capture IDT"
 *     step — the bytes are part of the memslot copy.
 *
 * Phase 3 design memo:
 *   Documentation/virt/uml/redesign/02-workstreams/
 *   D-kvm-backend/26-snapshot-v2-port.md §Phase 3
 */
#define KVM_V2_SNAPSHOT_MSR_COUNT	7

/**
 * struct kvm_v2_memslot_snapshot - per-memslot capture entry.
 *
 * One entry per registered memslot. Allocated as a heap array
 * (snap->memslots) by kvm_v2_snapshot_capture_full; freed by
 * kvm_v2_snapshot_free.
 *
 * @region:	the KVM_SET_USER_MEMORY_REGION descriptor that was
 *		used to register the slot. Restore replays it through
 *		KVM_SET_USER_MEMORY_REGION to make sure the host-side
 *		mapping is in place before we memcpy bytes back.
 * @data:	kvmalloc'd copy of the slot's contents at capture
 *		time (size = region.memory_size). NULL means "skip
 *		this entry" — used by restore-side defensive checks.
 * @data_size:	size of @data in bytes; matches region.memory_size.
 */
struct kvm_v2_memslot_snapshot {
	struct kvm_userspace_memory_region	region;
	void					*data;
	size_t					 data_size;
};

/**
 * struct kvm_v2_snapshot - captured vCPU + memslot state.
 *
 * @regs:	GP regs (RIP/RSP/RFLAGS + 16 GPRs). KVM_GET_REGS.
 * @sregs:	segments + CR0/2/3/4 + IDT/GDT/TR/LDT. KVM_GET_SREGS.
 *		Phase 3: also covers the IDT/GDT/TSS base/limit fields
 *		(sregs.idt / sregs.gdt / sregs.tr) — those are scalar
 *		descriptor-table cache state, NOT the table contents
 *		themselves (the contents live in physmem and ride the
 *		memslot copy).
 * @xsave:	XSAVE area (4 KB legacy fixed-size struct kvm_xsave;
 *		KVM_GET_XSAVE). Covers X87/SSE/YMM under v2's XCR0=0x7;
 *		AVX-512/AMX bits stay zero per the curated CPUID mask
 *		so KVM_GET_XSAVE2's variable-size shape isn't required.
 * @xcrs:	XCR0 (and any future XCRn). KVM_GET_XCRS. v1 omitted
 *		this; v2 needs it because SMP-T57 Phase A made XCR0
 *		non-trivial.
 * @events:	pending exception / IRQ window / NMI state.
 *		KVM_GET_VCPU_EVENTS.
 * @msrs:	7-entry MSR list (LSTAR/STAR/FMASK/KERNEL_GS_BASE/
 *		FS_BASE/GS_BASE/EFER). KVM_GET_MSRS.
 * @memslots:	Phase 3: per-memslot capture array. NULL when the
 *		snapshot was captured by kvm_v2_snapshot_capture_regs_only.
 *		Allocated by capture_full via kvmalloc(memslot_count
 *		* sizeof(*memslots)); each entry's .data is a separate
 *		kvmalloc'd buffer sized by region.memory_size.
 * @memslot_count: number of valid entries in @memslots.
 * @mem_backing: legacy single-buffer field kept for ABI continuity
 *		with Phase 1. Always NULL under Phase 3; @memslots
 *		is the authoritative storage.
 * @mem_size:	legacy single-buffer length; always 0 under Phase 3.
 * @task_iotrap_fpu: Phase 4. Per-task FPU snapshot pulled from
 *		current->thread.arch.kvm_v2.iotrap_fpu at capture time.
 *		Distinct from @xsave (which reflects the vCPU's view, i.e.
 *		whichever task last dispatched). On a per-host-CPU vCPU
 *		pool shared by multiple UML tasks, capturing the calling
 *		task's saved iotrap_fpu lets restore re-bind FPU state to
 *		the correct task regardless of which task happens to own
 *		the vCPU at restore time.
 * @task_iotrap_events: Phase 4. Per-task pending-event snapshot
 *		pulled from current->thread.arch.kvm_v2.iotrap_events
 *		(the SMP-T75 architectural shape).
 * @task_iotrap_fpu_valid: true iff iotrap_fpu was populated on the
 *		source task at capture time.
 * @task_iotrap_events_valid: true iff iotrap_events was populated.
 * @task_source_pid: pid of the task that called capture_task. Used
 *		for debug/traceability only — restore is "to current,"
 *		not "to source_pid."
 * @task_state_captured: true iff one of the kvm_v2_snapshot_capture_
 *		task variants populated the task_* fields; gate for
 *		restore_task to refuse to install task state from a
 *		snapshot that never captured it (would zero current's
 *		iotrap_*_valid and lose state across restore).
 */
struct kvm_v2_snapshot {
	struct kvm_regs		regs;
	struct kvm_sregs	sregs;
	struct kvm_xsave	xsave;
	struct kvm_xcrs		xcrs;
	struct kvm_vcpu_events	events;
	struct {
		__u32 nmsrs;
		__u32 pad;
		struct kvm_msr_entry entries[KVM_V2_SNAPSHOT_MSR_COUNT];
	} msrs;
	struct kvm_v2_memslot_snapshot	*memslots;
	int				 memslot_count;
	void	*mem_backing;
	size_t	 mem_size;

	/* Phase 4 — cross-task semantics (memo 26-snapshot §Phase 4). */
	struct kvm_xsave	task_iotrap_fpu;
	struct kvm_vcpu_events	task_iotrap_events;
	bool			task_iotrap_fpu_valid;
	bool			task_iotrap_events_valid;
	bool			task_state_captured;
	pid_t			task_source_pid;
};

struct kvm_v2_snapshot *kvm_v2_snapshot_alloc(void);
void  kvm_v2_snapshot_destroy(struct kvm_v2_snapshot *snap);
void  kvm_v2_snapshot_free(struct kvm_v2_snapshot *snap);
int   kvm_v2_snapshot_capture(struct kvm_v2_snapshot *snap);
int   kvm_v2_snapshot_capture_regs_only(struct kvm_v2_snapshot *snap);
int   kvm_v2_snapshot_capture_regs_only_for_vcpu(struct kvm_v2_snapshot *snap,
						 struct kvm_v2_vcpu *vcpu);
int   kvm_v2_snapshot_restore_full(struct kvm_v2_snapshot *snap);

/*
 * Phase 3 explicit-vCPU variants (memo 26-snapshot §Phase 3). Take a
 * @vcpu argument so callers can snapshot/restore against an arbitrary
 * pool entry rather than the implicit kvm_v2_snapshot_pick_vcpu()
 * result. The Phase 1 wrappers above forward to these with vcpu=NULL,
 * which falls back to the pick-by-last_task helper.
 *
 * kvm_v2_snapshot_capture_full captures regs + sregs + xsave + xcrs +
 * events + msrs + memslot contents. The memslot contents include the
 * IDT/GDT/IST/TSS/gadget-state pages (allocated from buddy → in
 * physmem → covered by the giant physmem memslot at gpa=0..
 * physmem_size). No separate "capture IDT" step is needed.
 *
 * kvm_v2_snapshot_restore_full_vcpu restores all of the above. Restore
 * order:
 *   1. memslot memcpy back (so descriptor-table pages are valid
 *      before we install the cached sregs.{idt,gdt,tr} bases).
 *   2. KVM_SET_SREGS (must precede KVM_SET_REGS — RIP/RSP validation
 *      against the post-SREGS segment cache).
 *   3. KVM_SET_REGS / SET_XCRS / SET_XSAVE / SET_VCPU_EVENTS /
 *      SET_MSRS.
 *
 * Post-restore: clear vcpu->cpuid_primed if @vcpu differs from the
 * vCPU the snapshot was captured against (memo §C v2-deltas — cross-
 * vCPU restore needs the next dispatch's lazy CPUID arming to run
 * against the restored sregs state). Phase 3 doesn't track that
 * cross-vCPU identity, so we conservatively leave cpuid_primed alone
 * for same-vCPU restore (the captured CPUID is already installed) and
 * leave the cross-vCPU semantics for Phase 4. The cross-task gates
 * (last_task / last_mm / fpu_owner_task / fpu_dirty) ARE cleared — see
 * memo §4.5.
 */
int kvm_v2_snapshot_capture_full(struct kvm_v2_snapshot *snap,
				 struct kvm_v2_vcpu *vcpu);
int kvm_v2_snapshot_restore_full_vcpu(const struct kvm_v2_snapshot *snap,
				      struct kvm_v2_vcpu *vcpu);

/*
 * Phase 4 cross-task variants (memo 26-snapshot §Phase 4). The
 * capture path issues capture_full AND then copies the calling
 * task's iotrap_fpu / iotrap_events into the snapshot's task_*
 * fields. The restore path issues restore_full_vcpu AND then
 * installs the snapshot's task_* fields into current's arch_thread.
 *
 * Use case: record/replay (#169) snapshots from task A's context at
 * a deterministic checkpoint, then later replays from possibly-task-A
 * (in-process restore) or possibly-task-B (cross-task replay). The
 * cross-task path needs the iotrap_* pair re-bound to the replaying
 * task's arch_thread so the next dispatch sees the right per-task
 * state via the SMP-T73 / SMP-T75 plumbing.
 *
 * Distinct API rather than a flag on capture_full so callers that
 * deliberately want vCPU-only state (debug snapshot of a foreign
 * vCPU) don't pay the iotrap_* copy and don't accidentally clobber
 * current's iotrap_* on restore.
 *
 * Returns same as the underlying _full variants; -EINVAL if
 * restore_task is called against a snapshot with !task_state_captured.
 */
int kvm_v2_snapshot_capture_task(struct kvm_v2_snapshot *snap,
				 struct kvm_v2_vcpu *vcpu);
int kvm_v2_snapshot_restore_task(const struct kvm_v2_snapshot *snap,
				 struct kvm_v2_vcpu *vcpu);

/*
 * #181 — snapshot v2 ELF64-core export.
 *
 * Write a previously-captured snapshot to disk in the ET_CORE shape
 * gdb / readelf / crash(8) understand. The on-disk file consists of
 * an Elf64 header, a PT_NOTE phdr (NT_PRSTATUS + NT_FPREGSET +
 * NT_X86_XSTATE + a UML-private state note), and one PT_LOAD per
 * captured memslot mapped at p_vaddr == guest_phys_addr.
 *
 * The two entry points differ only in caller plumbing — _to_fd is the
 * memfd-friendly path used by the KUnit test, _to_file is what the
 * debugfs trigger and the kernel-side `filp_open` path use.
 *
 * Defined in snapshot_elf.c. Format spec lives at
 * Documentation/virt/uml/snapshot-elf-format.rst.
 */
struct file;
int kvm_v2_snapshot_elf_export_to_file(const struct kvm_v2_snapshot *snap,
				       struct file *file);
int kvm_v2_snapshot_elf_export_to_fd(const struct kvm_v2_snapshot *snap,
				     int fd);

/*
 * Record/replay v2 port (memo 27, #169). Phase 1: state machine +
 * static-key gate only; observation/consume hooks land in Phase 2-3.
 *
 * The container shape is intentionally narrower than v1's growable-log
 * design (kvm-v1-archive/record.c:166-196). Phase 1 keeps a fixed-size
 * scratch buffer + a monotonic sequence counter + statistics counters;
 * the variable-length log + side-buffer machinery is deferred to
 * Phase 2.5 once the per-NR routing surface is in place. The state
 * machine + static-key gate land now so the Phase 2 hook in
 * syscall_trap.c can be added as a pure addition without touching the
 * data structures.
 *
 * Design memo:
 *   Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
 *   27-record-replay-v2-port.md §Phase 1.
 */

/**
 * enum kvm_v2_record_state - state machine for the record container.
 *
 * @KVM_V2_RECORD_INIT:	     freshly allocated; observe/consume are
 *			     no-ops; the static-key gate is off (unless
 *			     another container holds it).
 * @KVM_V2_RECORD_RECORDING: armed via kvm_v2_record_start(); observe
 *			     hooks append to the buffer. Static-key gate
 *			     is enabled for the duration.
 * @KVM_V2_RECORD_STOPPED:   captured a record session; observation
 *			     hooks are no-ops again. Container retains
 *			     its buffer + counters for diagnostics.
 *			     kvm_v2_record_replay() advances to REPLAYING.
 * @KVM_V2_RECORD_REPLAYING: replay arm; consume hooks (Phase 3) walk
 *			     the captured entries in FIFO order. Static-
 *			     key gate is re-enabled so the consume hook
 *			     in handle_io_trap fires.
 *
 * State graph (Phase 1):
 *     INIT ─start→ RECORDING ─stop→ STOPPED ─replay→ REPLAYING
 *                                        ↑               │
 *                                        └────stop───────┘
 * Invalid transitions return -EINVAL. start-after-start, replay-
 * without-stop, stop-without-start are all rejected.
 */
enum kvm_v2_record_state {
	KVM_V2_RECORD_INIT = 0,
	KVM_V2_RECORD_RECORDING,
	KVM_V2_RECORD_STOPPED,
	KVM_V2_RECORD_REPLAYING,
};

/**
 * enum kvm_v2_replay_kind - source-of-nondeterminism tag.
 *
 * Phase 1 declares the full enum so the Phase 2-6 observation hooks
 * can land as pure additions. Phase 1's observe/consume are no-op
 * stubs; the enum values are otherwise unused at this phase.
 *
 * Numerically aligned with v1's enum kvm_replay_kind
 * (kvm-v1-archive/record.c:85-91) but with a leading sentinel so a
 * future on-disk replay format can use 0 as "invalid".
 */
enum kvm_v2_replay_kind {
	KVM_V2_REPLAY_NONE = 0,
	KVM_V2_REPLAY_SYSCALL,	/* class-A passthrough syscall return */
	KVM_V2_REPLAY_SIGALRM,	/* SIGALRM injection point (Phase 6) */
	KVM_V2_REPLAY_RDTSC,	/* RDTSC / RDTSCP exit (Phase 5) */
	KVM_V2_REPLAY_VVAR_READ,/* vvar refresh capture (Phase 5) */
	KVM_V2_REPLAY_INTERRUPT,/* preemption injection point */
	KVM_V2_REPLAY_MMIO_READ,/* device read (future) */
	KVM_V2_REPLAY_TIME_TRAVEL,/* time_travel_set_time() advance —
				   * memo 04 Phase 2 (post-2026-05-19
				   * sprint).
				   */
};

/**
 * enum kvm_v2_replay_meta_kind - per-entry metadata-payload tag.
 *
 * Phase 1 declares the enum so the Phase 2.5 side-buffer routing can
 * land additively. Today only NONE is used; recvfrom/readv tags come
 * with Phase 2.5.
 *
 * Mirror of v1's enum kvm_replay_meta_kind (kvm-v1-archive/
 * kvm_backend.h surface).
 */
enum kvm_v2_replay_meta_kind {
	KVM_V2_REPLAY_META_NONE = 0,
	KVM_V2_REPLAY_META_SOCKADDR,
	KVM_V2_REPLAY_META_IOV,
};

/**
 * struct kvm_v2_replay_entry - one captured side-effect.
 *
 * Phase 1 declares the on-buffer wire shape so Phase 2 can append
 * without touching the data structures. Entries are TLV-style: a
 * fixed 16-byte header followed by @size bytes of kind-specific
 * payload. The buffer in struct kvm_v2_record is a packed stream of
 * these entries; Phase 3's consume walks the stream forward from
 * cursor 0.
 *
 * @kind:     enum kvm_v2_replay_kind discriminator.
 * @size:     total entry size in bytes (header + payload). The Phase 3
 *	      consume walker uses this to advance the buffer cursor.
 * @sequence: monotonic counter assigned at append time. Phase 6's
 *	      SIGALRM-determinism plumbing uses this as the "syscall
 *	      count at signal" anchor.
 * @syscall:  inline payload for KVM_V2_REPLAY_SYSCALL entries (Phase 2):
 *		- @nr      syscall number (regs->gp[HOST_ORIG_AX]).
 *		- @_pad    explicit 4-byte hole for 8-byte alignment of @retval.
 *		- @retval  syscall return value (regs->gp[HOST_AX] post-
 *			   handle_syscall).
 *		- @args[6] RDI/RSI/RDX/R10/R8/R9 — captured at observe time,
 *			   replayed at consume time (Phase 3). Side-buffer
 *			   routing (Phase 2.5) replaces the inline path for
 *			   the read/recvfrom/getrandom NRs.
 *
 * Phase 2 grows this from Phase 1's bare header by adding the
 * `syscall` payload inline. Phase 2.5 / Phase 5-6 will add sibling
 * payload members under a union once the per-kind dispatcher lands;
 * the @size field allows mixed-payload entries to coexist in one
 * buffer without callers having to know every kind's payload size
 * at compile time.
 */
struct kvm_v2_replay_entry {
	u32	kind;
	u32	size;
	u64	sequence;
	/*
	 * Per-kind payload. Anonymous union — `e->syscall.foo` and
	 * `e->rdtsc.foo` are both accessed via the entry pointer with
	 * no extra qualifier. Discriminator is @kind; consumers
	 * validate (e->kind == EXPECTED_KIND) before touching the
	 * payload member of that kind.
	 *
	 * Phase 5 (#169) adds @rdtsc alongside @syscall. Phase 6 will
	 * add @sigalrm; Phase 2.5 will add the side-buffer-routed
	 * @sockaddr / @iov siblings under the same union.
	 */
	union {
		struct {
			s32	nr;
			s32	_pad;
			u64	retval;
			u64	args[6];
		} syscall;
		struct {
			u64	value;	/* RDTSC / RDTSCP TSC read */
			u64	_pad[7];
		} rdtsc;
		struct {
			/*
			 * Phase 6 (memo 27 §3.6(b)) — SIGALRM determinism.
			 * @signo            host signal number (SIGALRM in
			 *                   the common case; SIGIO / SIGVTALRM
			 *                   if the future timer mode changes).
			 * @syscall_count_at injected at this rec->syscall_count
			 *                   anchor. The replay-side
			 *                   consume_sigalrm gates on the
			 *                   current syscall_count == this
			 *                   captured value.
			 */
			u32	signo;
			u32	_pad0;
			u64	syscall_count_at;
			u64	_pad1[6];
		} sigalrm;
		struct {
			/*
			 * memo 04 Phase 2 (post-2026-05-19 sprint) —
			 * time_travel_set_time(ns) advance.  Captured at
			 * observe time, replayed at consume time so the
			 * guest sees the same monotonic-clock sequence
			 * across record + replay runs.
			 *
			 * @ns_at_advance         the ns value passed into
			 *                       time_travel_set_time.
			 * @syscall_count_anchor  rec->syscall_count at the
			 *                       observe site — same anchor
			 *                       shape as sigalrm, so the
			 *                       replay-side consume can
			 *                       defend against drift.
			 */
			u64	ns_at_advance;
			u64	syscall_count_anchor;
			u64	_pad[6];
		} time_travel;
	};
};

/**
 * struct kvm_v2_record - record/replay container (Phase 1 shape).
 *
 * @state:	  state machine cursor (enum kvm_v2_record_state).
 * @strict_replay: true to fail-stop on replay divergence / end-of-log
 *		  (v1's default and the Phase 3 default). False to fall
 *		  through to live handle_syscall (loose mode).
 * @buffer:	  kvmalloc'd entry stream (Phase 2 appends here).
 *		  NULL only on allocation failure inside _alloc; once a
 *		  container is returned to the caller the buffer is
 *		  guaranteed non-NULL until _destroy/_free.
 * @buffer_size:  total bytes allocated for @buffer.
 * @buffer_used:  bytes consumed by appended entries (write cursor);
 *		  Phase 2 grows this. Phase 1 keeps it at zero.
 * @buffer_replayed: read cursor for the Phase 3 replay walker;
 *		  bytes consumed via kvm_v2_record_consume_syscall.
 *		  Reset to 0 by kvm_v2_record_replay; invariant
 *		  @buffer_replayed <= @buffer_used at all times.
 * @sequence:	  monotonic counter handed out to each appended entry.
 *		  Phase 6's SIGALRM-on-syscall-count plumbing uses this.
 * @entries_recorded: count of entries appended (= the number of
 *		      observe-hook calls that produced an entry). Stat
 *		      counter for diagnostic visibility.
 * @entries_replayed: count of entries consumed on replay. Mirror.
 * @lock:	  serialises state-machine transitions + future buffer
 *		  appends. Phase 1's KUnit exercises the state machine
 *		  under a single thread; the lock is uncontended at
 *		  Phase 1 but in place for Phase 2's hook to slot into
 *		  without revisiting the data structure.
 *
 * Lifecycle:
 *   1. kvm_v2_record_alloc(buffer_size)  → state=INIT
 *   2. kvm_v2_record_start(rec)          → state=RECORDING; static
 *					     key armed.
 *   3. kvm_v2_record_stop(rec)           → state=STOPPED; static key
 *					     disarmed.
 *   4. kvm_v2_record_replay(rec)         → state=REPLAYING; static
 *					     key re-armed.
 *   5. kvm_v2_record_destroy(rec)        → free + drop static key if
 *					     still held.
 *
 * Concurrency: in Phase 1, only one container is "active" at a time
 * (single-active discipline mirrors v1, kvm-v1-archive/record.c:77).
 * The static-key gate is global; the active-container pointer lives
 * inside record.c. The mutex serialises field updates.
 */
struct kvm_v2_record {
	enum kvm_v2_record_state	state;
	bool				strict_replay;
	void				*buffer;
	size_t				buffer_size;
	size_t				buffer_used;
	size_t				buffer_replayed;
	u64				sequence;
	u64				entries_recorded;
	u64				entries_replayed;
	/*
	 * Phase 6 anchor (memo 27 §3.6(b)): bumped on each
	 * observe_syscall append; sampled into a sigalrm entry's
	 * @syscall_count_at slot at SIGALRM delivery so the replay
	 * driver can gate signal injection on a count-match.
	 * Independent of @sequence (which counts all entries
	 * regardless of kind).
	 */
	u64				syscall_count;
	struct mutex			lock;
};

/*
 * Hot-path gate. Off by default; flipped on by kvm_v2_record_start.
 *
 * The Phase 2 observe hook in syscall_trap.c wraps its call in
 *
 *   if (static_branch_unlikely(&um_kvm_v2_record_enabled))
 *	kvm_v2_record_observe_syscall(...);
 *
 * so non-record runtime pays zero per-syscall cost — the branch
 * compiles to a 5-byte NOP that the kernel patches out at boot, and
 * `static_branch_enable()` flips it to a `jmp` when record arms.
 *
 * Mirrors v1's DEFINE_STATIC_KEY_FALSE(um_kvm_record_enabled)
 * (kvm-v1-archive/record.c:73).
 */
DECLARE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled);

/*
 * Public surface — Phase 1: alloc/destroy/start/stop/replay + the
 * strict-replay toggle. Phase 2 adds the observe/consume entry
 * points; Phase 3+ adds the per-NR routing helpers.
 */
struct kvm_v2_record *kvm_v2_record_alloc(size_t buffer_size);
void kvm_v2_record_destroy(struct kvm_v2_record *rec);
void kvm_v2_record_free(struct kvm_v2_record *rec);
int  kvm_v2_record_start(struct kvm_v2_record *rec);
int  kvm_v2_record_stop(struct kvm_v2_record *rec);
int  kvm_v2_record_replay(struct kvm_v2_record *rec);
int  kvm_v2_record_set_strict_replay(struct kvm_v2_record *rec, bool strict);
bool kvm_v2_record_strict_replay(const struct kvm_v2_record *rec);

/*
 * Accessor for the single-active-record slot. Returns the container
 * currently registered with kvm_v2_record_start / _replay, or NULL.
 *
 * The Phase 2 hook in syscall_trap.c::kvm_v2_handle_io_trap calls this
 * INSIDE the static_branch_unlikely(&um_kvm_v2_record_enabled) gate,
 * so the slot pointer is read only when the static key was flipped on
 * by _start/_replay — which means the registered container was non-NULL
 * at the time of the flip. The accessor still re-checks under the
 * record_lock spinlock because _stop / _destroy can race the hot path
 * (they call static_branch_disable AFTER clearing the slot).
 *
 * Returning NULL is a quiet no-op for the hook — the gate may have
 * been flipped off between the static_branch_unlikely read and this
 * accessor call. Phase 1 / 2 never see this in single-CPU KUnit; SMP
 * matters more under Phase 3+ when the gate flips around live record
 * sessions.
 */
struct kvm_v2_record *kvm_v2_record_active(void);

/*
 * Phase 2 stub — observe hook called from syscall_trap.c. Phase 1
 * provides a no-op definition so the static-key gate can be
 * exercised end-to-end without the syscall_trap.c integration. The
 * Phase 2 commit re-implements this function to actually append to
 * the buffer; Phase 1 callers (none in tree today) just see a NOP.
 */
void kvm_v2_record_observe_syscall(struct kvm_v2_record *rec,
				   unsigned long syscall_nr,
				   long ret_value,
				   const struct uml_pt_regs *regs);

/*
 * Replay-side consume hook (Phase 3). FIFO walk over @rec->buffer
 * starting at @rec->buffer_replayed. Returns:
 *
 *   > 0 (1):    entry served — *ret_value populated with the recorded
 *               syscall retval; @rec->buffer_replayed advanced by
 *               entry->size; @rec->entries_replayed bumped. Caller
 *               must skip live handle_syscall + write @ret_value back
 *               into regs->gp[HOST_AX].
 *   = 0:        @rec is NULL or @rec->state != REPLAYING (no-op).
 *               Caller falls through to live handle_syscall.
 *   -ENODATA:   end-of-log (no more entries at the cursor). In strict
 *               replay mode the caller delivers SIGSEGV to current;
 *               in loose mode the caller falls through to live.
 *   -EILSEQ:    sequence error — next entry's kind != SYSCALL OR
 *               entry->syscall.nr != @syscall_nr. Strict-mode
 *               divergence; loose mode fall-through.
 *
 * Locking: takes @rec->lock to serialise against a racing _stop /
 * future observation. The same mutex protects @buffer_used so the
 * "buffer_replayed <= buffer_used" invariant is consistent under
 * concurrent access.
 */
int kvm_v2_record_consume_syscall(struct kvm_v2_record *rec,
				  unsigned long syscall_nr,
				  long *ret_value);

/*
 * Phase 5 (#169) RDTSC observe/consume.
 *
 * observe_rdtsc appends a KVM_V2_REPLAY_RDTSC entry to @rec->buffer
 * carrying the captured TSC value. consume_rdtsc walks @rec->buffer
 * forward from @rec->buffer_replayed and serves the next RDTSC entry,
 * writing its value via @value_out.
 *
 * The current consume walker is per-stream (one cursor shared by the
 * syscall and rdtsc kinds). Phase 6's mixed-stream consumer will
 * extend with kind-skipping; today the test workloads keep the
 * streams ordered enough that this works in practice. Phase 5's
 * KUnit exercises observe/consume in isolation (no mixed-stream).
 *
 * observe_rdtsc has the same quiet-no-op shape as observe_syscall:
 * NULL @rec, state != RECORDING, or buffer-full all silently drop the
 * append. consume_rdtsc returns 1 on success, 0 on quiet no-op
 * (NULL @rec or state != REPLAYING), -ENODATA on end-of-log, -EILSEQ
 * on kind mismatch.
 */
void kvm_v2_record_observe_rdtsc(struct kvm_v2_record *rec, u64 tsc_value);
int  kvm_v2_record_consume_rdtsc(struct kvm_v2_record *rec, u64 *value_out);

/*
 * Phase 6 (#169) SIGALRM determinism via syscall-count anchor.
 *
 * Each observe_syscall append bumps @rec->syscall_count (a separate
 * counter from @sequence so the SIGALRM log can reference a stable
 * anchor independent of mixed-kind entries). At SIGALRM delivery
 * time (host signal interception or VMCS interrupt-window exit, per
 * the Phase 6 wiring) the caller invokes observe_sigalrm with the
 * signo + current @syscall_count snapshot; the entry lands in the
 * stream under KVM_V2_REPLAY_INTERRUPT.
 *
 * On replay, the dispatcher gates each SIGALRM injection on
 * consume_sigalrm returning 1 — but only when the current
 * @syscall_count == the entry's captured @syscall_count_at. This is
 * memo 27 §3.6 decision (b): "syscall-count-driven SIGALRM."
 *
 * Phase 6 lands the API + KUnit-level round-trip; the host-side
 * signal-interception wiring that turns a real SIGALRM into an
 * observe_sigalrm call is a follow-on (memo §"Phase 6 wiring").
 */
void kvm_v2_record_observe_sigalrm(struct kvm_v2_record *rec, u32 signo);
int  kvm_v2_record_consume_sigalrm(struct kvm_v2_record *rec,
				   u32 *signo_out,
				   u64 *syscall_count_at_out);

/*
 * memo 04 Phase 2/3 (post-2026-05-19 sprint) — time_travel_set_time(ns)
 * observe + consume.  Hooks live next to the time-travel-clock site in
 * arch/um/kernel/time.c (the Phase 1 hook landed at b2ff4c0b775e).
 *
 * observe_time_travel: append a KVM_V2_REPLAY_TIME_TRAVEL entry carrying
 * @ns_at_advance.  The current @rec->syscall_count is captured as
 * @syscall_count_anchor so replay can defend against drift, same shape
 * as observe_sigalrm.
 *
 * consume_time_travel: returns 1 on success (with @ns_out written), 0
 * on quiet no-op (NULL @rec or state != REPLAYING), -ENODATA on
 * end-of-log, -EILSEQ on kind mismatch.
 */
void kvm_v2_record_observe_time_travel(struct kvm_v2_record *rec,
				       u64 ns_at_advance);
int  kvm_v2_record_consume_time_travel(struct kvm_v2_record *rec,
				       u64 *ns_out,
				       u64 *syscall_count_anchor_out);

/*
 * memo 04 Phase 2/3 + HONEST-AUDIT §1 follow-up.  The
 * `um_hook_record_replay` static-key gate is global — once on, it
 * fires from every time_travel_set_time call.  That contention
 * with KUnit tests (which exercise observe_/consume_ APIs directly
 * on a private rec) means the gate must NOT be auto-enabled by
 * kvm_v2_record_start.  Production callers explicitly engage via:
 *
 *   kvm_v2_record_engage_global_hooks();   // arm
 *   ... use rec ...
 *   kvm_v2_record_disengage_global_hooks();// disarm
 *
 * Tests skip these — they only call the observe/consume API.
 */
void kvm_v2_record_engage_global_hooks(void);
void kvm_v2_record_disengage_global_hooks(void);

#if IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_KUNIT)
/*
 * Snapshot Phase 2 (memo 26-snapshot §Phase 2): KUnit fixture hook
 * that drives the lazy first-dispatch arming sequence (CPUID +
 * CR4.OSXSAVE + XCR0=FP|SSE|YMM) on a pool entry, so snapshot tests
 * have a vCPU whose KVM_GET_* / KVM_SET_* ioctls all accept at
 * do_basic_setup time — before any user task has dispatched. After
 * a successful return @v->cpuid_primed == true. Build-gated on
 * CONFIG_UM_BACKEND_KVM_V2_KUNIT (zero text in production builds).
 *
 * Defined in vcpu.c so the priming logic stays co-located with the
 * static helpers (kvm_v2_install_cpuid / kvm_v2_install_xcrs) it
 * dispatches to.
 */
int kvm_v2_vcpu_prime_for_kunit(struct kvm_v2_vcpu *v);
#endif /* CONFIG_UM_BACKEND_KVM_V2_KUNIT */

#endif /* __ARCH_UM_BACKEND_KVM_V2_H */
