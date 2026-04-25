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

#include <linux/errno.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "kvm_backend.h"

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
	u64	data[4];		/* kind-specific payload */
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
	if (!rec)
		return;
	if (rec->checkpoint)
		kvm_snapshot_destroy(rec->checkpoint);
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
	int rc;

	if (!rec)
		return -EINVAL;
	if (rec->checkpoint) {
		pr_warn("um: kvm record_start: container already armed\n");
		return -EBUSY;
	}

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
	if (!rec)
		return;
	if (!rec->recording && !rec->replaying)
		return;

	rec->recording = false;
	rec->replaying = false;
	pr_info("um: kvm record_stop: disarmed (%zu log entries captured)\n",
		rec->log_count);
}
EXPORT_SYMBOL_GPL(kvm_record_stop);

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
