// SPDX-License-Identifier: GPL-2.0
/*
 * KVM v2 record/replay core.
 *
 * This file owns the in-memory record container and the bounded syscall log
 * used by the experimental replay path. The live KVM syscall dispatcher
 * observes and consumes syscall entries from this core; snapshot-backed record
 * start is present, but time, signal, and device determinism remain outside the
 * current supported surface.
 */

#include <linux/errno.h>
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

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
