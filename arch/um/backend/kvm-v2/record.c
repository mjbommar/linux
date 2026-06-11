// SPDX-License-Identifier: GPL-2.0
/*
 * KVM v2 record/replay core.
 *
 * This file owns the in-memory record container and the bounded syscall log
 * used by the experimental replay path. The live KVM syscall dispatcher
 * observes and consumes syscall entries from this core; snapshot-backed record
 * start is present, but the supported replay surface remains deliberately
 * bounded by explicit syscall, time, signal, and external-I/O policy.
 */

#include <linux/errno.h>
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <asm/unistd.h>
#include <asm/um-hooks.h>
#include <sysdep/ptrace.h>

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

#define KVM_V2_RECORD_DEFAULT_BUFFER	(64U * 1024U)
#define KVM_V2_RECORD_MAX_BUFFER	(64U * 1024U * 1024U)
#define KVM_V2_RECORD_CTL_MAX		96

DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled);
EXPORT_SYMBOL_GPL(um_kvm_v2_record_enabled);

static DEFINE_SPINLOCK(um_kvm_v2_record_lock);
static struct kvm_v2_record *um_kvm_v2_active_record;

const char *kvm_v2_record_state_name(enum kvm_v2_record_state state)
{
	switch (state) {
	case KVM_V2_RECORD_INIT:
		return "init";
	case KVM_V2_RECORD_RECORDING:
		return "recording";
	case KVM_V2_RECORD_STOPPED:
		return "stopped";
	case KVM_V2_RECORD_REPLAYING:
		return "replaying";
	default:
		return "unknown";
	}
}
EXPORT_SYMBOL_GPL(kvm_v2_record_state_name);

bool kvm_v2_record_syscall_has_payload(unsigned long syscall_nr)
{
	switch (syscall_nr) {
	case __NR_getcwd:
	case __NR_uname:
		return true;
	default:
		return false;
	}
}
EXPORT_SYMBOL_GPL(kvm_v2_record_syscall_has_payload);

bool kvm_v2_record_syscall_supported(unsigned long syscall_nr)
{
	switch (syscall_nr) {
	case __NR_getpid:
	case __NR_getppid:
	case __NR_gettid:
		return true;
	default:
		return kvm_v2_record_syscall_has_payload(syscall_nr);
	}
}
EXPORT_SYMBOL_GPL(kvm_v2_record_syscall_supported);

int kvm_v2_record_check_strict_syscall(struct kvm_v2_record *rec,
				       unsigned long syscall_nr)
{
	bool strict;

	if (!rec || kvm_v2_record_syscall_supported(syscall_nr))
		return 0;

	mutex_lock(&rec->lock);
	strict = rec->strict_replay;
	mutex_unlock(&rec->lock);
	if (!strict)
		return 0;

	kvm_v2_record_note_replay_failure(rec, syscall_nr, -EOPNOTSUPP);
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_check_strict_syscall);

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
	rec->first_syscall_pid = 0;
	rec->last_syscall_pid = 0;
	rec->syscalls_from_snapshot_task = 0;
	rec->syscalls_from_other_tasks = 0;
	rec->payload_entries_recorded = 0;
	rec->payload_entries_replayed = 0;
	rec->payload_bytes_recorded = 0;
	rec->payload_bytes_replayed = 0;
	rec->strict_replay_failures = 0;
	rec->last_replay_failure_syscall = -1;
	rec->last_replay_failure_rc = 0;
	rec->suppress_next_syscall = false;
	rec->suppress_syscall_pid = 0;
	rec->suppress_syscall_nr = 0;
}

static void kvm_v2_record_suppress_next_syscall(struct kvm_v2_record *rec,
						unsigned long syscall_nr)
{
	if (!rec)
		return;

	mutex_lock(&rec->lock);
	rec->suppress_next_syscall = true;
	rec->suppress_syscall_pid = current->pid;
	rec->suppress_syscall_nr = syscall_nr;
	mutex_unlock(&rec->lock);
}

static bool kvm_v2_record_should_suppress_syscall_locked(struct kvm_v2_record *rec,
							 unsigned long syscall_nr)
{
	if (!rec->suppress_next_syscall ||
	    rec->suppress_syscall_pid != current->pid)
		return false;

	rec->suppress_next_syscall = false;
	rec->suppress_syscall_pid = 0;

	if (rec->suppress_syscall_nr != syscall_nr) {
		rec->suppress_syscall_nr = 0;
		return false;
	}

	rec->suppress_syscall_nr = 0;
	return true;
}

static void kvm_v2_record_release_snapshot_locked(struct kvm_v2_record *rec)
{
	kvm_v2_snapshot_destroy(rec->snapshot);
	rec->snapshot = NULL;
	rec->snapshot_attempted = false;
	rec->snapshot_valid = false;
	rec->snapshot_rc = 0;
}

int kvm_v2_record_reset(struct kvm_v2_record *rec)
{
	int rc = 0;

	if (!rec)
		return -EINVAL;

	mutex_lock(&rec->lock);
	if (rec->state == KVM_V2_RECORD_RECORDING ||
	    rec->state == KVM_V2_RECORD_REPLAYING) {
		rc = -EBUSY;
		goto out_unlock;
	}

	kvm_v2_record_reset_counters(rec);
	kvm_v2_record_release_snapshot_locked(rec);
	rec->state = KVM_V2_RECORD_INIT;
	rec->strict_replay = true;

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_reset);

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
		static_branch_disable(&um_hook_record_replay);
		kvm_v2_record_set_gadget_bypass(false);
	}

	return was_active;
}

void kvm_v2_record_destroy(struct kvm_v2_record *rec)
{
	if (!rec)
		return;

	kvm_v2_record_disarm(rec);
	mutex_lock(&rec->lock);
	kvm_v2_record_release_snapshot_locked(rec);
	mutex_unlock(&rec->lock);
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

static void kvm_v2_record_unclaim_active(struct kvm_v2_record *rec)
{
	unsigned long flags;

	spin_lock_irqsave(&um_kvm_v2_record_lock, flags);
	if (um_kvm_v2_active_record == rec)
		um_kvm_v2_active_record = NULL;
	spin_unlock_irqrestore(&um_kvm_v2_record_lock, flags);
}

static int __kvm_v2_record_start(struct kvm_v2_record *rec,
				 struct kvm_v2_snapshot *snapshot,
				 bool snapshot_attempted)
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
	kvm_v2_record_release_snapshot_locked(rec);
	rec->snapshot = snapshot;
	rec->snapshot_attempted = snapshot_attempted;
	rec->snapshot_valid = !!snapshot;
	rec->snapshot_rc = 0;
	rec->state = KVM_V2_RECORD_RECORDING;
	kvm_v2_record_set_gadget_bypass(true);
	static_branch_enable(&um_kvm_v2_record_enabled);
	static_branch_enable(&um_hook_record_replay);

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}

int kvm_v2_record_start(struct kvm_v2_record *rec)
{
	return __kvm_v2_record_start(rec, NULL, false);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_start);

static void kvm_v2_record_note_snapshot_failure(struct kvm_v2_record *rec,
						int rc)
{
	if (!rec)
		return;

	mutex_lock(&rec->lock);
	if (rec->state != KVM_V2_RECORD_RECORDING &&
	    rec->state != KVM_V2_RECORD_REPLAYING) {
		kvm_v2_record_release_snapshot_locked(rec);
		rec->snapshot_attempted = true;
		rec->snapshot_valid = false;
		rec->snapshot_rc = rc;
	}
	mutex_unlock(&rec->lock);
}

int kvm_v2_record_start_with_snapshot(struct kvm_v2_record *rec)
{
	struct kvm_v2_snapshot *snapshot;
	int rc;

	if (!rec)
		return -EINVAL;

	snapshot = kvm_v2_snapshot_alloc();
	if (!snapshot) {
		kvm_v2_record_note_snapshot_failure(rec, -ENOMEM);
		return -ENOMEM;
	}

	rc = kvm_v2_snapshot_capture_task(snapshot, NULL);
	if (rc < 0) {
		kvm_v2_snapshot_destroy(snapshot);
		kvm_v2_record_note_snapshot_failure(rec, rc);
		return rc;
	}

	rc = __kvm_v2_record_start(rec, snapshot, true);
	if (rc < 0)
		kvm_v2_snapshot_destroy(snapshot);

	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_start_with_snapshot);

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

	if (rec->snapshot_valid) {
		rc = kvm_v2_snapshot_restore_task(rec->snapshot, NULL);
		if (rc < 0) {
			rec->snapshot_rc = rc;
			kvm_v2_record_unclaim_active(rec);
			goto out_unlock;
		}
		rec->snapshot_rc = 0;
	}

	rec->state = KVM_V2_RECORD_REPLAYING;
	rec->buffer_replayed = 0;
	rec->entries_replayed = 0;
	kvm_v2_record_set_gadget_bypass(true);
	static_branch_enable(&um_kvm_v2_record_enabled);
	static_branch_enable(&um_hook_record_replay);

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

bool kvm_v2_record_replay_active(void)
{
	struct kvm_v2_record *rec = kvm_v2_record_active();

	return rec && READ_ONCE(rec->state) == KVM_V2_RECORD_REPLAYING;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_replay_active);

bool kvm_v2_record_finish_replay_if_complete(struct kvm_v2_record *rec)
{
	bool complete = false;

	if (!rec)
		return false;

	mutex_lock(&rec->lock);
	if (rec->state == KVM_V2_RECORD_REPLAYING &&
	    rec->buffer_replayed >= rec->buffer_used) {
		rec->state = KVM_V2_RECORD_STOPPED;
		complete = true;
	}
	mutex_unlock(&rec->lock);

	if (complete)
		kvm_v2_record_disarm(rec);

	return complete;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_finish_replay_if_complete);

static void kvm_v2_record_fill_syscall_args(u64 args[6],
					    const struct uml_pt_regs *regs)
{
	memset(args, 0, sizeof(u64) * 6);
	if (!regs)
		return;

	args[0] = regs->gp[HOST_DI];
	args[1] = regs->gp[HOST_SI];
	args[2] = regs->gp[HOST_DX];
	args[3] = regs->gp[HOST_R10];
	args[4] = regs->gp[HOST_R8];
	args[5] = regs->gp[HOST_R9];
}

static bool kvm_v2_record_payload_args_match(const u64 args[6],
					     const struct uml_pt_regs *regs,
					     unsigned long syscall_nr,
					     unsigned int payload_arg)
{
	u64 replay_args[6];
	unsigned int i;

	if (!regs)
		return true;
	if (payload_arg >= 6)
		return false;

	kvm_v2_record_fill_syscall_args(replay_args, regs);

	switch (syscall_nr) {
	case __NR_uname:
		return true;
	case __NR_getcwd:
		return args[1] == replay_args[1];
	default:
		break;
	}

	for (i = 0; i < 6; i++) {
		if (i == payload_arg)
			continue;
		if (args[i] != replay_args[i])
			return false;
	}
	return true;
}

static void kvm_v2_record_account_syscall_locked(struct kvm_v2_record *rec)
{
	rec->syscall_count++;
	if (!rec->first_syscall_pid)
		rec->first_syscall_pid = current->pid;
	rec->last_syscall_pid = current->pid;
	if (rec->snapshot_valid && rec->snapshot &&
	    current->pid == rec->snapshot->task_source_pid)
		rec->syscalls_from_snapshot_task++;
	else if (rec->snapshot_valid)
		rec->syscalls_from_other_tasks++;
}

static size_t kvm_v2_record_payload_entry_size(size_t payload_len)
{
	return ALIGN(sizeof(struct kvm_v2_replay_entry) + payload_len,
		     sizeof(u64));
}

static void kvm_v2_record_init_entry(struct kvm_v2_replay_entry *entry,
				     u32 kind, u32 size, u64 sequence)
{
	entry->kind = kind;
	entry->size = size;
	entry->sequence = sequence;
	entry->version = KVM_V2_RECORD_FORMAT_VERSION;
	entry->flags = KVM_V2_RECORD_ENTRY_F_NONE;
}

static bool kvm_v2_record_entry_matches(const struct kvm_v2_replay_entry *entry,
					u32 kind)
{
	return entry->kind == kind &&
	       entry->version == KVM_V2_RECORD_FORMAT_VERSION &&
	       entry->flags == KVM_V2_RECORD_ENTRY_F_NONE;
}

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
	if (kvm_v2_record_should_suppress_syscall_locked(rec, syscall_nr))
		goto out_unlock;

	if (rec->buffer_used + need > rec->buffer_size) {
		rec->entries_dropped++;
		goto out_unlock;
	}

	entry = (struct kvm_v2_replay_entry *)
		((u8 *)rec->buffer + rec->buffer_used);
	memset(entry, 0, sizeof(*entry));
	kvm_v2_record_init_entry(entry, KVM_V2_REPLAY_SYSCALL, (u32)need,
				 ++rec->sequence);
	entry->syscall.nr = (s32)syscall_nr;
	entry->syscall.retval = (s64)ret_value;
	kvm_v2_record_fill_syscall_args(entry->syscall.args, regs);

	rec->buffer_used += need;
	rec->entries_recorded++;
	kvm_v2_record_account_syscall_locked(rec);

out_unlock:
	mutex_unlock(&rec->lock);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_observe_syscall);

int kvm_v2_record_observe_syscall_payload(struct kvm_v2_record *rec,
					  unsigned long syscall_nr,
					  long ret_value,
					  const struct uml_pt_regs *regs,
					  unsigned int arg_index,
					  const void *payload,
					  size_t payload_len)
{
	struct kvm_v2_replay_entry *entry;
	size_t need;
	int rc = 0;

	if (!rec || arg_index >= 6 || (!payload && payload_len))
		return -EINVAL;
	if (payload_len > KVM_V2_RECORD_MAX_PAYLOAD)
		return -E2BIG;

	need = kvm_v2_record_payload_entry_size(payload_len);

	mutex_lock(&rec->lock);
	if (rec->state != KVM_V2_RECORD_RECORDING)
		goto out_unlock;

	if (rec->buffer_used + need > rec->buffer_size) {
		rec->entries_dropped++;
		rc = -ENOSPC;
		goto out_unlock;
	}

	entry = (struct kvm_v2_replay_entry *)
		((u8 *)rec->buffer + rec->buffer_used);
	memset(entry, 0, need);
	kvm_v2_record_init_entry(entry, KVM_V2_REPLAY_SYSCALL_PAYLOAD,
				 (u32)need, ++rec->sequence);
	entry->syscall_payload.nr = (s32)syscall_nr;
	entry->syscall_payload.arg_index = arg_index;
	entry->syscall_payload.retval = (s64)ret_value;
	kvm_v2_record_fill_syscall_args(entry->syscall_payload.args, regs);
	entry->syscall_payload.payload_len = (u32)payload_len;
	if (payload_len)
		memcpy((u8 *)entry + sizeof(*entry), payload, payload_len);

	rec->buffer_used += need;
	rec->entries_recorded++;
	rec->payload_entries_recorded++;
	rec->payload_bytes_recorded += payload_len;
	kvm_v2_record_account_syscall_locked(rec);
	rc = 1;

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_observe_syscall_payload);

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
	if (!kvm_v2_record_entry_matches(entry, KVM_V2_REPLAY_SYSCALL) ||
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

int kvm_v2_record_consume_syscall_payload(struct kvm_v2_record *rec,
					  unsigned long syscall_nr,
					  const struct uml_pt_regs *regs,
					  long *ret_value,
					  void *payload,
					  size_t payload_size,
					  size_t *payload_len_out)
{
	struct kvm_v2_replay_entry *entry;
	size_t payload_len;
	size_t cursor;
	int rc = 0;

	if (!rec || !payload)
		return 0;
	if (payload_len_out)
		*payload_len_out = 0;

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
	if (!kvm_v2_record_entry_matches(entry, KVM_V2_REPLAY_SYSCALL_PAYLOAD) ||
	    entry->size < sizeof(*entry) ||
	    cursor + entry->size > rec->buffer_used ||
	    entry->syscall_payload.nr != (s32)syscall_nr ||
	    entry->syscall_payload.arg_index >= 6 ||
	    !kvm_v2_record_payload_args_match(entry->syscall_payload.args,
					      regs, syscall_nr,
					      entry->syscall_payload.arg_index)) {
		rc = -EILSEQ;
		goto out_unlock;
	}

	payload_len = entry->syscall_payload.payload_len;
	if (payload_len > entry->size - sizeof(*entry)) {
		rc = -EILSEQ;
		goto out_unlock;
	}
	if (payload_len > payload_size) {
		rc = -ENOSPC;
		goto out_unlock;
	}

	if (payload_len)
		memcpy(payload, (u8 *)entry + sizeof(*entry), payload_len);
	if (payload_len_out)
		*payload_len_out = payload_len;
	if (ret_value)
		*ret_value = (long)entry->syscall_payload.retval;

	rec->buffer_replayed = cursor + entry->size;
	rec->entries_replayed++;
	rec->payload_entries_replayed++;
	rec->payload_bytes_replayed += payload_len;
	rc = 1;

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_consume_syscall_payload);

void kvm_v2_record_note_replay_failure(struct kvm_v2_record *rec,
				       unsigned long syscall_nr, int rc)
{
	if (!rec)
		return;

	mutex_lock(&rec->lock);
	if (rec->state == KVM_V2_RECORD_REPLAYING) {
		rec->strict_replay_failures++;
		rec->last_replay_failure_syscall = (long)syscall_nr;
		rec->last_replay_failure_rc = rc;
	}
	mutex_unlock(&rec->lock);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_note_replay_failure);

void kvm_v2_record_observe_time_travel(struct kvm_v2_record *rec,
				       u64 ns_at_advance)
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
	kvm_v2_record_init_entry(entry, KVM_V2_REPLAY_TIME_TRAVEL,
				 (u32)need, ++rec->sequence);
	entry->time_travel.ns_at_advance = ns_at_advance;
	entry->time_travel.syscall_count_anchor = rec->syscall_count;

	rec->buffer_used += need;
	rec->entries_recorded++;

out_unlock:
	mutex_unlock(&rec->lock);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_observe_time_travel);

int kvm_v2_record_consume_time_travel(struct kvm_v2_record *rec,
				      u64 *ns_out,
				      u64 *syscall_count_anchor_out)
{
	struct kvm_v2_replay_entry *entry;
	size_t cursor;
	int rc = 0;

	if (!rec || !ns_out)
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
	if (!kvm_v2_record_entry_matches(entry, KVM_V2_REPLAY_TIME_TRAVEL) ||
	    entry->size != sizeof(*entry) ||
	    cursor + entry->size > rec->buffer_used) {
		rc = -EILSEQ;
		goto out_unlock;
	}

	*ns_out = entry->time_travel.ns_at_advance;
	if (syscall_count_anchor_out)
		*syscall_count_anchor_out =
			entry->time_travel.syscall_count_anchor;
	rec->buffer_replayed = cursor + entry->size;
	rec->entries_replayed++;
	rc = 1;

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_consume_time_travel);

#define KVM_V2_RECORD_CLOCK_BENCH_N_MAX	4096U

static int kvm_v2_record_clock_bench_run(unsigned int n)
{
	struct kvm_v2_record *rec;
	u64 entries_recorded = 0;
	u64 entries_replayed = 0;
	u64 mismatches = 0;
	u64 base_ns = 1000;
	unsigned int i;
	bool pass;
	int rc;

	rec = kvm_v2_record_alloc((size_t)n *
				  sizeof(struct kvm_v2_replay_entry));
	if (!rec) {
		pr_warn("um: kvm-v2 record clock bench: alloc failed\n");
		return -ENOMEM;
	}

	rc = kvm_v2_record_start(rec);
	if (rc < 0) {
		pr_warn("um: kvm-v2 record clock bench: start rc=%d\n", rc);
		goto out_report;
	}

	for (i = 0; i < n; i++)
		um_on_time_travel_advance(base_ns + (u64)i * 1000);

	entries_recorded = rec->entries_recorded;
	rc = kvm_v2_record_stop(rec);
	if (rc < 0) {
		pr_warn("um: kvm-v2 record clock bench: stop rc=%d\n", rc);
		goto out_report;
	}

	rc = kvm_v2_record_replay(rec);
	if (rc < 0) {
		pr_warn("um: kvm-v2 record clock bench: replay rc=%d\n", rc);
		goto out_report;
	}

	for (i = 0; i < n; i++) {
		u64 expected = base_ns + (u64)i * 1000;
		u64 observed = 0;

		if (!um_time_travel_consume_replay(&observed))
			break;
		if (observed != expected)
			mismatches++;
	}

	entries_replayed = rec->entries_replayed;
	(void)kvm_v2_record_stop(rec);

out_report:
	pass = entries_recorded == n && entries_replayed == n &&
	       mismatches == 0;
	pr_info("um: kvm-v2 record clock bench: N=%u observed=%llu replayed=%llu mismatches=%llu verdict=%s\n",
		n, entries_recorded, entries_replayed, mismatches,
		pass ? "PASS" : "FAIL");

	kvm_v2_record_destroy(rec);
	return pass ? 0 : (rc < 0 ? rc : -EIO);
}

static unsigned int kvm_v2_record_clock_bench_n;

static int __init kvm_v2_record_clock_bench_setup(char *str)
{
	unsigned int n;

	if (!str || kstrtouint(str, 10, &n) < 0)
		return 1;
	if (!n || n > KVM_V2_RECORD_CLOCK_BENCH_N_MAX)
		return 1;

	kvm_v2_record_clock_bench_n = n;
	return 1;
}
__setup("kvm_v2_record_clock_bench=", kvm_v2_record_clock_bench_setup);

static int __init kvm_v2_record_clock_bench_late_init(void)
{
	if (!kvm_v2_record_clock_bench_n)
		return 0;

	(void)kvm_v2_record_clock_bench_run(kvm_v2_record_clock_bench_n);
	return 0;
}
late_initcall_sync(kvm_v2_record_clock_bench_late_init);

#ifdef CONFIG_DEBUG_FS

static DEFINE_MUTEX(kvm_v2_record_debugfs_lock);
static struct kvm_v2_record *kvm_v2_record_debugfs_rec;

static int kvm_v2_record_debugfs_ensure(size_t buffer_size)
{
	if (kvm_v2_record_debugfs_rec) {
		if (buffer_size &&
		    buffer_size > kvm_v2_record_debugfs_rec->buffer_size)
			return -E2BIG;
		return 0;
	}

	kvm_v2_record_debugfs_rec = kvm_v2_record_alloc(buffer_size);
	if (!kvm_v2_record_debugfs_rec)
		return -ENOMEM;

	return 0;
}

static int kvm_v2_record_parse_size(char *arg, size_t *buffer_size)
{
	unsigned long long bytes;
	int rc;

	arg = skip_spaces(arg);
	if (!*arg) {
		*buffer_size = 0;
		return 0;
	}

	rc = kstrtoull(arg, 0, &bytes);
	if (rc < 0)
		return rc;
	if (!bytes || bytes > KVM_V2_RECORD_MAX_BUFFER)
		return -EINVAL;

	*buffer_size = (size_t)bytes;
	return 0;
}

static int kvm_v2_record_parse_bool(char *arg, bool *value)
{
	arg = skip_spaces(arg);

	if (!strcmp(arg, "0") || !strcmp(arg, "false") || !strcmp(arg, "off")) {
		*value = false;
		return 0;
	}
	if (!strcmp(arg, "1") || !strcmp(arg, "true") || !strcmp(arg, "on")) {
		*value = true;
		return 0;
	}

	return -EINVAL;
}

static int kvm_v2_record_debugfs_destroy(void)
{
	struct kvm_v2_record *rec = kvm_v2_record_debugfs_rec;
	enum kvm_v2_record_state state;
	int rc;

	if (!rec)
		return 0;

	mutex_lock(&rec->lock);
	state = rec->state;
	mutex_unlock(&rec->lock);

	if (state == KVM_V2_RECORD_RECORDING ||
	    state == KVM_V2_RECORD_REPLAYING) {
		rc = kvm_v2_record_stop(rec);
		if (rc < 0)
			return rc;
	}

	return kvm_v2_record_reset(rec);
}

static int kvm_v2_record_debugfs_command(char *buf)
{
	char *cmd, *arg;
	bool strict;
	size_t buffer_size;
	int rc;

	cmd = strim(buf);
	if (!*cmd)
		return -EINVAL;

	arg = strpbrk(cmd, " \t");
	if (arg) {
		*arg++ = '\0';
		arg = skip_spaces(arg);
	} else {
		arg = cmd + strlen(cmd);
	}

	mutex_lock(&kvm_v2_record_debugfs_lock);

	if (!strcmp(cmd, "start")) {
		rc = kvm_v2_record_parse_size(arg, &buffer_size);
		if (rc < 0)
			goto out_unlock;
		rc = kvm_v2_record_debugfs_ensure(buffer_size);
		if (rc < 0)
			goto out_unlock;
		rc = kvm_v2_record_start_with_snapshot(kvm_v2_record_debugfs_rec);
		if (!rc)
			kvm_v2_record_suppress_next_syscall(kvm_v2_record_debugfs_rec,
							    __NR_write);
	} else if (!strcmp(cmd, "stop")) {
		if (!kvm_v2_record_debugfs_rec) {
			rc = -ENOENT;
			goto out_unlock;
		}
		rc = kvm_v2_record_stop(kvm_v2_record_debugfs_rec);
	} else if (!strcmp(cmd, "replay")) {
		if (!kvm_v2_record_debugfs_rec) {
			rc = -ENOENT;
			goto out_unlock;
		}
		rc = kvm_v2_record_replay(kvm_v2_record_debugfs_rec);
	} else if (!strcmp(cmd, "destroy") || !strcmp(cmd, "reset")) {
		rc = kvm_v2_record_debugfs_destroy();
	} else if (!strcmp(cmd, "strict")) {
		if (!kvm_v2_record_debugfs_rec) {
			rc = -ENOENT;
			goto out_unlock;
		}
		rc = kvm_v2_record_parse_bool(arg, &strict);
		if (rc < 0)
			goto out_unlock;
		rc = kvm_v2_record_set_strict_replay(kvm_v2_record_debugfs_rec,
						     strict);
	} else {
		rc = -EINVAL;
	}

out_unlock:
	mutex_unlock(&kvm_v2_record_debugfs_lock);
	return rc;
}

static ssize_t kvm_v2_record_ctl_write(struct file *file,
				       const char __user *buf,
				       size_t count, loff_t *ppos)
{
	char tmp[KVM_V2_RECORD_CTL_MAX];
	size_t copy_n;
	int rc;

	if (!count)
		return -EINVAL;

	copy_n = min_t(size_t, count, sizeof(tmp) - 1);
	if (copy_from_user(tmp, buf, copy_n))
		return -EFAULT;
	tmp[copy_n] = '\0';

	rc = kvm_v2_record_debugfs_command(tmp);
	return rc < 0 ? rc : (ssize_t)count;
}

static const struct file_operations kvm_v2_record_ctl_fops = {
	.write = kvm_v2_record_ctl_write,
};

static int kvm_v2_record_status_show(struct seq_file *m, void *v)
{
	struct kvm_v2_record *rec;
	bool enabled;

	enabled = static_branch_unlikely(&um_kvm_v2_record_enabled);

	mutex_lock(&kvm_v2_record_debugfs_lock);
	rec = kvm_v2_record_debugfs_rec;
	if (!rec) {
		seq_puts(m, "state: none\n");
		seq_printf(m, "enabled: %u\n", enabled ? 1 : 0);
		mutex_unlock(&kvm_v2_record_debugfs_lock);
		return 0;
	}

	mutex_lock(&rec->lock);
	seq_printf(m, "state: %s\n", kvm_v2_record_state_name(rec->state));
	seq_printf(m, "enabled: %u\n", enabled ? 1 : 0);
	seq_printf(m, "strict: %u\n", rec->strict_replay ? 1 : 0);
	seq_printf(m, "format_version: %u\n", KVM_V2_RECORD_FORMAT_VERSION);
	seq_printf(m, "entry_header_size: %zu\n",
		   (size_t)KVM_V2_RECORD_ENTRY_HEADER_SIZE);
	seq_printf(m, "entry_size: %zu\n",
		   sizeof(struct kvm_v2_replay_entry));
	seq_printf(m, "max_payload: %u\n", KVM_V2_RECORD_MAX_PAYLOAD);
	seq_printf(m, "snapshot_attempted: %u\n",
		   rec->snapshot_attempted ? 1 : 0);
	seq_printf(m, "snapshot_valid: %u\n", rec->snapshot_valid ? 1 : 0);
	seq_printf(m, "snapshot_rc: %d\n", rec->snapshot_rc);
	seq_printf(m, "snapshot_memslots: %d\n",
		   rec->snapshot ? rec->snapshot->memslot_count : 0);
	seq_printf(m, "snapshot_task_state: %u\n",
		   rec->snapshot && rec->snapshot->task_state_captured ? 1 : 0);
	seq_printf(m, "snapshot_source_pid: %d\n",
		   rec->snapshot ? rec->snapshot->task_source_pid : 0);
	seq_printf(m, "buffer_size: %zu\n", rec->buffer_size);
	seq_printf(m, "buffer_used: %zu\n", rec->buffer_used);
	seq_printf(m, "buffer_replayed: %zu\n", rec->buffer_replayed);
	seq_printf(m, "sequence: %llu\n", rec->sequence);
	seq_printf(m, "entries_recorded: %llu\n", rec->entries_recorded);
	seq_printf(m, "entries_replayed: %llu\n", rec->entries_replayed);
	seq_printf(m, "entries_dropped: %llu\n", rec->entries_dropped);
	seq_printf(m, "syscall_count: %llu\n", rec->syscall_count);
	seq_printf(m, "first_syscall_pid: %d\n", rec->first_syscall_pid);
	seq_printf(m, "last_syscall_pid: %d\n", rec->last_syscall_pid);
	seq_printf(m, "syscalls_from_snapshot_task: %llu\n",
		   rec->syscalls_from_snapshot_task);
	seq_printf(m, "syscalls_from_other_tasks: %llu\n",
		   rec->syscalls_from_other_tasks);
	seq_printf(m, "payload_entries_recorded: %llu\n",
		   rec->payload_entries_recorded);
	seq_printf(m, "payload_entries_replayed: %llu\n",
		   rec->payload_entries_replayed);
	seq_printf(m, "payload_bytes_recorded: %llu\n",
		   rec->payload_bytes_recorded);
	seq_printf(m, "payload_bytes_replayed: %llu\n",
		   rec->payload_bytes_replayed);
	seq_printf(m, "strict_replay_failures: %llu\n",
		   rec->strict_replay_failures);
	seq_printf(m, "last_replay_failure_syscall: %ld\n",
		   rec->last_replay_failure_syscall);
	seq_printf(m, "last_replay_failure_rc: %d\n",
		   rec->last_replay_failure_rc);
	mutex_unlock(&rec->lock);
	mutex_unlock(&kvm_v2_record_debugfs_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kvm_v2_record_status);

static int __init kvm_v2_record_debugfs_init(void)
{
	struct dentry *d;

	if (!debugfs_initialized())
		return 0;

	d = debugfs_lookup("um", NULL);
	if (!d) {
		d = debugfs_create_dir("um", NULL);
		if (IS_ERR(d))
			return PTR_ERR(d);
	}

	debugfs_create_file("kvm_v2_record_ctl", 0200, d, NULL,
			    &kvm_v2_record_ctl_fops);
	debugfs_create_file("kvm_v2_record_status", 0400, d, NULL,
			    &kvm_v2_record_status_fops);
	return 0;
}
late_initcall_sync(kvm_v2_record_debugfs_init);

#endif /* CONFIG_DEBUG_FS */
