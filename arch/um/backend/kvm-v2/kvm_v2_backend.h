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

#include <backend.h>

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

#endif /* __ARCH_UM_BACKEND_KVM_V2_H */
