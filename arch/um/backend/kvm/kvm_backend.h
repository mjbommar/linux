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
void kvm_setup_harness_sregs(struct kvm_sregs *sregs);

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
