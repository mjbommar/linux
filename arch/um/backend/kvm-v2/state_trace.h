/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_KVM_V2_STATE_TRACE_H
#define __UM_KVM_V2_STATE_TRACE_H

#include <linux/types.h>

enum kvm_v2_state_trace_op {
	KVM_V2_STATE_TRACE_RUN_ENTER = 1,
	KVM_V2_STATE_TRACE_RUN_EXIT,
	KVM_V2_STATE_TRACE_RUN_EINTR,
};

#ifdef CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE

#include <linux/jump_label.h>

DECLARE_STATIC_KEY_FALSE(kvm_v2_state_trace_key);

void kvm_v2_state_trace_record(enum kvm_v2_state_trace_op op, int cpu,
			       u32 exit_reason, u16 io_port, unsigned long rip,
			       unsigned long sp, unsigned long ax);

#define kvm_v2_state_trace(op, cpu, exit_reason, io_port, rip, sp, ax)	\
do {									\
	if (static_branch_unlikely(&kvm_v2_state_trace_key))		\
		kvm_v2_state_trace_record((op), (cpu), (exit_reason),	\
					  (io_port), (rip), (sp), (ax));	\
} while (0)

#else /* !CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE */

static inline void
kvm_v2_state_trace_record(enum kvm_v2_state_trace_op op, int cpu,
			  u32 exit_reason, u16 io_port, unsigned long rip,
			  unsigned long sp, unsigned long ax)
{
}

#define kvm_v2_state_trace(op, cpu, exit_reason, io_port, rip, sp, ax)	\
do {									\
	if (0)								\
		kvm_v2_state_trace_record((op), (cpu), (exit_reason),	\
					  (io_port), (rip), (sp), (ax));	\
} while (0)

#endif /* CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE */

#endif /* __UM_KVM_V2_STATE_TRACE_H */
