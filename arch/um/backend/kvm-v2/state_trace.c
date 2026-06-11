// SPDX-License-Identifier: GPL-2.0
/*
 * KVM v2 private state trace ring.
 *
 * This is a bounded diagnostic ring for KVM_RUN dispatch triage. It records a
 * compact set of entry/exit fields instead of the historical all-state trace
 * snapshot, which carried investigation-specific assumptions. Runtime control
 * is explicit through debugfs and the hot path is behind a static key.
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "state_trace.h"

#define KVM_V2_STATE_TRACE_CAPACITY	4096

DEFINE_STATIC_KEY_FALSE(kvm_v2_state_trace_key);

struct kvm_v2_state_trace_event {
	u64 seq;
	u64 ts_ns;
	u32 cpu;
	u32 pid;
	u32 op;
	u32 exit_reason;
	u16 io_port;
	u16 _pad;
	u64 rip;
	u64 sp;
	u64 ax;
};

static DEFINE_SPINLOCK(kvm_v2_state_trace_lock);
static struct kvm_v2_state_trace_event *kvm_v2_state_trace_ring;
static u64 kvm_v2_state_trace_next;
static u64 kvm_v2_state_trace_overwritten;

static const char *kvm_v2_state_trace_op_name(u32 op)
{
	switch (op) {
	case KVM_V2_STATE_TRACE_RUN_ENTER:
		return "run_enter";
	case KVM_V2_STATE_TRACE_RUN_EXIT:
		return "run_exit";
	case KVM_V2_STATE_TRACE_RUN_EINTR:
		return "run_eintr";
	default:
		return "unknown";
	}
}

void kvm_v2_state_trace_record(enum kvm_v2_state_trace_op op, int cpu,
			       u32 exit_reason, u16 io_port, unsigned long rip,
			       unsigned long sp, unsigned long ax)
{
	struct kvm_v2_state_trace_event *ev;
	unsigned long flags;
	u64 seq;

	if (!kvm_v2_state_trace_ring)
		return;

	spin_lock_irqsave(&kvm_v2_state_trace_lock, flags);
	seq = kvm_v2_state_trace_next++;
	if (seq >= KVM_V2_STATE_TRACE_CAPACITY)
		kvm_v2_state_trace_overwritten++;

	ev = &kvm_v2_state_trace_ring[seq % KVM_V2_STATE_TRACE_CAPACITY];
	ev->seq = seq;
	ev->ts_ns = ktime_get_mono_fast_ns();
	ev->cpu = cpu;
	ev->pid = current ? task_pid_nr(current) : 0;
	ev->op = op;
	ev->exit_reason = exit_reason;
	ev->io_port = io_port;
	ev->rip = rip;
	ev->sp = sp;
	ev->ax = ax;
	spin_unlock_irqrestore(&kvm_v2_state_trace_lock, flags);
}

static void kvm_v2_state_trace_clear(void)
{
	unsigned long flags;

	spin_lock_irqsave(&kvm_v2_state_trace_lock, flags);
	if (kvm_v2_state_trace_ring)
		memset(kvm_v2_state_trace_ring, 0,
		       sizeof(*kvm_v2_state_trace_ring) *
		       KVM_V2_STATE_TRACE_CAPACITY);
	kvm_v2_state_trace_next = 0;
	kvm_v2_state_trace_overwritten = 0;
	spin_unlock_irqrestore(&kvm_v2_state_trace_lock, flags);
}

static int kvm_v2_state_trace_ctl_write_command(const char *cmd)
{
	if (!strcmp(cmd, "enable")) {
		static_branch_enable(&kvm_v2_state_trace_key);
		return 0;
	}
	if (!strcmp(cmd, "disable")) {
		static_branch_disable(&kvm_v2_state_trace_key);
		return 0;
	}
	if (!strcmp(cmd, "clear")) {
		kvm_v2_state_trace_clear();
		return 0;
	}

	return -EINVAL;
}

static ssize_t kvm_v2_state_trace_ctl_write(struct file *file,
					    const char __user *ubuf,
					    size_t len, loff_t *ppos)
{
	char buf[32];
	size_t n;

	n = min(len, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = '\0';
	strim(buf);

	if (kvm_v2_state_trace_ctl_write_command(buf))
		return -EINVAL;

	return len;
}

static const struct file_operations kvm_v2_state_trace_ctl_fops = {
	.write = kvm_v2_state_trace_ctl_write,
	.llseek = noop_llseek,
};

static int kvm_v2_state_trace_status_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	u64 next, overwritten;
	u64 entries;
	bool enabled = static_key_enabled(&kvm_v2_state_trace_key.key);

	spin_lock_irqsave(&kvm_v2_state_trace_lock, flags);
	next = kvm_v2_state_trace_next;
	overwritten = kvm_v2_state_trace_overwritten;
	spin_unlock_irqrestore(&kvm_v2_state_trace_lock, flags);

	entries = min_t(u64, next, KVM_V2_STATE_TRACE_CAPACITY);

	seq_printf(m, "enabled: %u\n", enabled ? 1 : 0);
	seq_printf(m, "capacity: %u\n", KVM_V2_STATE_TRACE_CAPACITY);
	seq_printf(m, "entry_size: %zu\n",
		   sizeof(struct kvm_v2_state_trace_event));
	seq_printf(m, "entries: %llu\n", entries);
	seq_printf(m, "sequence: %llu\n", next);
	seq_printf(m, "overwritten: %llu\n", overwritten);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kvm_v2_state_trace_status);

static int kvm_v2_state_trace_dump_show(struct seq_file *m, void *v)
{
	struct kvm_v2_state_trace_event *copy;
	unsigned long flags;
	u64 next, start;
	u32 entries, i;

	copy = kcalloc(KVM_V2_STATE_TRACE_CAPACITY, sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return -ENOMEM;

	spin_lock_irqsave(&kvm_v2_state_trace_lock, flags);
	next = kvm_v2_state_trace_next;
	entries = min_t(u64, next, KVM_V2_STATE_TRACE_CAPACITY);
	start = next - entries;
	for (i = 0; i < entries; i++)
		copy[i] = kvm_v2_state_trace_ring[(start + i) %
						  KVM_V2_STATE_TRACE_CAPACITY];
	spin_unlock_irqrestore(&kvm_v2_state_trace_lock, flags);

	seq_puts(m, "# seq ts_ns cpu pid op exit_reason io_port rip sp ax\n");
	for (i = 0; i < entries; i++) {
		const struct kvm_v2_state_trace_event *ev = &copy[i];

		seq_printf(m,
			   "seq=%llu ts_ns=%llu cpu=%u pid=%u op=%s exit_reason=%u io_port=%u rip=%#llx sp=%#llx ax=%#llx\n",
			   ev->seq, ev->ts_ns, ev->cpu, ev->pid,
			   kvm_v2_state_trace_op_name(ev->op),
			   ev->exit_reason, ev->io_port, ev->rip, ev->sp,
			   ev->ax);
	}

	kfree(copy);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kvm_v2_state_trace_dump);

static int __init kvm_v2_state_trace_debugfs_init(void)
{
	struct dentry *d;

	kvm_v2_state_trace_ring =
		kcalloc(KVM_V2_STATE_TRACE_CAPACITY,
			sizeof(*kvm_v2_state_trace_ring), GFP_KERNEL);
	if (!kvm_v2_state_trace_ring)
		return -ENOMEM;

	if (!debugfs_initialized())
		return 0;

	d = debugfs_lookup("um", NULL);
	if (!d) {
		d = debugfs_create_dir("um", NULL);
		if (IS_ERR(d))
			return PTR_ERR(d);
	}

	debugfs_create_file("kvm_v2_state_trace_ctl", 0200, d, NULL,
			    &kvm_v2_state_trace_ctl_fops);
	debugfs_create_file("kvm_v2_state_trace_status", 0400, d, NULL,
			    &kvm_v2_state_trace_status_fops);
	debugfs_create_file("kvm_v2_state_trace_dump", 0400, d, NULL,
			    &kvm_v2_state_trace_dump_fops);
	return 0;
}
late_initcall_sync(kvm_v2_state_trace_debugfs_init);
