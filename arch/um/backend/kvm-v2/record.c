// SPDX-License-Identifier: GPL-2.0
/*
 * KVM v2 record/replay core.
 *
 * This file owns the in-memory record container and the bounded syscall log
 * used by the experimental replay path. The live KVM syscall dispatcher
 * observes and consumes syscall entries from this core; snapshot, time, signal,
 * and device determinism remain outside the current supported surface.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <sysdep/ptrace.h>

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

#define KVM_V2_RECORD_DEFAULT_BUFFER	(64U * 1024U)
#define KVM_V2_RECORD_MAX_BUFFER	(64U * 1024U * 1024U)

DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled);
EXPORT_SYMBOL_GPL(um_kvm_v2_record_enabled);

static DEFINE_SPINLOCK(um_kvm_v2_record_lock);
static struct kvm_v2_record *um_kvm_v2_active_record;

void kvm_v2_record_set_gadget_bypass_page(void *gadget_state, bool on)
{
	u8 *flag;

	if (!gadget_state)
		return;

	flag = (u8 *)gadget_state + KVM_V2_GADGET_OFF_RECORD;
	WRITE_ONCE(*flag, on ? 1 : 0);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_set_gadget_bypass_page);

void kvm_v2_record_sync_gadget_bypass_page(void *gadget_state)
{
	bool enabled = static_branch_unlikely(&um_kvm_v2_record_enabled);

	kvm_v2_record_set_gadget_bypass_page(gadget_state, enabled);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_sync_gadget_bypass_page);

static void kvm_v2_record_set_gadget_bypass(bool on)
{
	int cpu;

	for (cpu = 0; cpu < KVM_V2_MAX_VCPUS; cpu++) {
		struct kvm_v2_vcpu *vcpu = kvm_v2_vcpu_get(cpu);

		if (vcpu)
			kvm_v2_record_set_gadget_bypass_page(vcpu->gadget_state_kva,
							     on);
	}
}

static void kvm_v2_record_reset_counters(struct kvm_v2_record *rec)
{
	rec->buffer_used = 0;
	rec->buffer_replayed = 0;
	rec->sequence = 0;
	rec->entries_recorded = 0;
	rec->entries_replayed = 0;
	rec->entries_dropped = 0;
	rec->syscall_count = 0;
}

struct kvm_v2_record *kvm_v2_record_alloc(size_t buffer_size)
{
	struct kvm_v2_record *rec;

	if (!buffer_size)
		buffer_size = KVM_V2_RECORD_DEFAULT_BUFFER;
	if (buffer_size > KVM_V2_RECORD_MAX_BUFFER)
		return NULL;

	rec = kzalloc_obj(*rec, GFP_KERNEL);
	if (!rec)
		return NULL;

	rec->buffer = kvmalloc(buffer_size, GFP_KERNEL);
	if (!rec->buffer) {
		kfree(rec);
		return NULL;
	}

	rec->buffer_size = buffer_size;
	rec->state = KVM_V2_RECORD_INIT;
	rec->strict_replay = true;
	mutex_init(&rec->lock);

	return rec;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_alloc);

static bool kvm_v2_record_disarm(struct kvm_v2_record *rec)
{
	unsigned long flags;
	bool was_active;

	spin_lock_irqsave(&um_kvm_v2_record_lock, flags);
	was_active = um_kvm_v2_active_record == rec;
	if (was_active)
		um_kvm_v2_active_record = NULL;
	spin_unlock_irqrestore(&um_kvm_v2_record_lock, flags);

	if (was_active) {
		static_branch_disable(&um_kvm_v2_record_enabled);
		kvm_v2_record_set_gadget_bypass(false);
	}

	return was_active;
}

void kvm_v2_record_destroy(struct kvm_v2_record *rec)
{
	if (!rec)
		return;

	kvm_v2_record_disarm(rec);
	kvfree(rec->buffer);
	mutex_destroy(&rec->lock);
	kfree(rec);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_destroy);

void kvm_v2_record_free(struct kvm_v2_record *rec)
{
	kvm_v2_record_destroy(rec);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_free);

static int kvm_v2_record_claim_active(struct kvm_v2_record *rec)
{
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&um_kvm_v2_record_lock, flags);
	if (um_kvm_v2_active_record && um_kvm_v2_active_record != rec)
		rc = -EBUSY;
	else
		um_kvm_v2_active_record = rec;
	spin_unlock_irqrestore(&um_kvm_v2_record_lock, flags);

	return rc;
}

int kvm_v2_record_start(struct kvm_v2_record *rec)
{
	int rc;

	if (!rec)
		return -EINVAL;

	mutex_lock(&rec->lock);
	if (rec->state == KVM_V2_RECORD_RECORDING ||
	    rec->state == KVM_V2_RECORD_REPLAYING) {
		rc = -EINVAL;
		goto out_unlock;
	}

	rc = kvm_v2_record_claim_active(rec);
	if (rc)
		goto out_unlock;

	kvm_v2_record_reset_counters(rec);
	rec->state = KVM_V2_RECORD_RECORDING;
	kvm_v2_record_set_gadget_bypass(true);
	static_branch_enable(&um_kvm_v2_record_enabled);

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_start);

int kvm_v2_record_stop(struct kvm_v2_record *rec)
{
	if (!rec)
		return -EINVAL;

	mutex_lock(&rec->lock);
	if (rec->state != KVM_V2_RECORD_RECORDING &&
	    rec->state != KVM_V2_RECORD_REPLAYING) {
		mutex_unlock(&rec->lock);
		return -EINVAL;
	}

	rec->state = KVM_V2_RECORD_STOPPED;
	mutex_unlock(&rec->lock);

	kvm_v2_record_disarm(rec);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_stop);

int kvm_v2_record_replay(struct kvm_v2_record *rec)
{
	int rc;

	if (!rec)
		return -EINVAL;

	mutex_lock(&rec->lock);
	if (rec->state != KVM_V2_RECORD_STOPPED) {
		rc = -EINVAL;
		goto out_unlock;
	}

	rc = kvm_v2_record_claim_active(rec);
	if (rc)
		goto out_unlock;

	rec->state = KVM_V2_RECORD_REPLAYING;
	rec->buffer_replayed = 0;
	rec->entries_replayed = 0;
	kvm_v2_record_set_gadget_bypass(true);
	static_branch_enable(&um_kvm_v2_record_enabled);

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_replay);

int kvm_v2_record_set_strict_replay(struct kvm_v2_record *rec, bool strict)
{
	if (!rec)
		return -EINVAL;

	mutex_lock(&rec->lock);
	rec->strict_replay = strict;
	mutex_unlock(&rec->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_set_strict_replay);

bool kvm_v2_record_strict_replay(const struct kvm_v2_record *rec)
{
	return rec && rec->strict_replay;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_strict_replay);

struct kvm_v2_record *kvm_v2_record_active(void)
{
	struct kvm_v2_record *rec;
	unsigned long flags;

	spin_lock_irqsave(&um_kvm_v2_record_lock, flags);
	rec = um_kvm_v2_active_record;
	spin_unlock_irqrestore(&um_kvm_v2_record_lock, flags);

	return rec;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_active);

void kvm_v2_record_observe_syscall(struct kvm_v2_record *rec,
				   unsigned long syscall_nr,
				   long ret_value,
				   const struct uml_pt_regs *regs)
{
	struct kvm_v2_replay_entry *entry;
	const size_t need = sizeof(*entry);

	if (!rec)
		return;

	mutex_lock(&rec->lock);
	if (rec->state != KVM_V2_RECORD_RECORDING)
		goto out_unlock;

	if (rec->buffer_used + need > rec->buffer_size) {
		rec->entries_dropped++;
		goto out_unlock;
	}

	entry = (struct kvm_v2_replay_entry *)
		((u8 *)rec->buffer + rec->buffer_used);
	memset(entry, 0, sizeof(*entry));
	entry->kind = KVM_V2_REPLAY_SYSCALL;
	entry->size = (u32)need;
	entry->sequence = ++rec->sequence;
	entry->syscall.nr = (s32)syscall_nr;
	entry->syscall.retval = (s64)ret_value;

	if (regs) {
		entry->syscall.args[0] = regs->gp[HOST_DI];
		entry->syscall.args[1] = regs->gp[HOST_SI];
		entry->syscall.args[2] = regs->gp[HOST_DX];
		entry->syscall.args[3] = regs->gp[HOST_R10];
		entry->syscall.args[4] = regs->gp[HOST_R8];
		entry->syscall.args[5] = regs->gp[HOST_R9];
	}

	rec->buffer_used += need;
	rec->entries_recorded++;
	rec->syscall_count++;

out_unlock:
	mutex_unlock(&rec->lock);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_observe_syscall);

int kvm_v2_record_consume_syscall(struct kvm_v2_record *rec,
				  unsigned long syscall_nr,
				  long *ret_value)
{
	struct kvm_v2_replay_entry *entry;
	size_t cursor;
	int rc = 0;

	if (!rec)
		return 0;

	mutex_lock(&rec->lock);
	if (rec->state != KVM_V2_RECORD_REPLAYING)
		goto out_unlock;

	cursor = rec->buffer_replayed;
	if (cursor >= rec->buffer_used) {
		rc = -ENODATA;
		goto out_unlock;
	}

	if (cursor + sizeof(*entry) > rec->buffer_used) {
		rc = -EILSEQ;
		goto out_unlock;
	}

	entry = (struct kvm_v2_replay_entry *)((u8 *)rec->buffer + cursor);
	if (entry->kind != KVM_V2_REPLAY_SYSCALL ||
	    entry->size != sizeof(*entry) ||
	    cursor + entry->size > rec->buffer_used ||
	    entry->syscall.nr != (s32)syscall_nr) {
		rc = -EILSEQ;
		goto out_unlock;
	}

	if (ret_value)
		*ret_value = (long)entry->syscall.retval;
	rec->buffer_replayed = cursor + entry->size;
	rec->entries_replayed++;
	rc = 1;

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_consume_syscall);
