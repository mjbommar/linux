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

#include <linux/refcount.h>
#include <backend.h>

/*
 * Single per-UML-process KVM context (decisions-log D57):
 * one /dev/kvm handle, one KVM_CREATE_VM fd, shared across every
 * UML guest mm. mm_refcount counts outstanding kvm_mm_attach()s
 * so a late shutdown during teardown doesn't close the fds from
 * under a still-attached mm. Fields are populated by kvm_init()
 * and stay read-only thereafter; mm.c is the only consumer of
 * the refcount.
 *
 * vcpu0_fd + run0 + run_size are the D-04a vCPU scaffold: one
 * vCPU sufficient for ncpus=1 UML (the default). SMP (ncpus>1)
 * wants one vCPU per UML CPU and moves creation to
 * thread_start_idle — tracked for D-05 in 04-ring-transition.md.
 */
struct kvm_um {
	int		kvm_fd;		/* /dev/kvm */
	int		vm_fd;		/* KVM_CREATE_VM */
	int		vcpu0_fd;	/* KVM_CREATE_VCPU, slot 0 */
	void		*run0;		/* mmap'd kvm_run for vcpu0 */
	size_t		run_size;	/* KVM_GET_VCPU_MMAP_SIZE */
	refcount_t	mm_refcount;	/* attached mm_ids */

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	/*
	 * Shadow page table (memo 09 step 1). Allocated in kvm_init
	 * for KVM_INTEGRATED builds; this is a singleton per-process
	 * PGD (ncpus=1 friendly — one active mm at a time). A
	 * future per-mm variant lives in struct mm_id's per-backend
	 * bookkeeping once memo 09 step 2 needs it.
	 *
	 *   shadow_pgd_page — backing `struct page *` for teardown.
	 *   shadow_pgd      — kernel VA of the 4 KiB top-level PGD.
	 *   shadow_pgd_gpa  — __pa(shadow_pgd) for loading into CR3.
	 *
	 * Zero-initialised at allocation. Fill-in happens in memo 09
	 * step 2 (eager bootstrap mapping) + step 3 (lazy fault-in
	 * from KVM_EXIT_MMIO). Empty pgd at this stage is expected;
	 * kvm_enter_guest still loads current->active_mm->pgd as CR3
	 * until step 2 flips that wire.
	 */
	struct page	*shadow_pgd_page;
	void		*shadow_pgd;
	u64		shadow_pgd_gpa;

	/*
	 * Audit round-5 F6: shadow-PT invalidation state. Set
	 * whenever a PTE in the shadow PGD is cleared or
	 * overwritten from non-vCPU context (mm_unmap / mm_map
	 * from UML's mm layer). Cleared by kvm_enter_guest after
	 * issuing a CR3 reload via KVM_SET_SREGS, which forces
	 * the vCPU to flush its TLB on the next KVM_RUN per the
	 * AMD64 SDM's "writes to CR3 flush non-global TLB
	 * entries" architectural guarantee.
	 *
	 * Without this flag, stale TLB entries could survive a
	 * mm_unmap and the guest would observe the old mapping
	 * until its next spontaneous TLB flush. The singleton
	 * shadow PGD model means there's only one CR3 that
	 * matters, so a single bool suffices.
	 */
	bool		shadow_dirty;

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
 */
int kvm_backend_fd(void);
int kvm_backend_vm_fd(void);
int kvm_backend_vcpu0_fd(void);
struct kvm_um *kvm_backend_ctx(void);

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
	u64 _pad2[3];			/* cache-line pad (64 B struct) */
};

#define KVM_VVAR_OFF_SEQ		0x00
#define KVM_VVAR_OFF_MONO_SEC		0x08
#define KVM_VVAR_OFF_MONO_NSEC		0x10
#define KVM_VVAR_OFF_REAL_SEC		0x18
#define KVM_VVAR_OFF_REAL_NSEC		0x20
#define KVM_VVAR_OFF_BUDGET		0x28

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
/*
 * Shadow PT lifecycle (memo 09 step 1). Called by kvm_init /
 * kvm_shutdown; returns 0 on success, -errno on failure.
 * Callers panic on failure during init since an unallocatable
 * shadow PT is a bring-up bug, not a runtime error.
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
int kvm_shadow_invalidate_va_range(u64 va_start, u64 len);

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
int kvm_shadow_map_page(u64 va, u64 phys_gpa, u64 leaf_flags);

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
int kvm_shadow_fill_from_uml_pgd(void *pgd);

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
