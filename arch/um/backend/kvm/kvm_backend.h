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
 * Test hook: returns the current singleton shadow_pgd_gpa (or 0
 * if unallocated). KUnit assertion target; never used from the
 * run_userspace path.
 */
u64 kvm_shadow_pgd_gpa(void);

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
 * UM KVM wire constants: port numbers the LSTAR trampoline uses
 * to signal exit reasons to the host. Matches the harness wire
 * format so sub-commit #3's decode can lift the harness paths
 * unchanged.
 */
#define UM_KVM_SYSCALL_PORT	0xf4	/* SYSCALL trap exit */
#define UM_KVM_SYSRETQ_PORT	0xf5	/* ring-3 exit (post-SYSRETQ) */
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
