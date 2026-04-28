// SPDX-License-Identifier: GPL-2.0
/*
 * KVM-backend record/replay (task #253, memo 13).
 *
 * This TU is the v1 record/replay path. It ships:
 *
 *   - struct kvm_record container + alloc/start/stop/replay/destroy.
 *   - Record-side enum kvm_replay_kind + struct kvm_replay_entry.
 *   - DEFINE_STATIC_KEY_FALSE(um_kvm_record_enabled) hot-path gate
 *     + active-record registry. The dispatcher in kvm_decode_syscall
 *     consults the gate via static_branch_unlikely so non-record
 *     runtime pays zero cost.
 *   - Observation hooks: kvm_record_observe_syscall (inline-only),
 *     kvm_record_observe_syscall_buf (with side-buffer payload),
 *     kvm_record_observe_dispatch (per-NR router covering
 *     getrandom / read / pread64 / recvfrom).
 *   - Replay-side dispatcher: kvm_record_consume_syscall returns
 *     the recorded (NR, ret, payload) for the next log entry.
 *     Strict-replay mode (default) fail-stops on divergence /
 *     end-of-log; loose mode falls through to live syscalls.
 *   - Debugfs control surface: /sys/kernel/debug/um/kvm_record_
 *     {ctl,state,log}.
 *
 * The container layers on top of the memo-12 snapshot primitives:
 * a kvm_record holds a kvm_snapshot (the checkpoint) plus a
 * variable-length kvm_replay_entry log. Capture and restore reuse
 * snapshot.c primitives; this TU adds the log + state machine.
 *
 * Design memo: Documentation/virt/uml/redesign/02-workstreams/
 *              D-kvm-backend/13-record-replay-determinism.md
 *
 * Deferred (memo 13 ladder):
 *   - PMU-driven INST_RETIRED for sub-instruction interrupt
 *     boundaries (step 4).
 *   - MMIO recording (step 6) — UML doesn't have device emulation
 *     in a shape that produces nondeterministic MMIO bytes today.
 *   - Additional NRs needing scatter-gather (recvmsg / readv) or
 *     per-driver knowledge (ioctl).
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
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <asm/unistd.h>
#include <sysdep/ptrace.h>		/* struct uml_pt_regs + HOST_* */

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
	u64	instruction_count;	/* ktime_get_ns() at capture
					 * time. v1: monotonic ns
					 * timestamp (per-entry ordering
					 * + diagnostic). Memo 13 step 4
					 * upgrades this to a PMU
					 * INST_RETIRED.ANY count for
					 * rr-style sub-instruction
					 * interrupt boundary recording.
					 */
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

	/*
	 * Memo 13 P2 #13 metadata-buffer extension: second side
	 * buffer for syscalls whose output is split between a data
	 * buffer (above) and out-parameters (sockaddr / addrlen /
	 * msg_control / iovec snapshot). NULL/0 when the entry
	 * doesn't need one.
	 *
	 * The metadata interpretation is per-NR, encoded by
	 * kvm_replay_meta_kind. recvfrom uses
	 * KVM_REPLAY_META_SOCKADDR with metadata layout
	 *   { u32 addrlen_returned; u8 sa_storage[]; }
	 * recvmsg / readv use KVM_REPLAY_META_IOV with a packed
	 * { iov_count; { iov_base, iov_len, iov_payload[] } * }
	 * structure.
	 *
	 * Same lifecycle as payload: allocated in append, freed on
	 * destroy. Bounded by KVM_RECORD_PAYLOAD_PER_CAP.
	 */
	void	*metadata;
	size_t	metadata_len;
	u32	metadata_kind;
};

/*
 * KVM_REPLAY_META_* constants are exported from kvm_backend.h
 * (and test_kvm_hooks.h for the contract test). Don't redeclare
 * them here.
 */

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

	/*
	 * Review-01 P1: replay strictness mode. Default is "strict"
	 * — log exhaustion + NR-mismatch divergence both refuse to
	 * dispatch, so the caller stops the guest task instead of
	 * silently running live syscalls. "loose" preserves the
	 * original v1 behaviour (fall-through to handle_syscall)
	 * for use cases that explicitly want to extend recording
	 * beyond the original session.
	 */
	bool				strict_replay;

	/*
	 * Review-01 P2 #14: payload memory accounting + caps. Sum
	 * of all payload_len over the log; bounded by per-entry
	 * and total-bytes caps to prevent unbounded growth on
	 * record-mode workloads that perform large reads /
	 * receives. Dropped-entry count surfaced via
	 * kvm_record_state for operator visibility.
	 */
	size_t				payload_bytes;
	size_t				payload_drops;
};

/*
 * Per-entry side-buffer cap: 1 MiB. Real-world syscalls returning
 * more than this in a single call (large read of a multi-MB file,
 * large recvmsg of a jumbo-frame batch) get truncated to the cap
 * with an entry that still records (NR, ret, user_va) but a
 * partial payload. Replay correctness for such truncated entries
 * is best-effort; the operator can raise the cap via the Kconfig
 * if their workload demands it (future work).
 */
#define KVM_RECORD_PAYLOAD_PER_CAP	(1U << 20)

/*
 * Total log-payload cap: 64 MiB. Sum of payload_bytes across all
 * entries. Once exceeded, further side-buffer requests get
 * dropped to inline-only entries (entry still records the NR +
 * ret, but no payload — replay can't restore the user buffer).
 * Bounded so a runaway record session can't wedge the host.
 */
#define KVM_RECORD_PAYLOAD_TOTAL_CAP	(64U * (1U << 20))

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
	rec->strict_replay = true;	/* review-01 P1: default fail-stop */
	rec->payload_bytes = 0;
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

	/* Memo 13 step 3: free per-entry side + metadata buffers. */
	for (i = 0; i < rec->log_count; i++) {
		kvfree(rec->log[i].payload);
		kvfree(rec->log[i].metadata);
	}
	rec->payload_bytes = 0;

	kvfree(rec->log);
	kfree(rec);
}
EXPORT_SYMBOL_GPL(kvm_record_destroy);

/**
 * kvm_record_start - arm record mode; opportunistically checkpoint.
 * @rec: container previously kvm_record_alloc'd.
 *
 * Tries kvm_snapshot_capture against vcpu0 and stores it as the
 * record's checkpoint. If the capture fails (e.g. early-boot when
 * the memslot isn't yet registered, or low memory), the record
 * still arms in *log-only* mode: the syscall log machinery is
 * independent of the snapshot, so observe/consume hooks remain
 * functional. Replay then skips the state restore step (no
 * checkpoint to restore) and just rewinds the log cursor.
 *
 * Sets recording=true; replaying=false; rec->checkpoint is
 * non-NULL only when capture succeeded.
 *
 * Returns 0 always (modulo allocation failure for the kvm_record
 * itself, which is -ENOMEM, or contract violations -EINVAL/-EBUSY).
 * The caller can inspect rec->checkpoint after return to know
 * whether replay will restore vCPU state or just drive the log.
 *
 * History: prior to this change, capture failure short-circuited
 * start — that meant the KUnit suite skipped the round-trip
 * assertions on early-boot (when capture reliably -ENODEV's
 * because the memslot is registered lazily after init_backend).
 * Decoupling lets the log-machinery tests run unconditionally
 * and exposes log-only recording as a first-class mode.
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
	if (rec->checkpoint) {
		rc = kvm_snapshot_capture(rec->checkpoint);
		if (rc < 0) {
			pr_info("um: kvm record_start: snapshot capture rc=%d — entering log-only mode (replay will not restore vCPU state)\n",
				rc);
			kvm_snapshot_destroy(rec->checkpoint);
			rec->checkpoint = NULL;
		}
	} else {
		pr_info("um: kvm record_start: snapshot alloc failed — entering log-only mode\n");
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

	pr_info("um: kvm record_start: armed (checkpoint=%s, log capacity=%zu)\n",
		rec->checkpoint ? "captured" : "log-only",
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
				const void *payload, size_t payload_len,
				const void *metadata, size_t metadata_len,
				u32 metadata_kind)
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
		void *meta = NULL;
		size_t side_len = payload_len;
		size_t meta_len = metadata_len;

		if (payload && payload_len) {
			/*
			 * Review-01 P2 #14: cap enforcement.
			 *
			 * Per-entry: cap at KVM_RECORD_PAYLOAD_PER_CAP.
			 * Metadata also bounded by the same cap (its
			 * realistic size is always a few hundred bytes —
			 * sockaddr_storage 128, msg_control truncated by
			 * msg_controllen — so the cap rarely fires).
			 *
			 * Total: payload_bytes counts both payload + metadata.
			 * If new total would exceed KVM_RECORD_PAYLOAD_
			 * TOTAL_CAP, drop the payload (and metadata) for
			 * this entry; bump payload_drops.
			 */
			if (side_len > KVM_RECORD_PAYLOAD_PER_CAP)
				side_len = KVM_RECORD_PAYLOAD_PER_CAP;
		}
		if (metadata && metadata_len) {
			if (meta_len > KVM_RECORD_PAYLOAD_PER_CAP)
				meta_len = KVM_RECORD_PAYLOAD_PER_CAP;
		}
		if (rec->payload_bytes + side_len + meta_len >
		    KVM_RECORD_PAYLOAD_TOTAL_CAP) {
			if (side_len || meta_len)
				rec->payload_drops++;
			side_len = 0;
			meta_len = 0;
		}

		if ((payload && side_len) || (metadata && meta_len)) {
			/*
			 * Drop the lock to allocate (kvmalloc may sleep
			 * under GFP_KERNEL), then re-validate rec under
			 * the lock. The append is rare enough that
			 * dropping/re-acquiring is cheap; doing the
			 * allocation under the lock would risk an
			 * IRQs-disabled-too-long lockup on big payloads.
			 */
			spin_unlock_irqrestore(&um_kvm_record_lock, flags);
			if (payload && side_len) {
				side = kvmalloc(side_len, GFP_KERNEL);
				if (!side)
					return -ENOMEM;
				memcpy(side, payload, side_len);
			}
			if (metadata && meta_len) {
				meta = kvmalloc(meta_len, GFP_KERNEL);
				if (!meta) {
					kvfree(side);
					return -ENOMEM;
				}
				memcpy(meta, metadata, meta_len);
			}

			spin_lock_irqsave(&um_kvm_record_lock, flags);
			if (um_kvm_active_record != rec || !rec->recording) {
				spin_unlock_irqrestore(&um_kvm_record_lock,
						       flags);
				kvfree(side);
				kvfree(meta);
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
				 * partial buffers.
				 */
				spin_unlock_irqrestore(&um_kvm_record_lock,
						       flags);
				kvfree(side);
				kvfree(meta);
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
		e->payload_len = side ? side_len : 0;
		e->metadata = meta;
		e->metadata_len = meta ? meta_len : 0;
		e->metadata_kind = meta ? metadata_kind :
			(u32)KVM_REPLAY_META_NONE;
		if (side)
			rec->payload_bytes += side_len;
		if (meta)
			rec->payload_bytes += meta_len;
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
				   ktime_get_ns(),
				   (u64)syscall_nr,
				   (u64)ret_value,
				   arg0_data, arg1_data,
				   NULL, 0,
				   NULL, 0, KVM_REPLAY_META_NONE);
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
				   ktime_get_ns(),
				   (u64)syscall_nr,
				   (u64)ret_value,
				   user_buf_va,
				   (u64)payload_len,
				   payload, payload_len,
				   NULL, 0, KVM_REPLAY_META_NONE);
}
EXPORT_SYMBOL_GPL(kvm_record_observe_syscall_buf);

/**
 * kvm_record_observe_syscall_buf_meta - record a class-A syscall
 *                                       with both data buffer +
 *                                       metadata side buffer.
 * @syscall_nr / @ret_value / @user_buf_va / @payload / @payload_len:
 *     same as kvm_record_observe_syscall_buf.
 * @metadata: kernel-side pointer to the metadata payload (e.g. a
 *            sockaddr_storage filled by recvfrom). Copied into
 *            an entry-owned kvmalloc allocation; safe to free
 *            post-call.
 * @metadata_len: byte length of @metadata.
 * @metadata_kind: enum kvm_replay_meta_kind tag describing how
 *                 the replay-side dispatcher should restore
 *                 the metadata. SOCKADDR for recvfrom; IOV for
 *                 recvmsg / readv (future).
 *
 * Memo 13 P2 #13 metadata-buffer extension. Today serves only
 * SOCKADDR (recvfrom); IOV cases come in follow-on commits.
 */
void kvm_record_observe_syscall_buf_meta(unsigned long syscall_nr,
					 long ret_value,
					 u64 user_buf_va,
					 const void *payload,
					 size_t payload_len,
					 const void *metadata,
					 size_t metadata_len,
					 u32 metadata_kind)
{
	(void)um_kvm_record_append(KVM_REPLAY_SYSCALL,
				   ktime_get_ns(),
				   (u64)syscall_nr,
				   (u64)ret_value,
				   user_buf_va,
				   (u64)payload_len,
				   payload, payload_len,
				   metadata, metadata_len, metadata_kind);
}
EXPORT_SYMBOL_GPL(kvm_record_observe_syscall_buf_meta);

/*
 * Per-NR record routing. Called from the dispatcher's gated
 * post-handle_syscall branch (kvm_decode_syscall) when recording
 * is active. Special-cases syscalls whose output buffer is a
 * source of replay nondeterminism — getrandom and friends —
 * routing them through the _buf variant; everything else uses
 * the inline-only kvm_record_observe_syscall.
 *
 * Today's special-cased set:
 *   __NR_getrandom: random bytes filled into user buffer.
 *
 * Future additions (memo 13 step 3 ladder):
 *   __NR_read, __NR_pread64, __NR_readv: file/socket payloads.
 *   __NR_recvfrom, __NR_recvmsg: network payloads.
 *   __NR_ioctl: device output.
 *
 * Keeping the special-case set narrow today minimizes the per-
 * syscall copy_from_user overhead on the recording hot path.
 */
/*
 * Internal: copy `len` bytes from user `va` into a fresh
 * kvmalloc staging buffer + emit an observe_syscall_buf entry.
 * Falls back to inline-only on allocation / copy failure so
 * recording stays partial-success rather than dropping entirely.
 */
static void um_kvm_record_capture_user_buf(unsigned long syscall_nr,
					   long ret_value,
					   unsigned long va,
					   size_t len,
					   unsigned long inline_arg1)
{
	void *staging;

	if (!len) {
		kvm_record_observe_syscall(syscall_nr, ret_value,
					   va, inline_arg1);
		return;
	}
	staging = kvmalloc(len, GFP_KERNEL);
	if (!staging) {
		kvm_record_observe_syscall(syscall_nr, ret_value,
					   va, inline_arg1);
		return;
	}
	if (copy_from_user(staging, (const void __user *)va, len)) {
		kvfree(staging);
		kvm_record_observe_syscall(syscall_nr, ret_value,
					   va, inline_arg1);
		return;
	}
	kvm_record_observe_syscall_buf(syscall_nr, ret_value, va,
				       staging, len);
	kvfree(staging);
}

void kvm_record_observe_dispatch(unsigned long syscall_nr,
				 long ret_value,
				 const struct uml_pt_regs *regs)
{
	if (!regs) {
		kvm_record_observe_syscall(syscall_nr, ret_value, 0, 0);
		return;
	}

	/*
	 * Per-NR routing. Each special case extracts the user-buffer
	 * VA + the captured-byte count from the appropriate regs->gp
	 * slot. Default falls through to the inline-only path with
	 * (HOST_DI, HOST_SI) as the inline payload — same shape the
	 * pre-step-3 path used.
	 */
	switch (syscall_nr) {
	case __NR_getrandom:
		/*
		 * getrandom(buf, len, flags): rdi=buf, rsi=len.
		 * On success ret_value == bytes filled (≤ len).
		 */
		if (ret_value > 0)
			um_kvm_record_capture_user_buf(syscall_nr,
						       ret_value,
						       regs->gp[HOST_DI],
						       (size_t)ret_value,
						       regs->gp[HOST_SI]);
		else
			kvm_record_observe_syscall(syscall_nr, ret_value,
						   regs->gp[HOST_DI],
						   regs->gp[HOST_SI]);
		return;
	case __NR_read:
	case __NR_pread64:
		/*
		 * read(fd, buf, count): rdi=fd, rsi=buf, rdx=count.
		 * pread64 same shape with extra offset arg.
		 * On success ret_value == bytes read (≤ count).
		 */
		if (ret_value > 0)
			um_kvm_record_capture_user_buf(syscall_nr,
						       ret_value,
						       regs->gp[HOST_SI],
						       (size_t)ret_value,
						       regs->gp[HOST_DX]);
		else
			kvm_record_observe_syscall(syscall_nr, ret_value,
						   regs->gp[HOST_DI],
						   regs->gp[HOST_SI]);
		return;
	case __NR_recvfrom: {
		/*
		 * recvfrom(sockfd, buf, len, flags, src_addr, addrlen):
		 * rdi=sockfd, rsi=buf, rdx=len, r10=flags, r8=src_addr,
		 * r9=addrlen. ret_value == bytes received.
		 *
		 * v2 (memo 13 P2 #13 metadata extension): capture both
		 * the data buffer at rsi AND the sockaddr at r8 (if
		 * non-NULL). The metadata side buffer holds:
		 *   { u32 addrlen_returned; u8 sa_storage[]; }
		 * so replay can copy_to_user the addrlen + sa bytes
		 * back. Replay-side dispatcher uses metadata_kind ==
		 * KVM_REPLAY_META_SOCKADDR to interpret + restore.
		 *
		 * src_addr / addrlen pointers may be NULL (caller
		 * doesn't care about origin); fall back to the no-
		 * metadata path then.
		 */
		unsigned long src_addr_va = regs->gp[HOST_R8];
		unsigned long addrlen_va = regs->gp[HOST_R9];

		if (ret_value <= 0) {
			kvm_record_observe_syscall(syscall_nr, ret_value,
						   regs->gp[HOST_DI],
						   regs->gp[HOST_SI]);
			return;
		}
		if (!src_addr_va || !addrlen_va) {
			/* Connected-socket case: data buffer only. */
			um_kvm_record_capture_user_buf(syscall_nr,
						       ret_value,
						       regs->gp[HOST_SI],
						       (size_t)ret_value,
						       regs->gp[HOST_DX]);
			return;
		}
		{
			/*
			 * Pull addrlen first (kernel wrote-back the
			 * actual returned length), then sockaddr_
			 * storage of that length. Bound to
			 * sizeof(struct sockaddr_storage) = 128 to
			 * match POSIX and to bound the per-entry
			 * memory.
			 */
			u32 addrlen = 0;
			u8 sa_buf[128];
			u8 meta[sizeof(u32) + sizeof(sa_buf)];
			size_t sa_len, meta_len;
			void *data_staging;

			if (copy_from_user(&addrlen,
					   (const void __user *)addrlen_va,
					   sizeof(addrlen))) {
				/* Fall back to data-only on metadata
				 * fetch failure. */
				um_kvm_record_capture_user_buf(syscall_nr,
							       ret_value,
							       regs->gp[HOST_SI],
							       (size_t)ret_value,
							       regs->gp[HOST_DX]);
				return;
			}
			sa_len = addrlen;
			if (sa_len > sizeof(sa_buf))
				sa_len = sizeof(sa_buf);
			if (sa_len &&
			    copy_from_user(sa_buf,
					   (const void __user *)src_addr_va,
					   sa_len)) {
				um_kvm_record_capture_user_buf(syscall_nr,
							       ret_value,
							       regs->gp[HOST_SI],
							       (size_t)ret_value,
							       regs->gp[HOST_DX]);
				return;
			}
			memcpy(meta, &addrlen, sizeof(addrlen));
			memcpy(meta + sizeof(addrlen), sa_buf, sa_len);
			meta_len = sizeof(addrlen) + sa_len;

			/*
			 * Stage the data buffer too — observe_syscall_
			 * buf_meta wants a kernel-side pointer for
			 * both. Use kvmalloc so we don't blow the
			 * kernel stack on multi-MB recvs.
			 */
			data_staging = kvmalloc((size_t)ret_value, GFP_KERNEL);
			if (!data_staging) {
				um_kvm_record_capture_user_buf(syscall_nr,
							       ret_value,
							       regs->gp[HOST_SI],
							       (size_t)ret_value,
							       regs->gp[HOST_DX]);
				return;
			}
			if (copy_from_user(data_staging,
					   (const void __user *)regs->gp[HOST_SI],
					   (size_t)ret_value)) {
				kvfree(data_staging);
				kvm_record_observe_syscall(syscall_nr, ret_value,
							   regs->gp[HOST_DI],
							   regs->gp[HOST_SI]);
				return;
			}
			kvm_record_observe_syscall_buf_meta(syscall_nr,
							    ret_value,
							    regs->gp[HOST_SI],
							    data_staging,
							    (size_t)ret_value,
							    meta, meta_len,
							    KVM_REPLAY_META_SOCKADDR);
			kvfree(data_staging);
		}
		return;
	}
	case __NR_readv: {
		/*
		 * readv(fd, iov, iovcnt): rdi=fd, rsi=iov, rdx=iovcnt.
		 * On success ret_value == total bytes scattered across
		 * iov[0..iovcnt-1] in order (kernel fills iov[0] fully
		 * before iov[1], etc.).
		 *
		 * Metadata layout (KVM_REPLAY_META_IOV):
		 *   u32 nr_iov;     // iovcnt as supplied by caller
		 *   u32 _pad;
		 *   struct { u64 base; u64 len; } iovs[nr_iov];
		 *
		 * Payload: a single concatenated buffer of ret_value
		 * bytes — the data the kernel actually wrote. Replay-
		 * side dispatcher walks the iovs and copies
		 * min(remaining, iovs[i].len) bytes from payload to
		 * iovs[i].base, just as the kernel did.
		 *
		 * Bound: cap iovcnt at UIO_FASTIOV (8) for v1 — that
		 * covers all common readv shapes (zero-copy header/
		 * payload split, scatter into N small ring buffers)
		 * without unbounded metadata growth. Larger iovcnts
		 * fall back to data-only capture; replay of those
		 * specific calls won't restore the buffers but the
		 * record cursor stays advanced and divergence detection
		 * still works on inline (NR, ret_value).
		 */
		unsigned long iov_va = regs->gp[HOST_SI];
		unsigned long iovcnt = regs->gp[HOST_DX];
		struct kvm_iov_pair { u64 base; u64 len; };

		if (ret_value <= 0 || !iov_va) {
			kvm_record_observe_syscall(syscall_nr, ret_value,
						   regs->gp[HOST_DI],
						   regs->gp[HOST_SI]);
			return;
		}
		if (iovcnt == 0 || iovcnt > 8 /* UIO_FASTIOV */) {
			/* Out of v1 envelope; data-only fallback. */
			kvm_record_observe_syscall(syscall_nr, ret_value,
						   regs->gp[HOST_DI],
						   regs->gp[HOST_SI]);
			return;
		}
		{
			/*
			 * Pull the iovec array (16 B per entry on x86_64).
			 * struct iovec { void *iov_base; size_t iov_len; }
			 * is layout-compatible with kvm_iov_pair on UML/
			 * x86_64 (both LP64). Capture by raw u64 pairs to
			 * avoid pulling in linux/uio.h for a layout-fixed
			 * use.
			 */
			struct kvm_iov_pair user_iovs[8];
			struct kvm_iov_pair captured[8];
			u8 meta[sizeof(u32) * 2 + sizeof(captured)];
			size_t meta_len;
			size_t i, total_capacity = 0;
			size_t bytes_remaining;
			void *data_staging, *cursor;
			u32 nr_iov_u32 = (u32)iovcnt;
			u32 zero_pad = 0;

			if (copy_from_user(user_iovs,
					   (const void __user *)iov_va,
					   iovcnt * sizeof(user_iovs[0]))) {
				kvm_record_observe_syscall(syscall_nr, ret_value,
							   regs->gp[HOST_DI],
							   regs->gp[HOST_SI]);
				return;
			}
			/*
			 * Sanity: total iov capacity must cover
			 * ret_value, otherwise the kernel couldn't have
			 * written that many bytes. Out-of-envelope →
			 * data-only fallback.
			 */
			for (i = 0; i < iovcnt; i++)
				total_capacity += user_iovs[i].len;
			if ((size_t)ret_value > total_capacity) {
				kvm_record_observe_syscall(syscall_nr, ret_value,
							   regs->gp[HOST_DI],
							   regs->gp[HOST_SI]);
				return;
			}
			memcpy(captured, user_iovs, iovcnt * sizeof(captured[0]));
			memcpy(meta, &nr_iov_u32, sizeof(nr_iov_u32));
			memcpy(meta + sizeof(nr_iov_u32), &zero_pad,
			       sizeof(zero_pad));
			memcpy(meta + 2 * sizeof(u32), captured,
			       iovcnt * sizeof(captured[0]));
			meta_len = 2 * sizeof(u32) +
				   iovcnt * sizeof(captured[0]);

			data_staging = kvmalloc((size_t)ret_value, GFP_KERNEL);
			if (!data_staging) {
				kvm_record_observe_syscall(syscall_nr, ret_value,
							   regs->gp[HOST_DI],
							   regs->gp[HOST_SI]);
				return;
			}
			/*
			 * Walk iovs in order, copying min(remaining,
			 * iovs[i].len) bytes from each iov_base into the
			 * concatenated staging buffer. Mirrors what the
			 * kernel did when filling them.
			 */
			bytes_remaining = (size_t)ret_value;
			cursor = data_staging;
			for (i = 0; i < iovcnt && bytes_remaining; i++) {
				size_t this_chunk = user_iovs[i].len;

				if (this_chunk > bytes_remaining)
					this_chunk = bytes_remaining;
				if (this_chunk &&
				    copy_from_user(cursor,
						   (const void __user *)
						   user_iovs[i].base,
						   this_chunk)) {
					kvfree(data_staging);
					kvm_record_observe_syscall(syscall_nr,
								   ret_value,
								   regs->gp[HOST_DI],
								   regs->gp[HOST_SI]);
					return;
				}
				cursor = (u8 *)cursor + this_chunk;
				bytes_remaining -= this_chunk;
			}
			/*
			 * user_buf_va is the iov array address (so replay
			 * has a stable handle for the metadata). The
			 * payload covers all iovs concatenated; the
			 * metadata tells replay how to scatter it.
			 */
			kvm_record_observe_syscall_buf_meta(syscall_nr,
							    ret_value,
							    iov_va,
							    data_staging,
							    (size_t)ret_value,
							    meta, meta_len,
							    KVM_REPLAY_META_IOV);
			kvfree(data_staging);
		}
		return;
	}
	default:
		break;
	}

	/* Default: inline-only payload. */
	kvm_record_observe_syscall(syscall_nr, ret_value,
				   regs->gp[HOST_DI],
				   regs->gp[HOST_SI]);
}
EXPORT_SYMBOL_GPL(kvm_record_observe_dispatch);

/**
 * kvm_record_consume_syscall - replay-side counterpart to
 *                              kvm_record_observe_syscall.
 * @syscall_nr: NR of the syscall the dispatcher is about to
 *              dispatch. Used to validate the log cursor.
 * @ret_out: receives the recorded return value on success.
 *
 * Returns 1 if a log entry was consumed (caller must use *ret_out
 * as the syscall return + skip handle_syscall + restore the user
 * buffer if entry->payload is non-NULL); 0 if no entry available
 * (caller falls through to handle_syscall as if not replaying);
 * negative on error.
 *
 * Design contract: the log was captured in dispatcher order, so
 * replay consumption is also in order. A replay that hits
 * end-of-log returns 0 and the caller dispatches normally —
 * useful for "extend the record beyond the original session"
 * shapes, though typical fuzzing replay terminates when the log
 * is exhausted.
 *
 * NR mismatch (log[cursor].data[0] != syscall_nr) currently
 * rejects with -EILSEQ. A stricter shape would terminate replay;
 * a looser one would skip and resync. v1 is strict — divergence
 * is a bug worth surfacing loudly.
 */
int kvm_record_consume_syscall(unsigned long syscall_nr,
			       long *ret_out,
			       u64 *user_buf_va_out,
			       const void **payload_out,
			       size_t *payload_len_out)
{
	return kvm_record_consume_syscall_meta(syscall_nr, ret_out,
					       user_buf_va_out,
					       payload_out, payload_len_out,
					       NULL, NULL, NULL);
}
EXPORT_SYMBOL_GPL(kvm_record_consume_syscall);

int kvm_record_consume_syscall_meta(unsigned long syscall_nr,
				    long *ret_out,
				    u64 *user_buf_va_out,
				    const void **payload_out,
				    size_t *payload_len_out,
				    const void **metadata_out,
				    size_t *metadata_len_out,
				    u32 *metadata_kind_out)
{
	unsigned long flags;
	struct kvm_record *rec;
	struct kvm_replay_entry *e;
	bool strict;
	int rc = 0;

	if (!ret_out)
		return -EINVAL;

	spin_lock_irqsave(&um_kvm_record_lock, flags);
	rec = um_kvm_active_record;
	if (!rec || !rec->replaying) {
		spin_unlock_irqrestore(&um_kvm_record_lock, flags);
		return 0;
	}
	strict = rec->strict_replay;
	if (rec->replay_cursor >= rec->log_count) {
		spin_unlock_irqrestore(&um_kvm_record_lock, flags);
		/*
		 * Review-01 P1: end-of-log policy is governed by
		 * strict_replay. Strict (default) returns -ENODATA so
		 * the caller fail-stops the guest task. Loose returns
		 * 0 (caller falls through to live handle_syscall) for
		 * "extend recording beyond original session" use.
		 */
		return strict ? -ENODATA : 0;
	}
	e = &rec->log[rec->replay_cursor];
	if (e->kind != KVM_REPLAY_SYSCALL ||
	    e->data[0] != (u64)syscall_nr) {
		pr_warn_ratelimited("um: kvm record_consume: divergence at cursor %zu (kind=%d expected_nr=%lu got_nr=%llu strict=%d)\n",
				    rec->replay_cursor, e->kind,
				    syscall_nr,
				    (unsigned long long)e->data[0],
				    strict);
		/*
		 * Review-01 P1: divergence always returns -EILSEQ.
		 * The dispatcher distinguishes strict vs loose by
		 * checking the active record's strict_replay flag
		 * via kvm_record_strict_replay() — strict aborts
		 * the syscall (delivers SIGSEGV), loose falls
		 * through to live handle_syscall.
		 */
		rc = -EILSEQ;
		goto out;
	}

	*ret_out = (long)e->data[1];
	if (user_buf_va_out)
		*user_buf_va_out = e->data[2];
	/*
	 * BUG.3 fix (2026-04-27): copy payload + metadata bytes under
	 * the lock into caller-owned heap allocations. Pre-fix, this
	 * function returned raw pointers into rec->log[i].payload /
	 * .metadata; the caller dereferenced them after dropping the
	 * lock, racing with debugfs `echo destroy > kvm_record_ctl`
	 * which calls kvm_record_destroy → kvfree(payload). UAF.
	 *
	 * Caller owns the duped buffers and must kvfree them when
	 * done. *_len_out is set even when *_out is NULL so the
	 * caller can still report the recorded size for diagnostics.
	 */
	if (payload_len_out)
		*payload_len_out = e->payload_len;
	if (payload_out) {
		*payload_out = NULL;
		if (e->payload && e->payload_len) {
			void *dup = kvmemdup(e->payload, e->payload_len,
					     GFP_ATOMIC);
			if (!dup) {
				rc = -ENOMEM;
				goto out;
			}
			*payload_out = dup;
		}
	}
	if (metadata_len_out)
		*metadata_len_out = e->metadata_len;
	if (metadata_out) {
		*metadata_out = NULL;
		if (e->metadata && e->metadata_len) {
			void *dup = kvmemdup(e->metadata, e->metadata_len,
					     GFP_ATOMIC);
			if (!dup) {
				/* Free the payload we already duped to keep symmetry */
				if (payload_out && *payload_out) {
					kvfree((void *)*payload_out);
					*payload_out = NULL;
				}
				rc = -ENOMEM;
				goto out;
			}
			*metadata_out = dup;
		}
	}
	if (metadata_kind_out)
		*metadata_kind_out = e->metadata_kind;
	rec->replay_cursor++;
	rc = 1;

out:
	spin_unlock_irqrestore(&um_kvm_record_lock, flags);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_record_consume_syscall_meta);

/**
 * kvm_record_strict_replay - is the active record in strict replay mode?
 *
 * Returns true iff the active container is in replay mode and has
 * strict_replay set. Returns false when no record is active, or
 * when the active record is recording, or when loose replay is in
 * effect. Used by the dispatcher to decide whether to fail-stop
 * (deliver SIGSEGV) or fall through to live handle_syscall on
 * end-of-log / divergence.
 */
bool kvm_record_strict_replay(void)
{
	unsigned long flags;
	struct kvm_record *rec;
	bool strict = false;

	spin_lock_irqsave(&um_kvm_record_lock, flags);
	rec = um_kvm_active_record;
	if (rec && rec->replaying)
		strict = rec->strict_replay;
	spin_unlock_irqrestore(&um_kvm_record_lock, flags);
	return strict;
}
EXPORT_SYMBOL_GPL(kvm_record_strict_replay);

/**
 * kvm_record_set_strict_replay - toggle strict-replay mode on the
 *                                active record.
 * @strict: true for fail-stop on divergence/end-of-log; false to
 *          fall through to live handle_syscall.
 *
 * Returns 0 if a record is active, -ENODEV if not.
 */
int kvm_record_set_strict_replay(bool strict)
{
	unsigned long flags;
	struct kvm_record *rec;
	int rc = -ENODEV;

	spin_lock_irqsave(&um_kvm_record_lock, flags);
	rec = um_kvm_active_record;
	if (rec) {
		rec->strict_replay = strict;
		rc = 0;
	}
	spin_unlock_irqrestore(&um_kvm_record_lock, flags);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_record_set_strict_replay);

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
 * Returns 0 on success; -errno from kvm_snapshot_restore_full on
 * ioctl failure when a checkpoint exists. If the record was started
 * in log-only mode (no checkpoint captured), replay still rewinds
 * the log cursor + arms replay flag — the caller drives consume_
 * syscall against an existing log without state restore.
 */
int kvm_record_replay(struct kvm_record *rec)
{
	unsigned long flags;
	bool need_register;
	int rc;

	if (!rec)
		return -EINVAL;

	/*
	 * Review-01 P0: `start -> stop -> replay` must re-arm the
	 * active-record slot + the static-key gate, otherwise the
	 * dispatcher's kvm_record_consume_syscall hook never fires
	 * and replay silently runs live syscalls.
	 *
	 * Determine whether we need to (re)register: if no slot is
	 * currently held, claim it. If we already hold the slot
	 * (replay-after-replay or replay-while-recording), it's a
	 * no-op. If a *different* container holds the slot, refuse
	 * with -EBUSY same as kvm_record_start does.
	 */
	spin_lock_irqsave(&um_kvm_record_lock, flags);
	if (um_kvm_active_record && um_kvm_active_record != rec) {
		spin_unlock_irqrestore(&um_kvm_record_lock, flags);
		pr_warn("um: kvm record_replay: another record is already active\n");
		return -EBUSY;
	}
	need_register = (um_kvm_active_record != rec);
	spin_unlock_irqrestore(&um_kvm_record_lock, flags);

	if (rec->checkpoint) {
		rc = kvm_snapshot_restore_full(rec->checkpoint);
		if (rc < 0)
			return rc;
	}

	rec->recording = false;
	rec->replaying = true;
	rec->replay_cursor = 0;

	if (need_register) {
		spin_lock_irqsave(&um_kvm_record_lock, flags);
		um_kvm_active_record = rec;
		spin_unlock_irqrestore(&um_kvm_record_lock, flags);
		static_branch_enable(&um_kvm_record_enabled);
	}

	pr_info("um: kvm record_replay: armed (%s, %zu log entries to replay)\n",
		rec->checkpoint ? "checkpoint restored" : "log-only",
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
	} else if (!strcmp(tmp, "strict")) {
		rc = kvm_record_set_strict_replay(true);
	} else if (!strcmp(tmp, "loose")) {
		rc = kvm_record_set_strict_replay(false);
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
	bool recording = false, replaying = false, strict = false;
	size_t log_count = 0, log_capacity = 0, payload_bytes = 0;
	size_t replay_cursor = 0, payload_drops = 0;

	spin_lock_irqsave(&um_kvm_record_debugfs_lock, flags);
	rec = um_kvm_record_debugfs_rec;
	if (rec) {
		recording = rec->recording;
		replaying = rec->replaying;
		strict = rec->strict_replay;
		log_count = rec->log_count;
		log_capacity = rec->log_capacity;
		payload_bytes = rec->payload_bytes;
		payload_drops = rec->payload_drops;
		replay_cursor = rec->replay_cursor;
	}
	spin_unlock_irqrestore(&um_kvm_record_debugfs_lock, flags);

	seq_printf(m, "recording=%d replaying=%d strict_replay=%d log_count=%zu log_capacity=%zu replay_cursor=%zu payload_bytes=%zu payload_drops=%zu\n",
		   recording, replaying, strict,
		   log_count, log_capacity, replay_cursor,
		   payload_bytes, payload_drops);
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

static const char *kvm_replay_kind_name(enum kvm_replay_kind k)
{
	switch (k) {
	case KVM_REPLAY_TIME:		return "TIME";
	case KVM_REPLAY_RAND:		return "RAND";
	case KVM_REPLAY_INTERRUPT:	return "INTERRUPT";
	case KVM_REPLAY_SYSCALL:	return "SYSCALL";
	case KVM_REPLAY_MMIO_READ:	return "MMIO_READ";
	}
	return "?";
}

/*
 * `kvm_record_log` — readable dump of the active record's log
 * entries. One line per entry with the `kind` tag, instruction
 * count, inline data[], and a payload-presence indicator.
 *
 * Bounded output: caps at the first 256 entries to keep the seq
 * buffer manageable; deeper logs need a programmatic API (future
 * work — sysfs export, snapshot-to-disk, etc.). Today this is
 * for human triage of short fuzz runs.
 */
#define KVM_RECORD_LOG_DUMP_MAX	256

static int kvm_record_log_show(struct seq_file *m, void *unused)
{
	struct kvm_record *rec;
	unsigned long flags;
	size_t i, count, dump_n;
	struct kvm_replay_entry *snap;

	spin_lock_irqsave(&um_kvm_record_debugfs_lock, flags);
	rec = um_kvm_record_debugfs_rec;
	if (!rec) {
		spin_unlock_irqrestore(&um_kvm_record_debugfs_lock, flags);
		seq_puts(m, "(no active record)\n");
		return 0;
	}
	count = rec->log_count;
	dump_n = min_t(size_t, count, KVM_RECORD_LOG_DUMP_MAX);
	if (!dump_n) {
		spin_unlock_irqrestore(&um_kvm_record_debugfs_lock, flags);
		seq_printf(m, "(empty log; capacity=%zu)\n", rec->log_capacity);
		return 0;
	}
	/*
	 * Snapshot the bounded prefix while holding the lock so the
	 * seq_printf below doesn't race a concurrent observe_syscall.
	 * Bounded by KVM_RECORD_LOG_DUMP_MAX (256) so the alloc is
	 * always small.
	 */
	snap = kmalloc_array(dump_n, sizeof(*snap), GFP_ATOMIC);
	if (!snap) {
		spin_unlock_irqrestore(&um_kvm_record_debugfs_lock, flags);
		seq_puts(m, "(snapshot kmalloc failed)\n");
		return 0;
	}
	memcpy(snap, rec->log, dump_n * sizeof(*snap));
	spin_unlock_irqrestore(&um_kvm_record_debugfs_lock, flags);

	seq_printf(m, "log_count=%zu (showing first %zu)\n", count, dump_n);
	for (i = 0; i < dump_n; i++) {
		seq_printf(m, "[%4zu] %-9s ic=%llu d0=%llx d1=%llx d2=%llx d3=%llx payload_len=%zu\n",
			   i,
			   kvm_replay_kind_name(snap[i].kind),
			   (unsigned long long)snap[i].instruction_count,
			   (unsigned long long)snap[i].data[0],
			   (unsigned long long)snap[i].data[1],
			   (unsigned long long)snap[i].data[2],
			   (unsigned long long)snap[i].data[3],
			   snap[i].payload_len);
	}
	if (count > dump_n)
		seq_printf(m, "... (%zu more entries truncated)\n",
			   count - dump_n);

	kfree(snap);
	return 0;
}

static int kvm_record_log_open(struct inode *ip, struct file *f)
{
	return single_open(f, kvm_record_log_show, NULL);
}

static const struct file_operations kvm_record_log_fops = {
	.open    = kvm_record_log_open,
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
	debugfs_create_file("kvm_record_log",   0444, d, NULL,
			    &kvm_record_log_fops);
	return 0;
}
late_initcall_sync(kvm_record_debugfs_init);

#endif /* CONFIG_DEBUG_FS */
