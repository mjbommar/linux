// SPDX-License-Identifier: GPL-2.0
/*
 * KVM-backend record/replay — v2 port (Phase 1 only).
 *
 * Strategic Time-machine lift per PLAN-2026-05-14 §4.1 (#169) and
 * memo 27 (27-record-replay-v2-port.md). v1's implementation at
 * kvm-v1-archive/record.c (1597 LoC) proved the design; this file
 * ports the public surface onto v2's vCPU-pool + LSTAR-gadget +
 * signal-queue topology, starting from the smallest landable slice.
 *
 * Phase 1 (this revision):
 *   - struct kvm_v2_record + alloc/destroy/free.
 *   - State machine: INIT → RECORDING → STOPPED → REPLAYING.
 *   - DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled) hot-path gate
 *     + single-active-record discipline.
 *   - observe_syscall / consume_syscall are no-op stubs marked with
 *     a `Phase N` comment so the Phase 2-3 hooks can land additively.
 *   - strict_replay toggle (defaults to true, mirrors v1).
 *
 * Out of Phase 1 (per memo 27 §Phase 1 + §4):
 *   - Live syscall_trap.c observe hook (Phase 2).
 *   - RDTSC / vvar / SIGALRM capture (Phases 5-6).
 *   - Gadget disable on record-enable (Phase 4).
 *   - Replay primitive — consume from buffer (Phase 3).
 *   - Per-NR side-buffer routing (Phase 2.5).
 *   - Snapshot integration (the container holds a kvm_v2_snapshot in
 *     Phase 3+ but Phase 1 runs in "log-only" mode per memo 27 §3.9).
 *
 * The static-key gate is in place from day 1 so Phase 2's hook in
 * syscall_trap.c lands as a pure addition. The state-machine
 * transitions are the v1 shape (kvm-v1-archive/record.c:307-410)
 * minus the snapshot capture/restore calls (which the Phase 3
 * commit re-introduces against the kvm_v2_snapshot_* surface).
 *
 * Design memo:
 *   Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
 *   27-record-replay-v2-port.md
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <sysdep/ptrace.h>		/* struct uml_pt_regs */

#include "kvm_v2_backend.h"

/*
 * Hot-path gate.
 *
 * Off in every shipped profile; flipped on by kvm_v2_record_start
 * when an active container is registered, off again by _stop /
 * _destroy. The unlikely-key branch compiles to a 5-byte NOP that
 * the kernel patches out at boot, so non-record runtime pays zero
 * per-syscall cost. The Phase 2 hook in syscall_trap.c wraps its
 * call in static_branch_unlikely(&um_kvm_v2_record_enabled).
 *
 * Mirror of v1 (kvm-v1-archive/record.c:73-74) with the symbol
 * prefix swapped kvm_ → kvm_v2_ per memo 27 §3.10.
 */
DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled);
EXPORT_SYMBOL_GPL(um_kvm_v2_record_enabled);

/*
 * Single-active-record slot. The dispatcher hooks (Phase 2-3) consult
 * one global pointer, so refuse a second start when another container
 * already holds the slot. Lock-protected because debugfs / kselftest
 * writers can race the controlling process's teardown; under single-
 * vCPU Phase 1 the lock is uncontended in the steady state.
 *
 * Mirror of v1 (kvm-v1-archive/record.c:76-77).
 */
static DEFINE_SPINLOCK(um_kvm_v2_record_lock);
static struct kvm_v2_record *um_kvm_v2_active_record;

/*
 * Default buffer size when kvm_v2_record_alloc's caller passes 0.
 * 64 KiB matches v1's initial log capacity (256 entries × 256-byte
 * stride budget). Phase 2's append helper grows on demand; Phase 1
 * never writes to the buffer so the value is conservative for the
 * KUnit's allocator behaviour test.
 */
#define KVM_V2_RECORD_DEFAULT_BUFFER	(64U * 1024U)

/*
 * Maximum buffer size accepted by _alloc. Bounded so a debugfs /
 * userspace caller can't request an unbounded kvmalloc on Phase 2+
 * once the buffer actually gets written to.
 *
 * 64 MiB matches v1's KVM_RECORD_PAYLOAD_TOTAL_CAP
 * (kvm-v1-archive/record.c:216).
 */
#define KVM_V2_RECORD_MAX_BUFFER	(64U * 1024U * 1024U)

/**
 * kvm_v2_record_alloc - allocate a fresh record container.
 * @buffer_size: requested buffer size in bytes; 0 selects the default
 *               (KVM_V2_RECORD_DEFAULT_BUFFER). Capped at
 *               KVM_V2_RECORD_MAX_BUFFER; oversized requests return
 *               NULL.
 *
 * Returns a zero-initialised container on the heap with state =
 * KVM_V2_RECORD_INIT and @strict_replay = true (v1 default). Caller
 * pairs with kvm_v2_record_destroy.
 *
 * Allocation failure (kvmalloc for the entry buffer or kzalloc for
 * the container itself) returns NULL.
 *
 * Mirror of v1's kvm_record_alloc (kvm-v1-archive/record.c:224-243),
 * adapted to the Phase 1 fixed-size buffer shape (vs v1's growable
 * log array — Phase 2 may revisit).
 */
struct kvm_v2_record *kvm_v2_record_alloc(size_t buffer_size)
{
	struct kvm_v2_record *rec;

	if (buffer_size == 0)
		buffer_size = KVM_V2_RECORD_DEFAULT_BUFFER;
	if (buffer_size > KVM_V2_RECORD_MAX_BUFFER) {
		pr_warn("um: kvm-v2 record_alloc: buffer_size=%zu exceeds cap %u\n",
			buffer_size, KVM_V2_RECORD_MAX_BUFFER);
		return NULL;
	}

	rec = kzalloc(sizeof(*rec), GFP_KERNEL);
	if (!rec)
		return NULL;

	rec->buffer = kvmalloc(buffer_size, GFP_KERNEL);
	if (!rec->buffer) {
		kfree(rec);
		return NULL;
	}
	rec->buffer_size = buffer_size;
	rec->buffer_used = 0;
	rec->sequence = 0;
	rec->entries_recorded = 0;
	rec->entries_replayed = 0;
	rec->state = KVM_V2_RECORD_INIT;
	rec->strict_replay = true;	/* v1 default — fail-stop on divergence */
	mutex_init(&rec->lock);

	return rec;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_alloc);

/*
 * Internal: drop the static-key gate + clear the active-record slot
 * if @rec currently holds it. Called from _stop / _destroy. Safe to
 * call with @rec not the active record (no-op).
 *
 * Returns true iff we actually disarmed the gate (caller may want to
 * log that fact).
 */
static bool kvm_v2_record_disarm_gate(struct kvm_v2_record *rec)
{
	unsigned long flags;
	bool was_active;

	spin_lock_irqsave(&um_kvm_v2_record_lock, flags);
	was_active = (um_kvm_v2_active_record == rec);
	if (was_active)
		um_kvm_v2_active_record = NULL;
	spin_unlock_irqrestore(&um_kvm_v2_record_lock, flags);
	if (was_active)
		static_branch_disable(&um_kvm_v2_record_enabled);
	return was_active;
}

/**
 * kvm_v2_record_destroy - release a record container.
 * @rec: container to release; may be NULL.
 *
 * Defensive: if the caller forgot to stop, drop the gate first so
 * the Phase 2 dispatcher hook (once it lands) doesn't dereference
 * freed memory. Then free the entry buffer and the container.
 *
 * Mirror of v1's kvm_record_destroy (kvm-v1-archive/record.c:253-277).
 */
void kvm_v2_record_destroy(struct kvm_v2_record *rec)
{
	if (!rec)
		return;

	/* Drop the gate if we still hold it. */
	(void)kvm_v2_record_disarm_gate(rec);

	kvfree(rec->buffer);
	rec->buffer = NULL;
	mutex_destroy(&rec->lock);
	kfree(rec);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_destroy);

/**
 * kvm_v2_record_free - alias for kvm_v2_record_destroy.
 * @rec: container to release; may be NULL.
 *
 * v1 exposed both `_destroy` and `_free` (the latter for kselftest
 * consumers that wanted symmetric naming with `_alloc`); the port
 * preserves the alias so future Phase 7 kselftest plumbing can pick
 * either name without recompile churn. Implementation is identical.
 */
void kvm_v2_record_free(struct kvm_v2_record *rec)
{
	kvm_v2_record_destroy(rec);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_free);

/**
 * kvm_v2_record_start - arm record mode on @rec.
 * @rec: container previously returned by kvm_v2_record_alloc.
 *
 * Transitions INIT → RECORDING. Registers @rec as the single active
 * record and flips the static-key gate so the Phase 2 observe hook
 * starts firing. Resets the per-session counters (buffer_used,
 * sequence, entries_recorded) so a single container can be re-armed
 * after a stop without leaking previous-session state into the new
 * one.
 *
 * Returns 0 on success, -EINVAL on NULL @rec or invalid state,
 * -EBUSY when another container already holds the active slot.
 *
 * State-machine contract:
 *   INIT       → RECORDING   (0)
 *   RECORDING  → -EINVAL     (already recording)
 *   STOPPED    → RECORDING   (0; re-arm with cleared counters)
 *   REPLAYING  → -EINVAL     (must stop replay first)
 *
 * Mirror of v1's kvm_record_start (kvm-v1-archive/record.c:307-367)
 * minus the snapshot capture call (Phase 3 re-introduces against
 * kvm_v2_snapshot_capture).
 */
int kvm_v2_record_start(struct kvm_v2_record *rec)
{
	unsigned long flags;
	int rc = 0;

	if (!rec)
		return -EINVAL;

	mutex_lock(&rec->lock);

	if (rec->state == KVM_V2_RECORD_RECORDING) {
		mutex_unlock(&rec->lock);
		pr_warn("um: kvm-v2 record_start: container already recording\n");
		return -EINVAL;
	}
	if (rec->state == KVM_V2_RECORD_REPLAYING) {
		mutex_unlock(&rec->lock);
		pr_warn("um: kvm-v2 record_start: container is replaying (stop first)\n");
		return -EINVAL;
	}

	/*
	 * Single-active-record discipline. Refuse a second start while
	 * a different container holds the slot. The Phase 2 hook
	 * consults the slot via static_branch + spinlock; allowing
	 * multiple active records would race the dispatcher.
	 */
	spin_lock_irqsave(&um_kvm_v2_record_lock, flags);
	if (um_kvm_v2_active_record && um_kvm_v2_active_record != rec) {
		spin_unlock_irqrestore(&um_kvm_v2_record_lock, flags);
		mutex_unlock(&rec->lock);
		pr_warn("um: kvm-v2 record_start: another record is already active\n");
		return -EBUSY;
	}
	um_kvm_v2_active_record = rec;
	spin_unlock_irqrestore(&um_kvm_v2_record_lock, flags);

	/* Reset per-session counters; preserve strict_replay across re-arm. */
	rec->buffer_used = 0;
	rec->sequence = 0;
	rec->entries_recorded = 0;
	rec->entries_replayed = 0;
	rec->state = KVM_V2_RECORD_RECORDING;

	/*
	 * Flip the static-key gate. static_branch_enable is idempotent;
	 * if a previous start raced a stop the kernel-side patching
	 * machinery handles the multiple-call case correctly. Phase 2's
	 * hook reads the key with static_branch_unlikely().
	 */
	static_branch_enable(&um_kvm_v2_record_enabled);

	mutex_unlock(&rec->lock);

	pr_info("um: kvm-v2 record_start: armed (buffer_size=%zu, strict_replay=%d)\n",
		rec->buffer_size, rec->strict_replay);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_start);

/**
 * kvm_v2_record_stop - disarm record/replay mode on @rec.
 * @rec: container previously _start'd or _replay'd.
 *
 * Transitions RECORDING → STOPPED or REPLAYING → STOPPED. Clears
 * the active-record slot + flips the static-key gate off if @rec
 * was the registered container.
 *
 * Returns 0 on success, -EINVAL on NULL @rec or invalid state
 * (INIT / STOPPED — nothing to stop).
 *
 * State-machine contract:
 *   INIT       → -EINVAL     (never started)
 *   RECORDING  → STOPPED     (0)
 *   STOPPED    → -EINVAL     (already stopped)
 *   REPLAYING  → STOPPED     (0)
 *
 * Mirror of v1's kvm_record_stop (kvm-v1-archive/record.c:380-409),
 * which v1 made idempotent. The port here is STRICTER (returns
 * -EINVAL on stop-without-start) per memo 27 §Phase 1's "assert
 * invalid transitions return error" criterion; this is the
 * intended Phase 1 surface. Phase 2-3 may revisit if a use case
 * surfaces.
 */
int kvm_v2_record_stop(struct kvm_v2_record *rec)
{
	bool was_active;

	if (!rec)
		return -EINVAL;

	mutex_lock(&rec->lock);

	if (rec->state == KVM_V2_RECORD_INIT) {
		mutex_unlock(&rec->lock);
		pr_warn("um: kvm-v2 record_stop: container never started\n");
		return -EINVAL;
	}
	if (rec->state == KVM_V2_RECORD_STOPPED) {
		mutex_unlock(&rec->lock);
		pr_warn("um: kvm-v2 record_stop: container already stopped\n");
		return -EINVAL;
	}

	rec->state = KVM_V2_RECORD_STOPPED;

	/*
	 * Drop the gate + clear the active pointer if WE were the
	 * registered container. Other rec instances (rare under
	 * single-active discipline but defensible) leave the gate
	 * alone — only the one currently registered turns it off.
	 */
	mutex_unlock(&rec->lock);
	was_active = kvm_v2_record_disarm_gate(rec);

	pr_info("um: kvm-v2 record_stop: disarmed (%llu entries recorded, %llu replayed%s)\n",
		rec->entries_recorded, rec->entries_replayed,
		was_active ? "" : ", gate was held by another container");
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_stop);

/**
 * kvm_v2_record_replay - arm replay mode on @rec.
 * @rec: container previously _stop'd from RECORDING.
 *
 * Transitions STOPPED → REPLAYING. Re-registers @rec as the active
 * container + re-flips the static-key gate so the Phase 3 consume
 * hook starts firing. The recorded buffer + sequence counters are
 * NOT reset — replay reads them.
 *
 * Returns 0 on success, -EINVAL on NULL @rec or invalid state,
 * -EBUSY when another container holds the active slot.
 *
 * State-machine contract:
 *   INIT       → -EINVAL    (no log to replay)
 *   RECORDING  → -EINVAL    (must stop first)
 *   STOPPED    → REPLAYING  (0)
 *   REPLAYING  → -EINVAL    (already replaying)
 *
 * Phase 1 skips v1's snapshot restore step (kvm_record_replay's call
 * to kvm_snapshot_restore_full at kvm-v1-archive/record.c:1331).
 * Phase 3 of #169 re-introduces against kvm_v2_snapshot_restore_full
 * once snapshot Phase 3 is in tree (already landed; memo 27 §3.9).
 */
int kvm_v2_record_replay(struct kvm_v2_record *rec)
{
	unsigned long flags;
	bool need_register;

	if (!rec)
		return -EINVAL;

	mutex_lock(&rec->lock);

	if (rec->state == KVM_V2_RECORD_INIT) {
		mutex_unlock(&rec->lock);
		pr_warn("um: kvm-v2 record_replay: container has no recorded log\n");
		return -EINVAL;
	}
	if (rec->state == KVM_V2_RECORD_RECORDING) {
		mutex_unlock(&rec->lock);
		pr_warn("um: kvm-v2 record_replay: container is recording (stop first)\n");
		return -EINVAL;
	}
	if (rec->state == KVM_V2_RECORD_REPLAYING) {
		mutex_unlock(&rec->lock);
		pr_warn("um: kvm-v2 record_replay: container already replaying\n");
		return -EINVAL;
	}

	/*
	 * Re-claim the active-record slot if it's empty (or already
	 * ours). A different container holding the slot is a hard
	 * error — same fail-stop discipline as _start.
	 */
	spin_lock_irqsave(&um_kvm_v2_record_lock, flags);
	if (um_kvm_v2_active_record && um_kvm_v2_active_record != rec) {
		spin_unlock_irqrestore(&um_kvm_v2_record_lock, flags);
		mutex_unlock(&rec->lock);
		pr_warn("um: kvm-v2 record_replay: another record is already active\n");
		return -EBUSY;
	}
	need_register = (um_kvm_v2_active_record != rec);
	if (need_register)
		um_kvm_v2_active_record = rec;
	spin_unlock_irqrestore(&um_kvm_v2_record_lock, flags);

	rec->state = KVM_V2_RECORD_REPLAYING;
	rec->entries_replayed = 0;	/* replay starts from cursor 0 */

	if (need_register)
		static_branch_enable(&um_kvm_v2_record_enabled);

	mutex_unlock(&rec->lock);

	pr_info("um: kvm-v2 record_replay: armed (buffer_used=%zu, strict_replay=%d)\n",
		rec->buffer_used, rec->strict_replay);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_replay);

/**
 * kvm_v2_record_set_strict_replay - toggle strict replay mode.
 * @rec:    container to mutate; must be non-NULL.
 * @strict: true for fail-stop on divergence/end-of-log; false for
 *          fall-through-to-live-syscall (loose mode).
 *
 * Strict mode is the default (set by _alloc); it's what makes the
 * replay primitive a reproducer harness — divergence kills the task
 * instead of silently running live syscalls.
 *
 * Loose mode (v1's original behaviour) is occasionally useful for
 * "extend recording past end-of-log" workflows.
 *
 * Returns 0 on success, -EINVAL on NULL @rec.
 *
 * Mirror of v1's kvm_record_set_strict_replay (kvm-v1-archive/
 * record.c:1268-1283). v1 looked up the active container via a
 * global; the v2 port takes an explicit @rec argument so the test
 * harness can mutate the flag without going through debugfs.
 */
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

/**
 * kvm_v2_record_strict_replay - read the strict-replay flag.
 * @rec: container to inspect; may be NULL (returns false).
 *
 * Phase 3's consume hook calls this to decide whether to fail-stop
 * (SIGSEGV) or fall through to live handle_syscall on divergence /
 * end-of-log. Phase 1 callers (none in-tree today) get a snapshot of
 * the flag; the test harness reads it directly off the field too.
 */
bool kvm_v2_record_strict_replay(const struct kvm_v2_record *rec)
{
	if (!rec)
		return false;
	return rec->strict_replay;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_strict_replay);

/*
 * Phase 2 stub — observation hook.
 *
 * The Phase 2 syscall_trap.c integration calls this from the
 * post-handle_syscall arm of kvm_v2_handle_io_trap, gated by
 * static_branch_unlikely(&um_kvm_v2_record_enabled). The hook reads
 * @rec's state under @rec->lock and either appends an entry to the
 * buffer (when RECORDING) or no-ops (any other state — the active-
 * record slot may have advanced past RECORDING while a vmexit was
 * in flight).
 *
 * Phase 1 ships the function so the static-key gate can be flipped
 * end-to-end (the gate's hot path is in syscall_trap.c, but the
 * symbol resolution + the linkage have to be live by Phase 1).
 */
void kvm_v2_record_observe_syscall(struct kvm_v2_record *rec,
				   unsigned long syscall_nr,
				   long ret_value,
				   const struct uml_pt_regs *regs)
{
	/* Phase 2 — fill in: append a KVM_V2_REPLAY_SYSCALL entry to
	 * @rec->buffer when rec->state == KVM_V2_RECORD_RECORDING.
	 * For now suppress unused-arg warnings without emitting any
	 * code (the gate above means this is unreachable until Phase 2).
	 */
	(void)rec;
	(void)syscall_nr;
	(void)ret_value;
	(void)regs;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_observe_syscall);

/*
 * Phase 3 stub — replay-side consume hook.
 *
 * Returns 0 ("entry not served — fall through to live syscall") in
 * Phase 1; Phase 3 fills in the FIFO walk + -EILSEQ on NR-mismatch
 * + -ENODATA on end-of-log. The pre-handle_syscall arm of
 * kvm_v2_handle_io_trap calls this gated by the same static key as
 * the observe hook; the @ret_value out-param carries the served
 * return value when the function returns > 0.
 */
int kvm_v2_record_consume_syscall(struct kvm_v2_record *rec,
				  unsigned long syscall_nr,
				  long *ret_value)
{
	/* Phase 3 — fill in: pop next KVM_V2_REPLAY_SYSCALL entry,
	 * validate NR matches, return entry->ret_value via out-param.
	 */
	(void)rec;
	(void)syscall_nr;
	(void)ret_value;
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_consume_syscall);
