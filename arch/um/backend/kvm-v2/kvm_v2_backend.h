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

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <backend.h>

struct kvm_cpuid2;

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
	spinlock_t		lock;
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
 * Placeholder vCPU + CPUID install (memo 26 §A.3). Single instance for
 * Phase A.3; Phase C replaces it with a per-host-CPU pool. The vCPU
 * isn't actually run by any op yet — it just satisfies the "vCPU
 * exists + CPUID is installed" precondition for Phase B's memslot
 * work and Phase C/D's KVM_RUN dispatcher.
 */
struct kvm_v2_vcpu {
	int   vcpu_fd;
	void *kvm_run;
	u32   kvm_run_size;
};

int  kvm_v2_vcpu_create(struct kvm_v2_vm *vm);
void kvm_v2_vcpu_destroy(void);

#endif /* __ARCH_UM_BACKEND_KVM_V2_H */
