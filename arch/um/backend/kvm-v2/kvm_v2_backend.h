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
struct mm_struct;
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

/*
 * Phase B.5: load guest CR3. Caller passes __pa(mm->pgd). No
 * production caller until Phase C wires task->vCPU dispatch; B.5
 * lands the helper and the SREGS read/write plumbing for review.
 */
int  kvm_v2_load_cr3(unsigned long pgd);

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
