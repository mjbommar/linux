// SPDX-License-Identifier: GPL-2.0
/*
 * KVM-backend record/replay skeleton (task #253 step 1, memo 13).
 *
 * This TU is the compile-clean entry point for the deterministic
 * record/replay path. Today it ships:
 *
 *   - struct kvm_record container + alloc/free.
 *   - Record-side enum kvm_replay_kind + struct kvm_replay_entry.
 *   - kvm_record_start / _stop / _replay shells that operate on
 *     the container but DON'T yet hook into the kvm_run_userspace
 *     dispatcher. Step 2+ of the memo-13 ladder (time / random /
 *     interrupt / syscall recording) does that wiring; this
 *     commit just stakes out the API surface and validates it
 *     compiles cleanly under CONFIG_UM_BACKEND_KVM_INTEGRATED.
 *
 * The container layers on top of the memo-12 snapshot primitives:
 * a kvm_record holds a kvm_snapshot (the checkpoint) plus a
 * variable-length kvm_replay_entry log. Capture and restore reuse
 * the snapshot.c primitives; the new code here is just the log +
 * the recording/replay state machine.
 *
 * Design memo: Documentation/virt/uml/redesign/02-workstreams/
 *              D-kvm-backend/13-record-replay-determinism.md
 *
 * NOT in this commit:
 *   - Hooks into kvm_run_userspace's dispatcher (memo 13 step 2-6).
 *   - Cycle / TSC freezing (step 2).
 *   - Syscall result interception (step 3).
 *   - PMU-driven interrupt boundary recording (step 4).
 *   - getrandom / drbg seeding (step 5).
 *   - MMIO recording (step 6).
 *
 * The recording / replaying flags live on the container but no
 * code path consults them yet. A future commit that adds the
 * dispatcher hooks will gate via static_branch_unlikely(&um_
 * record_enabled) so non-record builds pay zero cost.
 */

#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "kvm_backend.h"

/*
 * Hot-path gate. Off in every shipped profile; flipped on by
 * kvm_record_start when an active container is registered.
 * Dispatcher hooks (memo 13 step 2-6) check
 * static_branch_unlikely(&um_kvm_record_enabled) before doing
 * any record-side work, so non-record builds + non-recording
 * runtime pay zero per-syscall cost.
 *
 * The active record pointer is a global single-slot. ncpus=1
 * UML guarantees at most one record container active at a time;
 * the spinlock guards against debugfs racing kernel-side
 * teardown but is uncontended in the steady state.
 */
DEFINE_STATIC_KEY_FALSE(um_kvm_record_enabled);
EXPORT_SYMBOL_GPL(um_kvm_record_enabled);

static DEFINE_SPINLOCK(um_kvm_record_lock);
static struct kvm_record *um_kvm_active_record;

/*
 * Source-of-nondeterminism tags. Each kvm_replay_entry is one
 * captured guest-observable side-effect. The dispatcher hooks
 * (memo 13 step 2-6) emit these on capture and consume them on
 * replay.
 */
enum kvm_replay_kind {
	KVM_REPLAY_TIME = 0,	/* RDTSC / clock_gettime / vvar */
	KVM_REPLAY_RAND,	/* getrandom / RDRAND */
	KVM_REPLAY_INTERRUPT,	/* preemption injection point */
	KVM_REPLAY_SYSCALL,	/* class-A passthrough syscall return */
	KVM_REPLAY_MMIO_READ,	/* device read */
};

struct kvm_replay_entry {
	enum kvm_replay_kind kind;
	u64	instruction_count;	/* TSC at point of capture */
	u64	data[4];		/* kind-specific inline payload */

	/*
	 * Memo 13 step 3: variable-length side buffer for syscalls
	 * with output payloads larger than the inline `data[]`.
	 * NULL/0 when the entry doesn't need one (TIME / RAND
	 * fits in inline; SYSCALL with no output buffer doesn't
	 * need it; INTERRUPT / MMIO_READ ditto).
	 *
	 * For SYSCALL entries with side buffers, data[] carries:
	 *   data[0] = syscall NR
	 *   data[1] = return value (signed long, cast to u64)
	 *   data[2] = user buffer VA (so replay can re-write it)
	 *   data[3] = side-buffer length (== payload_len; redundant
	 *             but lets the replay path validate without
	 *             chasing the entry).
	 *
	 * Lifecycle: allocated via kvmalloc inside um_kvm_record_
	 * append_with_payload, freed in kvm_record_destroy when the
	 * container is torn down.
	 */
	void	*payload;
	size_t	payload_len;
};

/*
 * Initial log capacity in entries. Empirically, a typical
 * `/bin/echo` round-trip captures ~20 entries (mostly TIME +
 * SYSCALL); a syzkaller-style workload pushes that to a few
 * thousand per testcase. We grow on demand so the bound is
 * "available memory", not this constant.
 */
#define KVM_RECORD_LOG_INITIAL	256

struct kvm_record {
	struct kvm_snapshot		*checkpoint;
	struct kvm_replay_entry		*log;
	size_t				log_capacity;
	size_t				log_count;
	bool				recording;
	bool				replaying;
	size_t				replay_cursor;
};

/**
 * kvm_record_alloc - allocate a fresh record container.
 *
 * Returns a zero-initialized container on the heap, or NULL on
 * allocation failure. Caller pairs with kvm_record_destroy.
 */
struct kvm_record *kvm_record_alloc(void)
{
	struct kvm_record *rec = kzalloc(sizeof(*rec), GFP_KERNEL);

	if (!rec)
		return NULL;

	rec->log = kvmalloc_array(KVM_RECORD_LOG_INITIAL,
				  sizeof(struct kvm_replay_entry),
				  GFP_KERNEL);
	if (!rec->log) {
		kfree(rec);
		return NULL;
	}
	rec->log_capacity = KVM_RECORD_LOG_INITIAL;
	rec->log_count = 0;
	return rec;
}
EXPORT_SYMBOL_GPL(kvm_record_alloc);

/**
 * kvm_record_destroy - release a record container.
 * @rec: container to release; can be NULL.
 *
 * Frees the embedded snapshot (if any), the log, and the
 * container itself.
 */
void kvm_record_destroy(struct kvm_record *rec)
{
	size_t i;

	if (!rec)
		return;
	/*
	 * Defensive: if the caller forgot to stop, drop the gate
	 * before freeing so the dispatcher hook doesn't dereference
	 * freed memory.
	 */
	kvm_record_stop(rec);
	if (rec->checkpoint)
		kvm_snapshot_destroy(rec->checkpoint);

	/* Memo 13 step 3: free per-entry side buffers. */
	for (i = 0; i < rec->log_count; i++)
		kvfree(rec->log[i].payload);

	kvfree(rec->log);
	kfree(rec);
}
EXPORT_SYMBOL_GPL(kvm_record_destroy);

/**
 * kvm_record_start - capture a checkpoint and arm record mode.
 * @rec: container previously kvm_record_alloc'd.
 *
 * Issues kvm_snapshot_capture against vcpu0 and stores it as the
 * record's checkpoint. Sets recording=true; replaying=false.
 *
 * Until the dispatcher hooks land (memo 13 step 2+), the
 * recording=true flag is observable but no log entries are
 * appended; the function is API surface only.
 *
 * Returns 0 on success; -errno from kvm_snapshot_capture on
 * failure. On failure rec stays in a clean (no-checkpoint)
 * state; the caller can retry or destroy.
 */
int kvm_record_start(struct kvm_record *rec)
{
	unsigned long flags;
	int rc;

	if (!rec)
		return -EINVAL;
	if (rec->checkpoint) {
		pr_warn("um: kvm record_start: container already armed\n");
		return -EBUSY;
	}

	/*
	 * Single-active-record discipline: the dispatcher hooks
	 * consult one global pointer, so refuse a second start if
	 * another container already holds the slot. Lock-protected
	 * because debugfs / kselftest writers can race the
	 * controlling process's teardown.
	 */
	spin_lock_irqsave(&um_kvm_record_lock, flags);
	if (um_kvm_active_record) {
		spin_unlock_irqrestore(&um_kvm_record_lock, flags);
		pr_warn("um: kvm record_start: another record is already active\n");
		return -EBUSY;
	}
	spin_unlock_irqrestore(&um_kvm_record_lock, flags);

	rec->checkpoint = kvm_snapshot_alloc();
	if (!rec->checkpoint)
		return -ENOMEM;

	rc = kvm_snapshot_capture(rec->checkpoint);
	if (rc < 0) {
		kvm_snapshot_destroy(rec->checkpoint);
		rec->checkpoint = NULL;
		return rc;
	}

	rec->recording = true;
	rec->replaying = false;
	rec->log_count = 0;
	rec->replay_cursor = 0;

	/*
	 * Register the container + flip the gate. The unlikely-key
	 * branch makes non-record builds (and non-recording runtime)
	 * pay zero per-syscall cost; only when at least one record
	 * is active does the dispatcher consult um_kvm_active_record.
	 */
	spin_lock_irqsave(&um_kvm_record_lock, flags);
	um_kvm_active_record = rec;
	spin_unlock_irqrestore(&um_kvm_record_lock, flags);
	static_branch_enable(&um_kvm_record_enabled);

	pr_info("um: kvm record_start: armed (checkpoint captured, log capacity=%zu)\n",
		rec->log_capacity);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_record_start);

/**
 * kvm_record_stop - disarm record mode.
 * @rec: container previously kvm_record_start'd.
 *
 * Sets recording=false. Doesn't touch the checkpoint or the log;
 * caller can subsequently kvm_record_replay against the same
 * container, or kvm_record_destroy.
 *
 * Idempotent: stopping an unarmed container is a no-op.
 */
void kvm_record_stop(struct kvm_record *rec)
{
	unsigned long flags;
	bool was_active;

	if (!rec)
		return;
	if (!rec->recording && !rec->replaying)
		return;

	rec->recording = false;
	rec->replaying = false;

	/*
	 * Drop the gate + clear the active pointer if WE were the
	 * registered container. Other rec instances (rare under
	 * single-active discipline but defensible) leave the gate
	 * alone — only the one currently registered turns it off.
	 */
	spin_lock_irqsave(&um_kvm_record_lock, flags);
	was_active = (um_kvm_active_record == rec);
	if (was_active)
		um_kvm_active_record = NULL;
	spin_unlock_irqrestore(&um_kvm_record_lock, flags);
	if (was_active)
		static_branch_disable(&um_kvm_record_enabled);

	pr_info("um: kvm record_stop: disarmed (%zu log entries captured)\n",
		rec->log_count);
}
EXPORT_SYMBOL_GPL(kvm_record_stop);

/*
 * Internal: append an entry to the active record's log, growing
 * the array on demand. Called only under
 * static_branch_unlikely(&um_kvm_record_enabled).
 *
 * Locking: the spinlock guards the active-record pointer + the
 * log mutation. Under ncpus=1 the lock is uncontended; under SMP
 * (post-Phase-3) the same lock serializes per-vCPU recording.
 *
 * @payload / @payload_len optional side-buffer (memo 13 step 3).
 * When non-NULL, the buffer is COPIED into a kvmalloc allocation
 * owned by the entry; the caller's buffer is unowned post-call.
 * NULL / 0 omits the side buffer.
 */
static int um_kvm_record_append(enum kvm_replay_kind kind,
				u64 instr_count,
				u64 d0, u64 d1, u64 d2, u64 d3,
				const void *payload, size_t payload_len)
{
	unsigned long flags;
	struct kvm_record *rec;
	int rc = 0;

	spin_lock_irqsave(&um_kvm_record_lock, flags);
	rec = um_kvm_active_record;
	if (!rec || !rec->recording) {
		spin_unlock_irqrestore(&um_kvm_record_lock, flags);
		return 0;
	}

	if (rec->log_count >= rec->log_capacity) {
		size_t new_cap = rec->log_capacity * 2;
		struct kvm_replay_entry *new_log;

		spin_unlock_irqrestore(&um_kvm_record_lock, flags);
		new_log = kvmalloc_array(new_cap,
					 sizeof(struct kvm_replay_entry),
					 GFP_KERNEL);
		if (!new_log)
			return -ENOMEM;

		spin_lock_irqsave(&um_kvm_record_lock, flags);
		/*
		 * Re-check rec under the lock — it could have stopped
		 * between drop + re-acquire. If it did, drop the new
		 * buffer and bail.
		 */
		if (um_kvm_active_record != rec || !rec->recording) {
			spin_unlock_irqrestore(&um_kvm_record_lock, flags);
			kvfree(new_log);
			return 0;
		}
		memcpy(new_log, rec->log,
		       rec->log_count * sizeof(struct kvm_replay_entry));
		kvfree(rec->log);
		rec->log = new_log;
		rec->log_capacity = new_cap;
	}

	{
		struct kvm_replay_entry *e = &rec->log[rec->log_count];
		void *side = NULL;

		if (payload && payload_len) {
			/*
			 * Drop the lock to allocate (kvmalloc may sleep
			 * under GFP_KERNEL), then re-validate rec under
			 * the lock. The append is rare enough that
			 * dropping/re-acquiring is cheap; doing the
			 * allocation under the lock would risk an
			 * IRQs-disabled-too-long lockup on big payloads.
			 */
			spin_unlock_irqrestore(&um_kvm_record_lock, flags);
			side = kvmalloc(payload_len, GFP_KERNEL);
			if (!side)
				return -ENOMEM;
			memcpy(side, payload, payload_len);

			spin_lock_irqsave(&um_kvm_record_lock, flags);
			if (um_kvm_active_record != rec || !rec->recording) {
				spin_unlock_irqrestore(&um_kvm_record_lock,
						       flags);
				kvfree(side);
				return 0;
			}
			/*
			 * Re-confirm the slot is still ours. The log_count
			 * could have advanced under another writer in
			 * principle (single-active discipline says no, but
			 * be defensive). Re-derive `e` from the current
			 * cursor.
			 */
			if (rec->log_count >= rec->log_capacity) {
				/*
				 * Capacity may have moved during our drop.
				 * Bail; the caller can retry. Free the
				 * partial side buffer.
				 */
				spin_unlock_irqrestore(&um_kvm_record_lock,
						       flags);
				kvfree(side);
				return -EAGAIN;
			}
			e = &rec->log[rec->log_count];
		}

		e->kind = kind;
		e->instruction_count = instr_count;
		e->data[0] = d0;
		e->data[1] = d1;
		e->data[2] = d2;
		e->data[3] = d3;
		e->payload = side;
		e->payload_len = side ? payload_len : 0;
		rec->log_count++;
	}

	spin_unlock_irqrestore(&um_kvm_record_lock, flags);
	return rc;
}

/**
 * kvm_record_observe_syscall - record a class-A syscall result.
 * @syscall_nr: the NR (e.g. __NR_clock_gettime).
 * @ret_value: the return value the syscall produced (regs[HOST_AX]).
 * @arg0_data: optional output-buffer payload (e.g. seconds field
 *             for clock_gettime).
 * @arg1_data: optional second payload (e.g. nseconds field).
 *
 * Appends a SYSCALL entry to the active record's log when
 * recording. Called from kvm_decode_syscall's tail under the
 * static-key gate; if no record is active the call is a no-op
 * (early return inside um_kvm_record_append).
 *
 * The data payload is small + fixed-shape today (just two u64s).
 * Future syscalls with larger output buffers (read/write of
 * arbitrary buffers) need a side-buffer allocation; that's
 * memo-13-step-3 territory and out of scope here.
 */
void kvm_record_observe_syscall(unsigned long syscall_nr,
				long ret_value,
				u64 arg0_data, u64 arg1_data)
{
	(void)um_kvm_record_append(KVM_REPLAY_SYSCALL,
				   0,	/* instruction_count: TBD memo-13 step 4 */
				   (u64)syscall_nr,
				   (u64)ret_value,
				   arg0_data, arg1_data,
				   NULL, 0);
}
EXPORT_SYMBOL_GPL(kvm_record_observe_syscall);

/**
 * kvm_record_observe_syscall_buf - record a class-A syscall whose
 *                                  effect includes a user output
 *                                  buffer.
 * @syscall_nr: the NR (e.g. __NR_read, __NR_getrandom).
 * @ret_value: the syscall's return value (regs[HOST_AX]).
 * @user_buf_va: user VA where the syscall wrote its output. The
 *               replay path uses this to know where to scatter
 *               the recorded payload back.
 * @payload: kernel-side pointer to the buffer contents (the
 *           caller has typically just `copy_from_user`'d into
 *           a temporary). Copied into the entry's owned side
 *           buffer; safe to free post-call.
 * @payload_len: byte length of @payload.
 *
 * For SYSCALL entries with side buffers the inline data[] now
 * carries (NR, ret, user_buf_va, payload_len). When the
 * eventual replay-side dispatcher consumes a SYSCALL entry with
 * non-zero data[3], it copies entry->payload back to data[2]
 * via copy_to_user instead of dispatching handle_syscall.
 *
 * memo-13 step 3 ladder rung. Callers in the kvm_decode_syscall
 * dispatcher should special-case the read/write/getrandom/...
 * NRs and call this variant with the just-written buffer; the
 * vanilla kvm_record_observe_syscall above handles the no-buffer
 * case for everything else.
 */
void kvm_record_observe_syscall_buf(unsigned long syscall_nr,
				    long ret_value,
				    u64 user_buf_va,
				    const void *payload,
				    size_t payload_len)
{
	(void)um_kvm_record_append(KVM_REPLAY_SYSCALL,
				   0,	/* instruction_count: TBD memo-13 step 4 */
				   (u64)syscall_nr,
				   (u64)ret_value,
				   user_buf_va,
				   (u64)payload_len,
				   payload, payload_len);
}
EXPORT_SYMBOL_GPL(kvm_record_observe_syscall_buf);

/**
 * kvm_record_replay - restore the checkpoint and arm replay mode.
 * @rec: container with a previously-captured checkpoint.
 *
 * Issues kvm_snapshot_restore_full to roll the vCPU + memslot
 * back to the checkpoint state, then sets replaying=true and
 * resets the replay cursor to 0. Subsequent KVM_RUN iterations
 * will (once the dispatcher hooks land) consume log entries
 * from the front instead of consulting the host.
 *
 * Returns 0 on success; -EINVAL if no checkpoint exists; -errno
 * from kvm_snapshot_restore_full on ioctl failure.
 */
int kvm_record_replay(struct kvm_record *rec)
{
	int rc;

	if (!rec)
		return -EINVAL;
	if (!rec->checkpoint) {
		pr_warn("um: kvm record_replay: no checkpoint (call record_start first)\n");
		return -EINVAL;
	}

	rc = kvm_snapshot_restore_full(rec->checkpoint);
	if (rc < 0)
		return rc;

	rec->recording = false;
	rec->replaying = true;
	rec->replay_cursor = 0;

	pr_info("um: kvm record_replay: armed (checkpoint restored, %zu log entries to replay)\n",
		rec->log_count);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_record_replay);

#ifdef CONFIG_DEBUG_FS

/*
 * Debugfs control surface for the record/replay container.
 *
 * One debugfs-managed kvm_record container backs the entire
 * surface; userspace drives it via simple text commands.
 *
 *   echo start  > /sys/kernel/debug/um/kvm_record_ctl
 *   echo stop   > /sys/kernel/debug/um/kvm_record_ctl
 *   echo replay > /sys/kernel/debug/um/kvm_record_ctl
 *   cat /sys/kernel/debug/um/kvm_record_state
 *      → "recording=N replaying=N log_count=N log_capacity=N"
 *
 * This is the v1 control protocol; future kselftests + the
 * eventual syzkaller integration consume these nodes. For
 * programmatic record/replay outside the debugfs path, callers
 * use the kvm_record_* C API directly.
 */
static struct kvm_record *um_kvm_record_debugfs_rec;
static DEFINE_SPINLOCK(um_kvm_record_debugfs_lock);

static ssize_t kvm_record_ctl_write(struct file *f,
				    const char __user *buf,
				    size_t count, loff_t *ppos)
{
	char tmp[16];
	struct kvm_record *rec;
	size_t copy_n;
	unsigned long flags;
	int rc = 0;

	copy_n = min_t(size_t, count, sizeof(tmp) - 1);
	if (copy_from_user(tmp, buf, copy_n))
		return -EFAULT;
	tmp[copy_n] = '\0';
	if (copy_n > 0 && tmp[copy_n - 1] == '\n')
		tmp[copy_n - 1] = '\0';

	if (!strcmp(tmp, "start")) {
		spin_lock_irqsave(&um_kvm_record_debugfs_lock, flags);
		if (!um_kvm_record_debugfs_rec) {
			rec = kvm_record_alloc();
			um_kvm_record_debugfs_rec = rec;
		} else {
			rec = um_kvm_record_debugfs_rec;
		}
		spin_unlock_irqrestore(&um_kvm_record_debugfs_lock, flags);
		if (!rec)
			return -ENOMEM;
		rc = kvm_record_start(rec);
	} else if (!strcmp(tmp, "stop")) {
		spin_lock_irqsave(&um_kvm_record_debugfs_lock, flags);
		rec = um_kvm_record_debugfs_rec;
		spin_unlock_irqrestore(&um_kvm_record_debugfs_lock, flags);
		if (rec)
			kvm_record_stop(rec);
	} else if (!strcmp(tmp, "replay")) {
		spin_lock_irqsave(&um_kvm_record_debugfs_lock, flags);
		rec = um_kvm_record_debugfs_rec;
		spin_unlock_irqrestore(&um_kvm_record_debugfs_lock, flags);
		if (!rec)
			return -EINVAL;
		rc = kvm_record_replay(rec);
	} else if (!strcmp(tmp, "destroy")) {
		spin_lock_irqsave(&um_kvm_record_debugfs_lock, flags);
		rec = um_kvm_record_debugfs_rec;
		um_kvm_record_debugfs_rec = NULL;
		spin_unlock_irqrestore(&um_kvm_record_debugfs_lock, flags);
		if (rec)
			kvm_record_destroy(rec);
	} else {
		return -EINVAL;
	}

	if (rc < 0)
		return rc;
	return count;
}

static const struct file_operations kvm_record_ctl_fops = {
	.write = kvm_record_ctl_write,
};

static int kvm_record_state_show(struct seq_file *m, void *unused)
{
	struct kvm_record *rec;
	unsigned long flags;
	bool recording = false, replaying = false;
	size_t log_count = 0, log_capacity = 0;

	spin_lock_irqsave(&um_kvm_record_debugfs_lock, flags);
	rec = um_kvm_record_debugfs_rec;
	if (rec) {
		recording = rec->recording;
		replaying = rec->replaying;
		log_count = rec->log_count;
		log_capacity = rec->log_capacity;
	}
	spin_unlock_irqrestore(&um_kvm_record_debugfs_lock, flags);

	seq_printf(m, "recording=%d replaying=%d log_count=%zu log_capacity=%zu\n",
		   recording, replaying, log_count, log_capacity);
	return 0;
}

static int kvm_record_state_open(struct inode *ip, struct file *f)
{
	return single_open(f, kvm_record_state_show, NULL);
}

static const struct file_operations kvm_record_state_fops = {
	.open    = kvm_record_state_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int __init kvm_record_debugfs_init(void)
{
	struct dentry *d;

	d = debugfs_lookup("um", NULL);
	if (!d) {
		d = debugfs_create_dir("um", NULL);
		if (IS_ERR(d))
			return PTR_ERR(d);
	}

	debugfs_create_file("kvm_record_ctl",   0200, d, NULL,
			    &kvm_record_ctl_fops);
	debugfs_create_file("kvm_record_state", 0444, d, NULL,
			    &kvm_record_state_fops);
	return 0;
}
late_initcall_sync(kvm_record_debugfs_init);

#endif /* CONFIG_DEBUG_FS */
