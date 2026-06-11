/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_KVM_V2_RECORD_H
#define __ASM_UM_KVM_V2_RECORD_H

#include <linux/types.h>

struct kvm_v2_record;

struct kvm_v2_record *kvm_v2_record_active(void);
void kvm_v2_record_observe_time_travel(struct kvm_v2_record *rec,
				       u64 ns_at_advance);
int kvm_v2_record_consume_time_travel(struct kvm_v2_record *rec,
				      u64 *ns_out,
				      u64 *syscall_count_anchor_out);

#endif /* __ASM_UM_KVM_V2_RECORD_H */
