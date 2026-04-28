/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal header for arch/um/backend/kvm/.
 *
 * The ops-table struct, dispatch macro, and per-backend op
 * prototypes are all in <backend.h> (shared header). This header
 * is the stable include point for kvm TUs, matching the
 * ptrace/seccomp backends' convention.
 *
 * Workstream D-02 scaffold; D-03a adds probe; D-03b adds the
 * per-UML-process kvm_um context consumed by mm.c.
 */
#ifndef __ARCH_UM_BACKEND_KVM_H
#define __ARCH_UM_BACKEND_KVM_H

#include <linux/mutex.h>
#include <linux/refcount.h>
#include <backend.h>

/*
 * Single per-UML-process KVM context (decisions-log D57):
 * one /dev/kvm handle, one KVM_CREATE_VM fd, shared across every
 * UML guest mm. Fields are populated by kvm_init() and stay
 * read-only thereafter.
 *
 * vcpu0_fd + run0 + run_size are the !INTEGRATED harness scaffold;
 * under CONFIG_UM_BACKEND_KVM_INTEGRATED each UML task allocates its
 * own per-task struct kvm_vcpu_handle via kvm_vcpu_for_current() —
 * the singleton vcpu0 was deleted by Stage A.7 for INTEGRATED.
 *
 * Shutdown semantics: kvm_shutdown() is best-effort. It closes vcpu/
 * vm/kvm fds without checking attached mms. Callers must ensure no
 * task is in kvm_run_userspace before invoking shutdown — UML
 * cleanup ordering achieves this implicitly by tearing down user
 * mms before backend shutdown.
 */
struct kvm_um {
	int		kvm_fd;		/* /dev/kvm */
	int		vm_fd;		/* KVM_CREATE_VM */
	int		vcpu0_fd;	/* KVM_CREATE_VCPU, slot 0 — kept ONLY
					 * for snapshot.c/record.c/test_ops.c
					 * diagnostic paths that haven't yet
					 * been migrated to per-task vCPU.
					 * Production hot path uses
					 * current->thread.arch.kvm.vcpu. */
	void		*run0;		/* mmap'd kvm_run for vcpu0 */
	size_t		run_size;	/* KVM_GET_VCPU_MMAP_SIZE */
	u64		sync_regs_caps;	/* KVM_CAP_SYNC_REGS bitmap; 0 if
					 * unsupported. When KVM_SYNC_X86_REGS
					 * is set, GP regs travel through the
					 * mmap'd kvm_run struct instead of
					 * KVM_GET/SET_REGS ioctls (perf-lever
					 * #2 — 2 ioctls per syscall saved).
					 */
	/*
	 * Stage A.4c: deleted the per-vCPU caches that previously lived
	 * here (msrs_primed / kernel_gs_base_primed / cpuid_done /
	 * sregs_primed / cached_cr3_gpa / cached_fs_base / cached_gs_base).
	 * They were singletons reflecting whichever task ran KVM_RUN last
	 * — wrong under per-task vCPU. The fields now live on each task's
	 * struct kvm_vcpu_handle (defined below), where they correctly
	 * track per-vCPU last-programmed state.
	 */
	u32		pmu_caps;	/* KVM_CAP_PMU_CAPABILITY (task
					 * #255). Non-zero means the host KVM
					 * exposes a vPMU + tunable caps. We
					 * don't tune today — vPMU is enabled
					 * by default, which is what research
					 * builds want; logging the cap value
					 * lets perf tooling confirm the
					 * channel is live.
					 */
	bool		pmu_event_filter_supported;
					/* KVM_CAP_PMU_EVENT_FILTER (task
					 * #255). Indicates KVM_SET_PMU_EVENT_
					 * FILTER ioctls are accepted; future
					 * follow-on if research workloads
					 * need narrower event windows.
					 */

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	/*
	 * Stage A.4c: deleted the deprecated singleton shadow PGD
	 * fields (shadow_pgd_page / shadow_pgd / shadow_pgd_gpa /
	 * shadow_dirty / shadow_pgd_synced / shadow_pgd_synced_mm /
	 * shadow_pgd_synced_va). All shadow PT state now lives on
	 * struct kvm_shadow_mm, allocated per-mm and reachable via
	 * mm->context.id.kvm_shadow / kvm_shadow_mm_for(mm).
	 */

	/*
	 * Memo 11 G3 gadget state page. Allocated lazily on
	 * first kvm_enter_guest call; freed in shutdown.
	 *   gadget_state_page — backing struct page *.
	 *   gadget_state      — kernel VA of the state struct.
	 *   gadget_state_gpa  — __pa(gadget_state).
	 *   gadget_state_va   — guest VA where the page is
	 *                       mapped in the shadow PT;
	 *                       MSR_GS_BASE is set to this.
	 */
	struct page	*gadget_state_page;
	struct kvm_gadget_state *gadget_state;
	u64		gadget_state_gpa;
	u64		gadget_state_va;

	/*
	 * Memo 11 G5 gadget vvar page. Same lazy-alloc +
	 * shadow-PT-map shape as the state page; maps one
	 * page higher in the guest VA range so that a single
	 * MSR_KERNEL_GS_BASE (pointing at the state page)
	 * covers both structs via disp32 addressing:
	 *   %gs:<KVM_GADGET_OFF_*>        → state fields
	 *   %gs:<PAGE_SIZE + KVM_VVAR_OFF_*> → vvar fields
	 * That keeps the gadget's `swapgs` bracket single
	 * and lets disp32 reach both bands without the
	 * handler having to reload GS mid-sequence.
	 */
	struct page	*gadget_vvar_page;
	struct kvm_gadget_vvar *gadget_vvar;
	u64		gadget_vvar_gpa;
	u64		gadget_vvar_va;
#endif
};

/*
 * Accessors. All return the module-static kvm_ctx; kvm_fd / vm_fd
 * are -1 until init() runs. Callers ordered against init_backend()
 * can treat -1 as a contract violation.
 *
 * vcpu0_fd / run0 are kept ONLY for the harness/non-INTEGRATED fallback
 * path (arch/um/backend/kvm/thread.c:#else branch). Under
 * CONFIG_UM_BACKEND_KVM_INTEGRATED, every UML task owns a private
 * struct kvm_vcpu_handle (see below) and MUST NOT touch vcpu0_fd /
 * run0. The pre-redesign singleton model violated KVM's "1 host thread
 * = 1 vCPU for life" contract and produced the documented per-trial
 * flakiness on heavy long-running guest workloads.
 */
int kvm_backend_fd(void);
int kvm_backend_vm_fd(void);
int kvm_backend_vcpu0_fd(void);
struct kvm_um *kvm_backend_ctx(void);

/*
 * ============================================================
 * struct kvm_vcpu_handle — per-task vCPU ownership (Stage A of
 * the 03-architecture-review-2026-04-27 redesign).
 *
 * KVM's API contract: the per-vCPU struct kvm_run mmap is
 * single-writer; vcpu ioctls should be issued from the same host
 * thread that called KVM_CREATE_VCPU; KVM_SET_SIGNAL_MASK must
 * cover the KVM_RUN window so signals route deterministically.
 *
 * The pre-redesign UML KVM backend had ONE vcpu0_fd shared across
 * every UML task via cooperative-thread `switch_threads` (longjmp
 * on the same host thread). That structurally violated all three
 * contract clauses and produced the playbook race classes.
 *
 * Each UML task now allocates a kvm_vcpu_handle on first
 * kvm_run_userspace and pins it for life. The handle owns:
 *
 *   - fd:         KVM_CREATE_VCPU result for this task's vCPU.
 *   - run:        mmap of struct kvm_run for this fd. Single-writer.
 *   - run_size:   KVM_GET_VCPU_MMAP_SIZE result.
 *   - sigmask_installed: KVM_SET_SIGNAL_MASK has been programmed
 *                 with everything blocked except KVM_UM_KICK_SIGNAL.
 *
 * Plus the per-vCPU caches that used to live on the singleton kvm_um
 * struct (cached_cr3_gpa / cached_fs_base / cached_gs_base /
 * sregs_primed / msrs_primed / cpuid_done / kernel_gs_base_primed) —
 * these are intrinsically per-vCPU because they describe what was
 * last programmed via KVM_SET_SREGS / KVM_SET_MSRS on a specific fd.
 *
 * Lifecycle:
 *   - Allocated by kvm_vcpu_for_current() on first use.
 *   - Freed by exit_thread() when the task is reaped.
 *   - Pinned for the host kthread the UML task runs on for life
 *     (UML's cooperative scheduler keeps each task on the same
 *     host pthread; we don't migrate vCPU ownership).
 */
/*
 * Kick signal for the per-task vCPU. Constraints:
 *   - SIGRTMIN+0 (=32) is UML's SMP IPI (arch/um/os-Linux/smp.c).
 *   - SIGRTMIN+1 (=33) is glibc's SIGSETXID — glibc's sigaction()
 *     wrapper returns EINVAL via __is_internal_signal().
 *   - SIGRTMIN+2 (=34) is glibc's SIGCANCEL/SIGTIMER reservation.
 *   - SIGUSR1 is used by the PM wake signal (signal.c:208).
 *
 * Pick SIGRTMIN+5 (=37): clear of all glibc/UML reservations on
 * x86_64 (kernel SIGRTMIN=32, glibc-exposed SIGRTMIN=35), and within
 * the SIGRTMIN..SIGRTMAX range every libc accepts.
 */
#define KVM_UM_KICK_SIGNAL	(SIGRTMIN + 5)

struct kvm_vcpu_handle {
	int		fd;			/* KVM_CREATE_VCPU result */
	void		*run;			/* mmap of struct kvm_run */
	size_t		run_size;		/* KVM_GET_VCPU_MMAP_SIZE */
	bool		cpuid_done;		/* KVM_SET_CPUID2 done */
	bool		msrs_primed;		/* MSR_STAR/LSTAR/FMASK done */
	bool		kernel_gs_base_primed;	/* MSR_KERNEL_GS_BASE done */
	bool		sregs_primed;		/* KVM_SET_SREGS done at least once */
	u64		cached_cr3_gpa;		/* last sregs.cr3 programmed */
	u64		cached_fs_base;		/* last sregs.fs.base */
	u64		cached_gs_base;		/* last sregs.gs.base */
	/*
	 * Stage A.4d: per-vCPU TLB-flush generation. Compared to
	 * shadow_mm->tlb_gen on each kvm_enter_guest; mismatch triggers
	 * the CR4.PGE-toggle SREGS write (forces VMCS reload + guest TLB
	 * invalidation). Replaces the per-mm shadow->dirty boolean which
	 * was unsound under per-task vCPU (one task's flush left
	 * sibling-task vCPUs sharing the same mm with stale guest TLB).
	 */
	u64		last_flushed_tlb_gen;
};

struct task_struct;
struct kvm_vcpu_handle *kvm_vcpu_handle_alloc(void);
void kvm_vcpu_handle_destroy(struct kvm_vcpu_handle *h);
struct kvm_vcpu_handle *kvm_vcpu_for_current(void);

/*
 * Memo 10 syscall classification. Static truth table lives in
 * arch/um/backend/kvm/syscall_class.c; consumed by
 * kvm_decode_syscall to short-circuit class-D denies and (in
 * future sub-commits) dispatch class-C sigreturn without
 * passing through handle_syscall. Pure function of the syscall
 * number; safe to call from any context.
 *
 * enum value 0 = CLASS_PASSTHROUGH so uninitialised slots in
 * the static table default to the identity dispatcher, which
 * is what we want for the ~370 passthrough syscalls.
 */
enum kvm_syscall_class {
	KVM_SYSCALL_CLASS_PASSTHROUGH = 0,
	KVM_SYSCALL_CLASS_VCPU_STATE,
	KVM_SYSCALL_CLASS_SIGFRAME,
	KVM_SYSCALL_CLASS_TRAP,
	/*
	 * Memo 11 G7: syscalls whose fast path lives in the
	 * in-guest LSTAR gadget (memo 11 G4-G6). A VMEXIT on a
	 * CLASS_GADGET syscall means the gadget chose the
	 * fallback path (retry budget exhausted, unsupported
	 * args, or !CONFIG_UM_BACKEND_KVM_GADGET), in which case
	 * the dispatcher handles it exactly like CLASS_PASSTHROUGH
	 * — the class is a categorization marker for perf gates
	 * and documentation, not a dispatcher branch. See
	 * arch/um/backend/kvm/thread.c::kvm_decode_syscall, which
	 * intentionally only checks for CLASS_TRAP short-circuit.
	 */
	KVM_SYSCALL_CLASS_GADGET,
};

enum kvm_syscall_class kvm_classify_syscall(unsigned long nr);

/*
 * Memo 11 G3: per-vCPU gadget state channel.
 *
 * The systrap gadget (memo 07 + memo 11) needs to read
 * per-task state like current->tgid / uid / gid from
 * ring-3 guest code without a VMEXIT. This struct is the
 * shared page that holds those values; MSR_GS_BASE is
 * programmed to point at it, so gadget handlers can do
 * `mov %gs:KVM_GADGET_FIELD_TGID, %rax` and the CPU
 * dereferences the current task's cached tgid in ~1 cycle.
 *
 * Host-side refresh discipline (v1, ncpus=1): the host
 * rewrites this page at every kvm_enter_guest, right
 * before KVM_RUN. Since the vCPU is single-threaded and
 * KVM_RUN is synchronous, there's no concurrent writer
 * while the guest is reading — seqlock elided. When SMP
 * (ncpus>1) lands, each vCPU needs its own state page
 * and a seqlock for reader-retry against concurrent
 * cred-change syscalls hitting a sibling vCPU; memo 11
 * §"Safety discipline" point 6 tracks that v2 shape.
 *
 * Field layout is ABI-stable: gadget asm hardcodes the
 * offsets. Changing layout requires updating every
 * handler in lockstep.
 */
struct kvm_gadget_state {
	u32 seq;			/* reserved for SMP v2; always 0 in v1 */
	u32 cpu_id;			/* vCPU index; 0 under ncpus=1 */
	/*
	 * Linux naming convention: "tgid" is what POSIX/libc
	 * calls the process ID (returned by getpid(2)); "tid"
	 * is the kernel's thread ID (returned by gettid(2)).
	 * They match on single-threaded tasks; they diverge
	 * on pthreads. Gadget handlers read from the field
	 * matching the syscall they implement:
	 *   - getpid  → tgid
	 *   - gettid  → tid
	 *   - getppid → ppid (parent's tgid)
	 */
	u32 tgid;			/* task_tgid_vnr(current) */
	u32 tid;			/* task_pid_vnr(current) */
	u32 ppid;			/* task_ppid_nr(current) */
	u32 uid;
	u32 euid;
	u32 gid;
	u32 egid;
	u32 _pad;			/* align to 8 bytes */
};

/* Gadget asm will reference these as %gs:<offset>. */
#define KVM_GADGET_OFF_SEQ	0x00
#define KVM_GADGET_OFF_CPU_ID	0x04
#define KVM_GADGET_OFF_TGID	0x08
#define KVM_GADGET_OFF_TID	0x0c
#define KVM_GADGET_OFF_PPID	0x10
#define KVM_GADGET_OFF_UID	0x14
#define KVM_GADGET_OFF_EUID	0x18
#define KVM_GADGET_OFF_GID	0x1c
#define KVM_GADGET_OFF_EGID	0x20

/*
 * Memo 11 G5 gadget vvar page. Separate from the per-task
 * state page (above) because its update cadence is host
 * timer-tick driven, not per-kvm_enter_guest; it's logically
 * a time source, not a task-state snapshot.
 *
 * Seqlock layout follows the canonical x86 vdso pattern: seq
 * is even when the writer isn't active, odd when a write is
 * in progress. Readers retry if they observe an odd seq or a
 * seq change between the pre-read and post-read samples.
 * Handler asm in G5c implements the retry.
 *
 * v1 populates CLOCK_MONOTONIC and CLOCK_REALTIME from
 * ktime_get_ns() + ktime_get_real_ts64(). Refresh runs at
 * every kvm_enter_guest, same cadence as gadget_state. A
 * future G5 follow-on can hook into UML's timer tick for
 * higher-frequency updates.
 */
struct kvm_gadget_vvar {
	u32 seq;			/* seqlock counter */
	u32 _pad0;
	s64 monotonic_sec;
	s64 monotonic_nsec;
	s64 realtime_sec;
	s64 realtime_nsec;
	/*
	 * Audit round-5 F8: per-refresh call budget for the
	 * clock gadget. kvm_gadget_vvar_refresh resets to
	 * KVM_VVAR_BUDGET_INITIAL; the gadget atomically
	 * decrements on each successful call and falls back
	 * when the signed value goes negative. On UML the
	 * host-side hrtimer path can't fire mid-KVM_RUN
	 * (UML's virtual IRQs are driven by SIGALRM which
	 * is blocked until KVM_RUN returns), so an in-gadget
	 * budget is the only reliable mechanism that bounds
	 * vvar staleness without requiring a periodic host-
	 * side writer. When the budget expires, the fallback
	 * VMEXIT triggers handle_syscall → interrupt_end →
	 * next kvm_enter_guest which refreshes the vvar and
	 * resets the budget.
	 */
	s32 budget;
	u32 _pad1;

	/*
	 * Audit round-6 G1: per-process TASK_SIZE cap used by
	 * the output-storing gadgets (clock_gettime, time,
	 * getcpu) to bound user pointers before storing
	 * through them at CPL=0. Without this check, a guest
	 * could pass an %rsi pointing into the kernel half
	 * (canonical 0xffff...) or a non-canonical address;
	 * the gadget runs in ring-0 and would either corrupt
	 * our own ring-0 data structures (mapped supervisor
	 * VA) or trigger #GP on a non-canonical store (no
	 * IDT[13] handler currently). The cap is set once at
	 * vvar_alloc time from UML's task_size global; gadgets
	 * compare the user pointer against it and fall back
	 * if the pointer is at-or-above the cap.
	 *
	 * Gadgets read this via %gs:KVM_VVAR_OFF_TASK_SIZE_CAP.
	 */
	u64 task_size_cap;
	u64 _pad2[2];			/* cache-line pad (64 B struct) */
};

#define KVM_VVAR_OFF_SEQ		0x00
#define KVM_VVAR_OFF_MONO_SEC		0x08
#define KVM_VVAR_OFF_MONO_NSEC		0x10
#define KVM_VVAR_OFF_REAL_SEC		0x18
#define KVM_VVAR_OFF_REAL_NSEC		0x20
#define KVM_VVAR_OFF_BUDGET		0x28
#define KVM_VVAR_OFF_TASK_SIZE_CAP	0x30

/*
 * Audit round-5 F8: initial budget handed to the clock gadget
 * at every refresh. 10000 gadget calls ≈ 300 us on modern
 * silicon (at ~30 ns/call), which is the worst-case staleness
 * the gadget can exhibit before falling back. A smaller budget
 * reduces staleness at the cost of more fallback VMEXITs
 * (~150k cyc each); larger budgets let tight loops run further
 * off a stale vvar. 10000 gives sub-ms staleness with a
 * fallback rate of ~3000/s, whose aggregated cost is ~0.4 ms/s
 * — well under 1% of wall-clock time.
 */
#define KVM_VVAR_BUDGET_INITIAL		10000

/*
 * Lazy memslot registration (D-04a). Call once before entering
 * KVM_RUN; subsequent calls are idempotent no-ops. Returns 0 on
 * success, -errno on failure (caller decides to continue or
 * panic). See lifecycle.c for the "deferred because uml_physmem
 * is set after init_backend()" rationale.
 */
int kvm_ensure_memslot(void);

/*
 * Task #273: one-shot lazy CPUID passthrough. Called from
 * kvm_enter_guest before the first KVM_RUN. Sets kvm_um.cpuid_done
 * on success. Idempotent — subsequent calls fast-path out.
 * No-op stub when CONFIG_UM_BACKEND_KVM_INTEGRATED=n.
 */
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
int kvm_ensure_cpuid_done(struct kvm_vcpu_handle *vcpu);
#else
static inline int kvm_ensure_cpuid_done(struct kvm_vcpu_handle *vcpu)
{
	(void)vcpu;
	return 0;
}
#endif

/*
 * Long-mode SREGS setup helpers (D-04b.1a, arch/um/backend/kvm/
 * sregs.c). Pure data-structure fills; callers are responsible
 * for placing the GDT / page-tables at matching offsets within
 * the vCPU's memslot and invoking KVM_SET_SREGS themselves. See
 * 03b-memslot-policy.md and 04b-long-mode-sregs.md for the
 * address-space model.
 */
struct kvm_sregs;
void kvm_setup_harness_gdt(u64 *gdt);
void kvm_setup_harness_paging(u64 *pml4, u64 *pdpt, u64 *pd);
int kvm_setup_harness_paging_range(u64 *pml4, u64 *pdpt, u64 *pd,
				   unsigned int pd_offset_in_slot,
				   unsigned int npages_2m);
void kvm_setup_harness_sregs(struct kvm_sregs *sregs);

/*
 * Production analogue of kvm_setup_harness_sregs — same segment
 * + CR0/CR4/EFER bits, caller-parameterized CR3 + GDT base.
 * See memo 08 sub-commit #1 and sregs.c for the contract.
 */
void kvm_setup_production_sregs(struct kvm_sregs *sregs,
				u64 cr3_gpa, u64 gdt_gpa);

/*
 * D-04/D-05 follow-on integration gate (memo 08, task #162).
 * When CONFIG_UM_BACKEND_KVM_INTEGRATED=y, `kvm_run_userspace`
 * enters the real KVM_RUN loop via `kvm_enter_guest` instead
 * of panic-ing. Off by default; sub-commits #1-#6 extend the
 * gated code path incrementally. The harness path
 * (CONFIG_UM_BACKEND_KVM_HARNESS) stays independently
 * selectable so regressions in either path are diagnosable
 * separately.
 */
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED

struct mm_struct;
struct mm_id;

/*
 * Per-mm shadow PGD container (#275). Replaces the singleton
 * shadow_pgd that lived on `struct kvm_um`. Each UML mm gets its
 * own shadow tree at mm_attach time; cross-mm switches just point
 * the vCPU's CR3 at a different shadow tree, with no clear/refill
 * dance and no risk of one mm's leaves leaking into another's view.
 *
 * Bootstrap kernel-half mappings (LSTAR trampoline page, IST
 * stack, gadget state + vvar, IDT/GDT/TSS) are installed into each
 * new shadow_mm at allocation time so every mm's CR3 is a complete
 * walkable tree the moment kvm_enter_guest loads it.
 *
 * Lifecycle: kvm_mm_attach allocates + bootstraps; kvm_mm_detach
 * frees. The mm_id holds an opaque void* so other backends don't
 * need to pull in this header.
 */
/*
 * F12 mutation ring entry. One per shadow PTE event; circular
 * buffer per shadow_mm so on a fatal fault we can locate the
 * recent mutation history for cr2's VA.
 */
struct kvm_shadow_mut_entry {
	u64	addr;		/* VA being mutated */
	u64	ume;		/* new UML PTE value */
	u64	old_spte;	/* shadow leaf BEFORE this mutation */
	u64	new_spte;	/* shadow leaf AFTER this mutation */
	u8	action;		/* see KVM_SHADOW_MUT_* below */
	u8	pad[7];
};

#define KVM_SHADOW_MUT_INSTALL		1
#define KVM_SHADOW_MUT_CLEAR		2
#define KVM_SHADOW_MUT_ABSENT_PATH	3	/* no path to leaf, install deferred */
#define KVM_SHADOW_MUT_NOOP_ABSENT	4	/* absent on both sides */

#define KVM_SHADOW_MUT_RING_SIZE	256

/*
 * ============================================================
 * Shadow-mm dirty / synced / needs_full_resync state machine
 * (task #94 — documented to make the otherwise-subtle invariants
 *  visible at the structure definition).
 *
 * Two independent flags drive guest re-entry behaviour:
 *
 *   shadow->dirty
 *     Meaning: "the shadow PT contents have been mutated since the
 *     last guest-TLB flush, so the next KVM_RUN MUST run a CR3
 *     reload / CR4.PGE toggle to flush the guest TLB before re-
 *     entering ring-3."
 *     Producers (must SET dirty after the leaf write, with smp_wmb
 *     between the two so the consumer cannot observe dirty=false
 *     after seeing the new leaf):
 *       - lifecycle.c:kvm_shadow_map_page  (lazy fill leaf install)
 *       - lifecycle.c:kvm_shadow_invalidate_va_range (mm_unmap range
 *         clear; also resets synced=false)
 *       - lifecycle.c:kvm_shadow_pgd_clear_user (cross-mm switch hook)
 *       - lifecycle.c:kvm_shadow_fill_from_uml_pgd (clear pass with
 *         cleared > 0; install pass via map_page)
 *       - shadow_sync.c:kvm_shadow_sync_pte (direct sync from
 *         set_pte_at / pte_clear hooks: install / clear / alloc_fail
 *         paths)
 *     Consumer:
 *       - thread.c:kvm_enter_guest SREGS-skip predicate. Reads via
 *         smp_load_acquire(&shadow->dirty); if true, fall through
 *         to KVM_SET_SREGS with the CR4.PGE-toggle dance, then
 *         cmpxchg(true → false) ONLY the producer-snapshot we
 *         observed (preserves any writer that bumped it during the
 *         predicate window).
 *
 *   shadow->synced
 *     Meaning: "the shadow PT contains an exact mirror of mm->pgd
 *     (modulo bootstrap aliases) as of the last successful
 *     kvm_shadow_fill_from_uml_pgd, AND nothing has invalidated a
 *     range since."
 *     Producers (must SET synced=true ONLY at the end of a complete
 *     fill, after smp_wmb):
 *       - lifecycle.c:kvm_shadow_fill_from_uml_pgd (only on the
 *         success path; the seqlock check leaves it false if a
 *         direct-sync writer fired during the walk — task #90).
 *     Producers (must CLEAR synced=false on any non-fill mutation):
 *       - lifecycle.c:kvm_shadow_invalidate_va_range (range cleared)
 *       - lifecycle.c:kvm_shadow_mm_alloc (initial state)
 *     Consumer:
 *       - thread.c:kvm_enter_guest fill-skip predicate. Reads via
 *         smp_load_acquire(&shadow->synced); if true AND
 *         synced_pgd_va matches mm->pgd AND !needs_full_resync,
 *         skip the full pgd walk on this entry.
 *
 *   shadow->needs_full_resync
 *     Meaning: "a direct-sync writer hit a recoverable failure
 *     (intermediate-table allocation in atomic context) or the
 *     fill-vs-direct-sync seqlock detected a race; the next
 *     kvm_enter_guest MUST run a full fill to converge."
 *     Producers (set true; do NOT clear synced — that would force a
 *     fill on every entry from now on; the resync flag is a one-
 *     shot signal):
 *       - shadow_sync.c:kvm_shadow_sync_pte (alloc-fail path)
 *       - shadow_sync.c:kvm_shadow_sync_range_atomic (range too
 *         large for per-page sync)
 *       - lifecycle.c:kvm_shadow_fill_from_uml_pgd (task #90 seqlock
 *         miss — also clears synced=false so the next entry refills)
 *     Consumer:
 *       - thread.c:kvm_enter_guest. Reads via READ_ONCE; if true,
 *         force fill regardless of synced; cmpxchg(true → false)
 *         ONLY the producer-snapshot we observed at predicate time
 *         (so a concurrent writer's true is preserved).
 *
 * Ordering invariants (smp_wmb pairs):
 *
 *   Writer side: <leaf write> ; smp_wmb() ; WRITE_ONCE(dirty, true)
 *                <leaf write> ; smp_wmb() ; WRITE_ONCE(synced_pgd_va,
 *                                                      ...) ;
 *                                          WRITE_ONCE(synced, true)
 *
 *   Reader side: smp_load_acquire(&dirty)  pairs the wmb above so
 *                a true dirty observed here implies the leaf write
 *                is also visible.
 *                smp_load_acquire(&synced) likewise.
 *
 * Decision matrix for kvm_enter_guest at predicate time:
 *
 *   dirty | synced | needs_resync | action
 *   ------+--------+--------------+-----------------------------------
 *   false | true   | false        | skip-SREGS + skip-fill (fast path)
 *   true  | true   | false        | SREGS reload + skip-fill
 *   *     | false  | *            | SREGS reload + full fill
 *   *     | *      | true         | SREGS reload + full fill
 *
 * Consume-via-cmpxchg discipline: every clear of a flag MUST use
 * cmpxchg(true → false) keyed to the snapshot read at predicate
 * time. A bare WRITE_ONCE clear would race a concurrent producer
 * that sets the flag during our SREGS reload / fill, dropping the
 * signal.
 *
 * ============================================================
 */
struct kvm_shadow_mm {
	struct page		*pgd_page;	/* backing page for teardown */
	void			*pgd;		/* kernel VA of top-level PGD */
	u64			pgd_gpa;	/* __pa(pgd) → CR3 load value */
	/*
	 * Stage A.4d: per-shadow TLB-flush generation. Producers
	 * (kvm_shadow_sync_pte / kvm_shadow_invalidate_va_range /
	 * kvm_shadow_fill_from_uml_pgd / kvm_shadow_pgd_clear_user)
	 * atomically increment AFTER writing leaves (release semantics).
	 * Consumer (kvm_enter_guest) acquires it and compares to the
	 * per-vCPU vcpu->last_flushed_tlb_gen — mismatch triggers a
	 * CR4.PGE-toggle SREGS write to flush THIS vCPU's guest TLB.
	 *
	 * Init to 1 so that fresh vCPUs (last_flushed_tlb_gen = 0)
	 * always observe a mismatch on first entry and flush.
	 */
	atomic64_t		tlb_gen;
	/*
	 * Stage B-race fix (2026-04-27): adapt KVM's own
	 * mmu_invalidate_in_progress + mmu_invalidate_seq pattern (see
	 * virt/kvm/kvm_main.c:673-795 for reference).
	 *
	 * Producers bracket their leaf-write work with:
	 *     atomic_inc(&shadow->invalidate_in_progress);
	 *     ... write leaves ...
	 *     atomic64_inc(&shadow->invalidate_seq);
	 *     smp_wmb();
	 *     atomic_dec(&shadow->invalidate_in_progress);
	 *
	 * Consumer (kvm_enter_guest) before triggering KVM_RUN checks:
	 *     if (atomic_read(&shadow->invalidate_in_progress) ||
	 *         atomic64_read(&shadow->invalidate_seq) != saved_seq)
	 *         retry-fill;
	 *
	 * Closes the seq-coherency gap between producer leaf-write and
	 * consumer's KVM_RUN entry — even if a producer fires between
	 * the consumer's predicate read and KVM_RUN start, the seq
	 * mismatch is observed and the consumer re-fills.
	 */
	atomic_t		invalidate_in_progress;
	atomic64_t		invalidate_seq;
	bool			dirty;		/* DEPRECATED — kept for telemetry counters; consult tlb_gen for correctness */
	bool			synced;		/* shadow mirrors mm->pgd */
	u64			synced_pgd_va;	/* mm->pgd at last fill */
	struct mutex		fill_lock;	/* serializes pgd-walk fills */
	/*
	 * Memo 18 Phase 2: per-mm IRETQ-frame staging.
	 *
	 * The bootstrap IRETQ gadget pops {RIP, CS, RFLAGS, RSP, SS}
	 * from kregs.rsp on every kvm_enter_guest. Pre-Phase-2 the
	 * staging area was the singleton kvm_bootstrap_page_stack —
	 * shared across ALL mms, racy across cross-mm preemption
	 * windows. With per-mm storage each mm has its own frame
	 * page, eliminating cross-mm contamination structurally.
	 *
	 * Mapped at iretq_frame_va_guest in this shadow's PGD
	 * (kernel-half slot, never touched by user-half clear/fill
	 * passes). The host-side iretq_frame_va is the kernel VA we
	 * write the frame into; the guest CPU reads from
	 * iretq_frame_va_guest which the shadow PT maps to the
	 * matching GPA.
	 */
	struct page		*iretq_frame_page;	/* per-mm IRETQ frame page */
	void			*iretq_frame_va;	/* host kernel VA — write target */
	u64			iretq_frame_gpa;	/* GPA of frame page */
	u64			iretq_frame_va_guest;	/* guest VA — kregs.rsp */
	/*
	 * Memo 15 direct-shadow-sync: set when a per-PTE direct
	 * sync hit an allocation failure or other recoverable
	 * error. kvm_enter_guest's verifier path runs a full fill
	 * before KVM_RUN to repair, then clears the flag.
	 */
	bool			needs_full_resync;
	/*
	 * Memo 15 #6 mutation observability counters. Bumped via
	 * WRITE_ONCE from atomic-context shadow_sync_pte; readable
	 * via /proc or panic-time dump.
	 */
	u64			direct_sync_install;
	u64			direct_sync_clear;
	u64			direct_sync_absent;
	u64			direct_sync_alloc_fail;
	u64			direct_sync_range_clear;
	/*
	 * Task #94 transition counters. Bumped via WRITE_ONCE under
	 * the producer's existing ordering (no extra barriers).
	 * Readable via panic-time dump or ad-hoc /proc — exposes
	 * which producer drove a given fill / TLB-flush event so we
	 * can correlate gate failures with the dirty-state path that
	 * triggered them.
	 *
	 * Naming: <producer>_<flag>_set / <consumer>_<flag>_clear.
	 * Consumer is always kvm_enter_guest; producers are tagged.
	 */
	u64			dirty_set_fill_install;
	u64			dirty_set_fill_clearpass;
	u64			dirty_set_invalidate;
	u64			dirty_set_pgd_clear_user;
	u64			dirty_set_direct_sync;
	u64			dirty_clear_enter_guest;
	u64			synced_set_fill;
	u64			synced_clear_invalidate;
	u64			synced_clear_seqlock_miss;
	u64			needs_full_resync_set_alloc_fail;
	u64			needs_full_resync_set_range_too_large;
	u64			needs_full_resync_set_seqlock_miss;
	u64			needs_full_resync_clear_enter_guest;
	/*
	 * F12 mutation ring. Circular buffer of the last N mutations.
	 * head_seq is monotonically increasing; ring index =
	 * head_seq % KVM_SHADOW_MUT_RING_SIZE. Single-writer per mm;
	 * read concurrently for diagnostics.
	 */
	u64			mut_head_seq;
	struct kvm_shadow_mut_entry mut_ring[KVM_SHADOW_MUT_RING_SIZE];
};

struct kvm_shadow_mm *kvm_shadow_mm_alloc(void);
void kvm_shadow_mm_free(struct kvm_shadow_mm *shadow);

/*
 * Stage A.4d + Stage B-race fix: producer-side helper. Every shadow
 * leaf mutation calls this AFTER writing the leaf bytes.
 *
 *   - WRITE_ONCE(dirty=true): legacy telemetry boolean.
 *   - atomic64_inc(tlb_gen): per-vCPU TLB-flush trigger; paired with
 *     vcpu->last_flushed_tlb_gen in kvm_enter_guest.
 *   - atomic64_inc(invalidate_seq) + smp_wmb(): seq-coherency for
 *     the consumer's pre-KVM_RUN retry check (see kvm_enter_guest).
 *
 * Producers that have a meaningful "in_progress" window (range-
 * invalidate, fill_from_uml_pgd) bracket with kvm_shadow_invalidate_
 * begin/end below. Single-PTE writers (kvm_shadow_sync_pte) just
 * call this — the seq bump alone is sufficient because the leaf
 * write is atomic.
 */
static inline void kvm_shadow_mark_dirty(struct kvm_shadow_mm *shadow)
{
	if (!shadow)
		return;
	WRITE_ONCE(shadow->dirty, true);
	atomic64_inc(&shadow->tlb_gen);
	atomic64_inc(&shadow->invalidate_seq);
	smp_wmb();
}

/*
 * Bracket helpers for multi-leaf invalidation (range_invalidate,
 * fill clear+install). Pairs with the consumer's
 *
 *     if (atomic_read(&shadow->invalidate_in_progress)) retry;
 *     smp_rmb();
 *     if (atomic64_read(&shadow->invalidate_seq) != saved) retry;
 *
 * pattern in kvm_enter_guest. Adapted from KVM's own mmu_invalidate
 * pattern at virt/kvm/kvm_main.c:673.
 */
static inline void kvm_shadow_invalidate_begin(struct kvm_shadow_mm *shadow)
{
	if (shadow)
		atomic_inc(&shadow->invalidate_in_progress);
}

static inline void kvm_shadow_invalidate_end(struct kvm_shadow_mm *shadow)
{
	if (!shadow)
		return;
	atomic64_inc(&shadow->invalidate_seq);
	smp_wmb();
	atomic_dec(&shadow->invalidate_in_progress);
}

/*
 * Resolve the active mm's shadow tree. NULL when called outside a
 * task with a valid active_mm (e.g. very early boot or a kernel
 * thread that lost active_mm). Callers MUST handle NULL — the most
 * common defensive shape is "fall back to no-op" for invalidate
 * paths and "panic-style abort" for kvm_enter_guest's CR3 source.
 */
struct kvm_shadow_mm *kvm_shadow_mm_current(void);

/*
 * Shadow PT lifecycle (memo 09 step 1). Pre-#275 these operated on
 * the singleton kvm_um.shadow_pgd. Post-#275 they're vestigial
 * stubs that compile but do nothing (and pr_warn_once); the actual
 * allocation runs out of kvm_mm_attach. Kept to avoid churning
 * KUnit test prototypes that reference them.
 */
int kvm_shadow_pgd_alloc(void);
void kvm_shadow_pgd_free(void);

/*
 * Audit round-5 F6: zero the shadow PTEs that cover
 * [va_start, va_start + len) and mark the singleton shadow PGD
 * dirty so the next kvm_enter_guest triggers a CR3 reload (i.e.
 * a guest TLB flush). Range is rounded down to page boundaries
 * on the start side and up on the end side.
 *
 * Returns 0 on success, -ENODEV if the shadow PGD hasn't been
 * allocated yet, -EINVAL on NULL/zero len.
 */
int kvm_shadow_invalidate_va_range(struct kvm_shadow_mm *shadow,
				   u64 va_start, u64 len);

/*
 * #274 phase-1 keystone diagnostic. Walks the UML logical pgd at
 * `va` and the shadow PT at the same `va`, then logs both leaf
 * encodings and whether the shadow agrees with the UML view. `tag`
 * distinguishes call sites in the boot log.
 *
 * Returns 0 if shadow agrees with UML pgd, 1 if they diverge,
 * negative errno on missing inputs. Comparison ignores the A/D
 * status bits the CPU sets at runtime; the audit triggers on PFN,
 * P, RW, US, or NX drift, not on legitimate accessed/dirty churn.
 */
int kvm_shadow_audit_va(u64 va, void *uml_pgd_va, const char *tag);

/*
 * Task #91: three-way memory-content audit. Compares the bytes
 * visible at `va` through three paths:
 *   A — UML pgd → leaf PFN → __va  (UML kernel's own view)
 *   B — shadow PT → leaf GPA → __va (KVM's view via memslot)
 *   C — direct user-VA dereference  (raw host-PT view from os_map_memory)
 *
 * Reads `bytes` (capped to page boundary, max 64). Returns 0 if all
 * available paths agree, 1 if A==B but C diverges (host-VA aliasing),
 * 2 if A diverges from B (shadow GPA mismatch), -ENODEV if shadow or
 * pgd is unavailable.
 *
 * Diagnostic-only; safe in any context where the UML pgd and shadow
 * PT are stable. Does NOT take fill_lock.
 */
int kvm_shadow_audit_content_va(u64 va, void *uml_pgd_va, unsigned int bytes,
				const char *tag);

/*
 * #274 phase-1 step 2 diagnostic: full-pgd lockstep audit. Walks
 * every present leaf in the UML pgd, looks up the same VA in the
 * shadow PT, and counts (leaves, matches, diverges). Logs the
 * first up-to-`max_log` divergences in detail and a summary line.
 *
 * Used to test "shadow->synced cache is honest" on the cached-skip
 * path of kvm_enter_guest. Returns the number of divergences (>= 0)
 * on success, negative errno on missing inputs. Cost is O(pages-
 * mapped); intentionally observational — does not mutate shadow.
 */
int kvm_shadow_audit_pgd(void *uml_pgd_va, const char *tag,
			 unsigned int max_log);

/*
 * Audit round-6 G2: clear all leaf PTEs in the user half of the
 * singleton shadow PGD (canonical low half, PGD slots 0..255).
 * Kernel-half mappings (bootstrap data + code, gadget state, vvar)
 * persist across the call so subsequent kvm_enter_guest doesn't
 * have to reinstall them.
 *
 * Called from kvm_context_switch when the active mm changes:
 * without it, prev->mm's user mappings leak into next->mm's view
 * because kvm_shadow_fill_from_uml_pgd only INSTALLS present
 * leaves and doesn't clear absent ones. The cleared shadow PGD
 * gets refilled lazily on next entry / on demand via the #PF
 * handler.
 *
 * Marks kvm_ctx.shadow_dirty so the next kvm_enter_guest's
 * KVM_SET_SREGS triggers a guest TLB flush.
 */
void kvm_shadow_pgd_clear_user(void);

/*
 * Test hook: returns the current singleton shadow_pgd_gpa (or 0
 * if unallocated). KUnit assertion target; never used from the
 * run_userspace path.
 */
u64 kvm_shadow_pgd_gpa(void);

/*
 * Install a single 4 KiB mapping in the shadow PT: guest VA →
 * host-physical (as a gpa under the Policy A identity memslot).
 * Allocates intermediate PUD/PMD/PTE tables as needed using
 * alloc_page(GFP_KERNEL). `leaf_flags` is the x86-hardware-
 * encoded PTE flags word (KVM_X86_PTE_* constants below);
 * intermediate entries are always installed as
 * P|R/W|U/S|A to keep the walk permissive regardless of leaf
 * permissions. Memo 09 step 2.
 *
 * Returns 0 on success, -errno on failure. Caller holds
 * whatever serialization the shadow_pgd needs (today: no
 * concurrent callers; when SMP lands memo 09 revisits).
 */
int kvm_shadow_map_page(struct kvm_shadow_mm *shadow,
			u64 va, u64 phys_gpa, u64 leaf_flags);

/* Canonical x86_64 PTE bits for the shadow PT builder. */
#define KVM_X86_PTE_P	(1ULL << 0)
#define KVM_X86_PTE_RW	(1ULL << 1)
#define KVM_X86_PTE_US	(1ULL << 2)
#define KVM_X86_PTE_A	(1ULL << 5)
#define KVM_X86_PTE_D	(1ULL << 6)
#define KVM_X86_PTE_NX	(1ULL << 63)

/*
 * Translate UML's software-encoded PTE → x86 hardware PTE.
 * D66 finding: UML uses `_PAGE_RW = 0x020`, `_PAGE_USER =
 * 0x040`, `_PAGE_ACCESSED = 0x080`, `_PAGE_DIRTY = 0x100` —
 * all non-hardware positions. This helper maps each UML bit
 * to its x86 equivalent + preserves the PFN. Used by the
 * shadow-PT eager fill (memo 09 step 3) walking
 * current->active_mm->pgd to mirror logical mappings as
 * hardware-walkable entries.
 */
u64 kvm_um_pte_to_x86(u64 um_pte);

/*
 * Memo 09 step 3: walk `pgd` (UML's logical page table) and
 * install every present mapping into the shadow PT via
 * kvm_shadow_map_page. Bounded by the number of actually-
 * present pages (most pgd entries are empty). Returns the
 * number of leaf PTEs installed, or -errno on allocation
 * failure.
 *
 * Caller is responsible for serialisation; today there's no
 * concurrent access (single-threaded kvm_enter_guest).
 */
int kvm_shadow_fill_from_uml_pgd(struct kvm_shadow_mm *shadow, void *pgd);

/*
 * Accessor for the bootstrap host VA — used by kvm_shadow_fill_from_uml_pgd's
 * transactional clear pass to preserve any user-half alias. Post-A.4i
 * returns 0 since the guest-visible install lives in PML4[256+]; the
 * preservation becomes a no-op.
 */
u64 kvm_bootstrap_va_get(void);

/*
 * A.4i: GUEST VA where bootstrap pages are mapped via shadow PT.
 * Sits in PML4[508] (kernel-half), so user CPL=3 walks of low VAs
 * never reach it. Constant 0xffffe00000000000.
 */
u64 kvm_bootstrap_guest_va_get(void);

struct uml_pt_regs;
int kvm_enter_guest(struct uml_pt_regs *regs);

/*
 * Test hook: pure data-structure subset of kvm_enter_guest.
 * Fills caller-provided kvm_sregs + kvm_regs via the
 * production helpers without calling any ioctl. Exposed for
 * arch/um/backend/contract/ KUnit; never called from the
 * normal run_userspace path.
 */
struct kvm_sregs;
struct kvm_regs;
int kvm_enter_guest_probe(struct kvm_sregs *sregs, struct kvm_regs *regs,
			  const struct uml_pt_regs *src,
			  u64 cr3_gpa, u64 gdt_gpa);
int kvm_exit_guest_probe(struct uml_pt_regs *dst, const struct kvm_regs *src);

/*
 * Copy the LSTAR-trampoline bytes as they're written into the
 * bootstrap page into a caller-provided buffer. Used by the
 * A-05 KUnit tests to assert the trampoline wire-form matches
 * the harness path; never called from run_userspace.
 *
 * Returns the number of bytes copied (always the full trampoline
 * size if `len >= size`), or -EINVAL on NULL / undersized buffer.
 */
int kvm_bootstrap_copy_lstar(u8 *dst, size_t len);

/*
 * Test-only: force-allocate the bootstrap page so KUnit tests
 * can exercise the populated path (normally allocated lazily
 * on the first kvm_enter_guest call, which doesn't run in a
 * KUnit-at-boot context). Returns 0 on success, -errno on
 * failure. Idempotent.
 */
int kvm_bootstrap_force_init(void);

/*
 * Audit round-4 F2: pure-data probe wrapper around
 * kvm_build_sysret_r11. Exposed so the contract KUnit suite
 * can assert that a saved user RFLAGS round-trips through the
 * SYSRETQ R11 computation with arithmetic / direction / trap
 * flags intact and correctness-critical bits (IF, IOPL,
 * reserved-bit-1) forced on.
 */
u64 kvm_build_sysret_r11_probe(u64 saved_user_rflags);

/*
 * Audit round-5 F7/1: classify a faulting RIP as a user-memory-
 * writing gadget body and return the NR the gadget was servicing
 * (clock_gettime / time / getcpu), or -1 if the fault is elsewhere.
 * Used by the #PF recovery path to convert ring-0 gadget-mid-store
 * faults into SYSCALL fallbacks that surface -EFAULT per POSIX.
 */
int kvm_gadget_fault_nr(u64 fault_rip);

/*
 * UM KVM wire constants: port numbers the LSTAR trampoline uses
 * to signal exit reasons to the host. Matches the harness wire
 * format so sub-commit #3's decode can lift the harness paths
 * unchanged.
 */
#define UM_KVM_SYSCALL_PORT	0xf4	/* SYSCALL trap exit */
#define UM_KVM_SYSRETQ_PORT	0xf5	/* ring-3 exit (post-SYSRETQ) */

/*
 * Memo 11 G3 gadget state lifecycle. Alloc/free mirror
 * the shadow_pgd pair; both are lazy per-process under
 * the singleton-vCPU model (will become per-vCPU in
 * the SMP v2). No host-side syscall updates the state
 * synchronously; kvm_gadget_state_refresh() rewrites
 * the page from `current` right before each KVM_RUN,
 * keeping the channel trivially coherent for ncpus=1.
 *
 * Allocation failure is not fatal for the non-gadget
 * build (integrated without bench); the state page is
 * only consumed by the bench + future G4 handlers. For
 * builds that reference it, callers panic/fail in the
 * usual way.
 */
int kvm_gadget_state_alloc(void);
void kvm_gadget_state_free(void);
void kvm_gadget_state_refresh(void);
u64 kvm_gadget_state_va(void);
u64 kvm_gadget_state_gpa(void);

/*
 * Memo 11 G5 gadget vvar lifecycle. Same shape as the state
 * page above. refresh() is called from kvm_enter_guest right
 * before KVM_RUN — writes fresh CLOCK_MONOTONIC +
 * CLOCK_REALTIME timestamps under a seqlock so the G5c handler
 * asm reads a consistent pair.
 */
int kvm_gadget_vvar_alloc(void);
void kvm_gadget_vvar_free(void);
void kvm_gadget_vvar_refresh(void);
u64 kvm_gadget_vvar_va(void);
u64 kvm_gadget_vvar_gpa(void);

/*
 * Task #250 v2 ladder, memo 12: snapshot/forkserver primitives
 * for the KVM backend. Replaces the C-09 v1 fork()-based path
 * which doesn't compose with KVM's per-vCPU fd state.
 *
 * Lifecycle: alloc → capture → restore_full*N → destroy.
 * Today these provide the building blocks; integration into
 * um_snapshot_ready() lands in a follow-up commit (memo 12
 * step 3).
 */
struct kvm_snapshot;
struct kvm_snapshot *kvm_snapshot_alloc(void);
int kvm_snapshot_capture(struct kvm_snapshot *snap);
int kvm_snapshot_capture_regs_only(struct kvm_snapshot *snap);
int kvm_snapshot_restore_full(struct kvm_snapshot *snap);
void kvm_snapshot_free(struct kvm_snapshot *snap);
void kvm_snapshot_destroy(struct kvm_snapshot *snap);

/*
 * Task #253 / memo 13 record/replay primitives. Sits on top of
 * kvm_snapshot — a kvm_record holds one snapshot (the checkpoint)
 * plus a log of captured nondeterminism (TIME / RAND / INTERRUPT
 * / SYSCALL / MMIO_READ entries). Today this is the API skeleton;
 * dispatcher hooks land in follow-up commits per memo-13 ladder.
 */
struct kvm_record;
struct kvm_record *kvm_record_alloc(void);
int kvm_record_start(struct kvm_record *rec);
void kvm_record_stop(struct kvm_record *rec);
int kvm_record_replay(struct kvm_record *rec);
void kvm_record_destroy(struct kvm_record *rec);

/*
 * Dispatcher-side observation hook for class-A syscall returns.
 * Gated by static_branch_unlikely(&um_kvm_record_enabled) so non-
 * recording runtime pays zero cost. Called from kvm_decode_syscall
 * after handle_syscall produces a return value.
 */
extern struct static_key_false um_kvm_record_enabled;
void kvm_record_observe_syscall(unsigned long syscall_nr,
				long ret_value,
				u64 arg0_data, u64 arg1_data);
void kvm_record_observe_syscall_buf(unsigned long syscall_nr,
				    long ret_value,
				    u64 user_buf_va,
				    const void *payload,
				    size_t payload_len);
void kvm_record_observe_syscall_buf_meta(unsigned long syscall_nr,
					 long ret_value,
					 u64 user_buf_va,
					 const void *payload,
					 size_t payload_len,
					 const void *metadata,
					 size_t metadata_len,
					 u32 metadata_kind);

/*
 * Memo 13 P2 #13 metadata-buffer kinds. The replay-side
 * dispatcher uses the kind tag to decide how to copy_to_user
 * the metadata bytes back (sockaddr_storage at user_va vs iovec
 * scatter-gather etc).
 */
#define KVM_REPLAY_META_NONE		0
#define KVM_REPLAY_META_SOCKADDR	1
#define KVM_REPLAY_META_IOV		2
int kvm_record_consume_syscall(unsigned long syscall_nr,
			       long *ret_out,
			       u64 *user_buf_va_out,
			       const void **payload_out,
			       size_t *payload_len_out);
int kvm_record_consume_syscall_meta(unsigned long syscall_nr,
				    long *ret_out,
				    u64 *user_buf_va_out,
				    const void **payload_out,
				    size_t *payload_len_out,
				    const void **metadata_out,
				    size_t *metadata_len_out,
				    u32 *metadata_kind_out);
bool kvm_record_strict_replay(void);
int kvm_record_set_strict_replay(bool strict);
struct uml_pt_regs;
void kvm_record_observe_dispatch(unsigned long syscall_nr,
				 long ret_value,
				 const struct uml_pt_regs *regs);

#endif

/*
 * D-04b.1b diagnostic harness (arch/um/backend/kvm/harness.c).
 * Only compiled when CONFIG_UM_BACKEND_KVM_HARNESS=y. Panics
 * with KVM_RUN exit_reason — never returns. kvm_init() invokes
 * it after vCPU creation on harness builds.
 */
#ifdef CONFIG_UM_BACKEND_KVM_HARNESS
int kvm_run_harness(void);
#endif

#endif /* __ARCH_UM_BACKEND_KVM_H */
