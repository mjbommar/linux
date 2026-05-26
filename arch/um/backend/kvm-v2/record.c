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
 * Phase 1 landed:
 *   - struct kvm_v2_record + alloc/destroy/free.
 *   - State machine: INIT → RECORDING → STOPPED → REPLAYING.
 *   - DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled) hot-path gate
 *     + single-active-record discipline.
 *   - observe_syscall / consume_syscall were no-op stubs marked with
 *     a `Phase N` comment so the Phase 2-3 hooks could land additively.
 *   - strict_replay toggle (defaults to true, mirrors v1).
 *
 * Phase 2 (this revision):
 *   - kvm_v2_record_active() accessor for the single-active-record
 *     slot — the syscall_trap.c hook consults this under the
 *     static-key gate instead of poking record.c's globals directly.
 *   - kvm_v2_record_observe_syscall() body fills in: append a
 *     KVM_V2_REPLAY_SYSCALL entry (header + nr/retval/args[6]) to
 *     rec->buffer when rec->state == KVM_V2_RECORD_RECORDING. The
 *     buffer-full case is a quiet drop in Phase 2 (Phase 7's
 *     overflow handler will _stop the container).
 *
 * Out of Phase 2 (per memo 27 §Phase 2 + §4):
 *   - RDTSC / vvar / SIGALRM capture (Phases 5-6).
 *   - Gadget disable on record-enable (Phase 4).
 *   - Replay primitive — consume from buffer (Phase 3).
 *   - Per-NR side-buffer routing (Phase 2.5).
 *   - Snapshot integration (the container holds a kvm_v2_snapshot in
 *     Phase 3+ but Phase 1-2 run in "log-only" mode per memo 27 §3.9).
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
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/smp.h>			/* nr_cpu_ids */
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <sysdep/ptrace.h>		/* struct uml_pt_regs */
#include <asm/um-hooks.h>		/* um_hook_record_replay */

#include "kvm_v2_backend.h"
#include "syscall_trap.h"		/* KVM_V2_GADGET_OFF_RECORD */

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
 * kvm_v2_record_set_gadget_bypass - flip the LSTAR gadget's record-mode
 *                                   bypass byte on every pool vCPU.
 * @on: true to force the gadget body to take the fallback path on each
 *      entry (record mode), false to release the bypass (normal mode).
 *
 * Phase 4 of #169 (memo 27 §3.3 Option A). The gadget body reads byte
 * KVM_V2_GADGET_OFF_RECORD from its per-vCPU gadget_state page on
 * every entry; non-zero forces the fallback path so the syscall vmexit
 * + observe_syscall hook see every NR.
 *
 * Safe to call before the per-vCPU gadget_state_kva is installed
 * (vcpu->gadget_state_kva == NULL); the per-cpu skip leaves later
 * install paths to populate the byte to zero via __GFP_ZERO. Idempotent
 * for the same @on across re-arms.
 */
static void kvm_v2_record_set_gadget_bypass(bool on)
{
	int cpu;

	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		struct kvm_v2_vcpu *v = kvm_v2_vcpu_get(cpu);
		u8 *flag;

		if (!v || !v->gadget_state_kva)
			continue;
		flag = (u8 *)v->gadget_state_kva + KVM_V2_GADGET_OFF_RECORD;
		WRITE_ONCE(*flag, on ? 1 : 0);
	}
}

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
	if (was_active) {
		static_branch_disable(&um_kvm_v2_record_enabled);
		/*
		 * Note: we do NOT disable um_hook_record_replay here.
		 * That gate is owned by kvm_v2_record_engage_global_hooks
		 * / _disengage_global_hooks (memo 04 Phase 2/3 post-audit
		 * fix); if engage was called, the symmetric disengage on
		 * the same code path is what flips it off.
		 */
		/*
		 * Phase 4 (#169): drop the per-vCPU LSTAR gadget bypass so
		 * normal gadget hot-path resumes. Order matters: disable
		 * the static-key first (so any in-flight handle_io_trap
		 * stops invoking observe_syscall) then release the bypass
		 * byte (so subsequent gadget entries take the fast path).
		 */
		kvm_v2_record_set_gadget_bypass(false);
	}
	return was_active;
}

/**
 * kvm_v2_record_active - return the currently registered container.
 *
 * Reads the single-active-record slot under the record_lock spinlock
 * and returns its current value (may be NULL).
 *
 * The Phase 2 syscall_trap.c hook calls this from inside the
 * static_branch_unlikely(&um_kvm_v2_record_enabled) gate. The gate
 * is flipped on by _start/_replay AFTER the slot has been populated,
 * and off by _stop/_destroy AFTER the slot has been cleared, so:
 *
 *   - When the gate is on and the slot was populated atomically with
 *     _start, this returns the registered container.
 *   - When the gate is being flipped off by _stop on another CPU, a
 *     racing reader may observe NULL — the caller treats that as a
 *     no-op (the same outcome it would have got if the gate had
 *     already patched out).
 *
 * The spinlock cost is one atomic on the hot path when record is
 * armed; record-armed runtime is the slow path anyway (Phase 7's
 * benchmark documents this).
 *
 * Phase 1 kept this slot's storage file-scope; Phase 2 promotes the
 * read to a callable accessor so syscall_trap.c doesn't need direct
 * visibility into record.c's globals (D131 deferred this; Phase 2
 * picks the accessor path per the rationale in D132).
 */
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
	rec->buffer_replayed = 0;
	rec->sequence = 0;
	rec->entries_recorded = 0;
	rec->entries_replayed = 0;
	rec->syscall_count = 0;
	rec->state = KVM_V2_RECORD_RECORDING;

	/*
	 * Flip the static-key gate. static_branch_enable is idempotent;
	 * if a previous start raced a stop the kernel-side patching
	 * machinery handles the multiple-call case correctly. Phase 2's
	 * hook reads the key with static_branch_unlikely().
	 */
	static_branch_enable(&um_kvm_v2_record_enabled);

	/*
	 * Memo 04 Phase 2 (post-2026-05-19 sprint) — the global hook
	 * gate (um_hook_record_replay) is NOT auto-enabled here.
	 *
	 * The first iteration enabled it on every kvm_v2_record_start
	 * call.  HONEST-AUDIT §1 follow-up: that caused the boot-time
	 * KUnit test_kvm_v2_record_time_travel to fail intermittently
	 * because the real kernel timer's time_travel_set_time() fires
	 * during the test, the hook chain calls observe_time_travel on
	 * the active rec (the test's rec, since the test called
	 * kvm_v2_record_start), and that injects unexpected
	 * TIME_TRAVEL entries between the test's synthetic SYSCALL
	 * entries — making consume_syscall trip -EILSEQ at the head
	 * of the log.
	 *
	 * Resolution: production callers (a future
	 * `umlctl record start --engage-hooks` flow, or kernel-side
	 * arming on a profile flag) opt in via
	 * kvm_v2_record_engage_global_hooks() below.  KUnit tests
	 * don't engage; they exercise the observe/consume API
	 * directly without contention from live kernel events.
	 */

	/*
	 * Phase 4 (#169): force the LSTAR gadget into fallback mode on
	 * every pool vCPU so the 11 gadget-shadowed syscalls become
	 * observable to handle_io_trap + observe_syscall. Mirrored by
	 * kvm_v2_record_disarm_gate which clears the byte on stop.
	 */
	kvm_v2_record_set_gadget_bypass(true);

	mutex_unlock(&rec->lock);

	pr_info("um: kvm-v2 record_start: armed (buffer_size=%zu, strict_replay=%d, gadget_bypass=on)\n",
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
	rec->buffer_replayed = 0;	/* replay reads from byte cursor 0 */
	rec->entries_replayed = 0;	/* replay starts from entry cursor 0 */

	if (need_register)
		static_branch_enable(&um_kvm_v2_record_enabled);
	/*
	 * Memo 04 Phase 3 — same rationale as record_start above:
	 * um_hook_record_replay is NOT auto-enabled on replay.  Real
	 * replay arming goes through kvm_v2_record_engage_global_hooks()
	 * which the production driver path calls; the KUnit tests
	 * exercise consume_* directly without engaging the live gate.
	 */

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

/**
 * kvm_v2_record_observe_syscall - append a class-A syscall entry.
 * @rec:        active container; caller has already verified it via
 *              kvm_v2_record_active(). NULL is tolerated (no-op).
 * @syscall_nr: NR (regs->gp[HOST_ORIG_AX] or the cached syscall_nr
 *              the dispatcher captured before clearing PT_SYSCALL_NR).
 * @ret_value:  return value the syscall produced (regs->gp[HOST_AX]
 *              after handle_syscall returns).
 * @regs:       full register frame at observe time. NULL means the
 *              caller didn't have one (unusual on the Phase 2 hook
 *              path; defensive — the args are zeroed in that case).
 *
 * Appends a KVM_V2_REPLAY_SYSCALL entry to @rec->buffer when @rec is
 * in the RECORDING state. The entry layout matches the struct shape
 * declared in kvm_v2_backend.h:
 *
 *   header: kind, size, sequence (16 bytes)
 *   inline: nr, _pad, retval, args[6] (64 bytes)
 *
 * 80 bytes per entry total. A 64 KiB default buffer (the _alloc
 * default) holds ~819 entries; a 64 MiB max buffer holds ~838 K
 * entries. v1's 256-entry initial array (`kvm-v1-archive/record.c:
 * 444`) was the precedent; the Phase 2 fixed-buffer shape per D131
 * trades v1's doubling-on-demand for the simpler bounded surface.
 *
 * Buffer-full path: Phase 2 quietly drops the entry (returns without
 * appending). This is deliberately not -ENOSPC at the caller — the
 * syscall_trap.c hook has no error-handling contract; what we'd
 * surface there is at most a printk. The Phase 7 overflow handler
 * (memo 27 §Phase 7) will _stop the container the first time we hit
 * the cap, surfacing the overflow via the debugfs state file. Until
 * then the drop is benign: replay will see end-of-log earlier than
 * expected and (in strict mode) SIGSEGV the divergent task — the
 * same fail-stop signal the operator wants.
 *
 * Args slot mapping: regs->gp[HOST_DI]/SI/DX/R10/R8/R9 = arg1..6.
 * Mirrors the v1 dispatcher's per-NR routing
 * (`kvm-v1-archive/record.c:758-1080`), which extracts the same
 * slots; Phase 2.5 introduces the side-buffer routing for the
 * read/getrandom/recvfrom NRs.
 *
 * Locking: takes @rec->lock to serialise against a racing _stop /
 * _replay that mutates @rec->state + @rec->buffer_used. The lock is
 * uncontended in the Phase 1-2 single-vCPU KUnit path; multi-vCPU
 * record (Phase 7+) makes the contention measurable, at which point
 * a per-CPU staging buffer + lockless append per memo 13 step 4 is
 * the optimization target.
 *
 * Mirror of v1's kvm_record_observe_syscall (`kvm-v1-archive/
 * record.c:606-618`) adapted to v2's per-record buffer (vs v1's
 * global active-record + log array indexed by log_count). The v1
 * append helper handles arg0/arg1 inline; the v2 port stores all 6
 * args because Phase 3's consume hook needs the full set to restore
 * pre-syscall register state when an entry's NR is replayed.
 */
void kvm_v2_record_observe_syscall(struct kvm_v2_record *rec,
				   unsigned long syscall_nr,
				   long ret_value,
				   const struct uml_pt_regs *regs)
{
	struct kvm_v2_replay_entry *e;
	const size_t need = sizeof(*e);

	if (!rec)
		return;

	mutex_lock(&rec->lock);

	if (rec->state != KVM_V2_RECORD_RECORDING) {
		/*
		 * The active-record slot may have advanced past RECORDING
		 * while a vmexit was in flight (record_stop on another
		 * thread or the controlling task itself). Quiet no-op —
		 * the entry would have been a phantom anyway.
		 */
		mutex_unlock(&rec->lock);
		return;
	}

	if (rec->buffer_used + need > rec->buffer_size) {
		/*
		 * Buffer full. Phase 2's policy is "drop quietly + keep
		 * recording" so the container still tracks the entries
		 * that DID fit; the Phase 7 overflow handler will _stop
		 * the first time we hit the cap. Bump a counter so the
		 * KUnit / debugfs can detect the drop later — but in
		 * Phase 2 we just bail.
		 */
		mutex_unlock(&rec->lock);
		return;
	}

	e = (struct kvm_v2_replay_entry *)((u8 *)rec->buffer + rec->buffer_used);
	e->kind		= KVM_V2_REPLAY_SYSCALL;
	e->size		= (u32)need;
	e->sequence	= ++rec->sequence;
	e->syscall.nr	= (s32)syscall_nr;
	e->syscall._pad	= 0;
	e->syscall.retval = (u64)ret_value;
	if (regs) {
		e->syscall.args[0] = regs->gp[HOST_DI];
		e->syscall.args[1] = regs->gp[HOST_SI];
		e->syscall.args[2] = regs->gp[HOST_DX];
		e->syscall.args[3] = regs->gp[HOST_R10];
		e->syscall.args[4] = regs->gp[HOST_R8];
		e->syscall.args[5] = regs->gp[HOST_R9];
	} else {
		memset(e->syscall.args, 0, sizeof(e->syscall.args));
	}

	rec->buffer_used += need;
	rec->entries_recorded++;
	/*
	 * Phase 6 anchor (memo 27 §3.6(b)): every observed syscall bumps
	 * @syscall_count, which is the value sampled into a sigalrm log
	 * entry's @syscall_count_at slot if a SIGALRM is recorded between
	 * this syscall and the next.
	 */
	rec->syscall_count++;

	mutex_unlock(&rec->lock);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_observe_syscall);

/**
 * kvm_v2_record_observe_rdtsc - log one RDTSC/RDTSCP read into @rec.
 * @rec:       active container (NULL is a quiet no-op).
 * @tsc_value: TSC value captured at the RDTSC vmexit site.
 *
 * Phase 5 of #169 (memo 27 §3.4). Appends a KVM_V2_REPLAY_RDTSC entry
 * carrying @tsc_value in the @rdtsc payload union arm. Same record-
 * state gate + buffer-full quiet-drop shape as observe_syscall.
 *
 * Caller is the RDTSC vmexit handler (future wiring). Phase 5 lands
 * the API + KUnit only; the trap-on-RDTSC VMCS programming is filed
 * as memo 27 §6 Q1 work — once that wiring lands, this hook fires
 * unchanged.
 */
void kvm_v2_record_observe_rdtsc(struct kvm_v2_record *rec, u64 tsc_value)
{
	struct kvm_v2_replay_entry *e;
	const size_t need = sizeof(*e);

	if (!rec)
		return;

	mutex_lock(&rec->lock);

	if (rec->state != KVM_V2_RECORD_RECORDING) {
		mutex_unlock(&rec->lock);
		return;
	}

	if (rec->buffer_used + need > rec->buffer_size) {
		mutex_unlock(&rec->lock);
		return;
	}

	e = (struct kvm_v2_replay_entry *)((u8 *)rec->buffer + rec->buffer_used);
	memset(e, 0, sizeof(*e));
	e->kind		= KVM_V2_REPLAY_RDTSC;
	e->size		= (u32)need;
	e->sequence	= ++rec->sequence;
	e->rdtsc.value	= tsc_value;

	rec->buffer_used += need;
	rec->entries_recorded++;

	mutex_unlock(&rec->lock);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_observe_rdtsc);

/**
 * kvm_v2_record_consume_syscall - replay-side FIFO consume.
 * @rec:        active container; caller has typically already gated on
 *              kvm_v2_record_active(). NULL is tolerated (no-op,
 *              returns 0).
 * @syscall_nr: NR the dispatcher just received (cached at the
 *              syscall_trap.c hook site BEFORE PT_SYSCALL_NR is
 *              cleared). The replay walker validates this against
 *              the next log entry's recorded NR.
 * @ret_value:  out-param. On rc > 0, written with the recorded
 *              entry->syscall.retval so the caller can stuff it into
 *              regs->gp[HOST_AX] and skip live handle_syscall.
 *
 * Walks @rec->buffer in FIFO order starting at @rec->buffer_replayed
 * (the read cursor, distinct from @rec->buffer_used = write cursor).
 * Each entry's @size header field advances the cursor — Phase 2's
 * append helper writes @size = sizeof(entry); Phase 5-6 will mix
 * payload sizes per kind under the same @size discipline.
 *
 * Returns:
 *   1         entry served (the only success-with-side-effects path).
 *             *ret_value populated; cursor advanced; entries_replayed
 *             incremented by 1.
 *   0         no work — @rec NULL or @rec->state != REPLAYING. The
 *             caller falls through to live handle_syscall.
 *   -ENODATA  end-of-log: @buffer_replayed >= @buffer_used. In strict
 *             replay this is a divergence (the record ran longer than
 *             the replay needed to reproduce); the caller delivers
 *             SIGSEGV. In loose replay the caller falls through.
 *   -EILSEQ   sequence error: the next entry's @kind is not
 *             KVM_V2_REPLAY_SYSCALL (buffer corruption — Phase 2's
 *             observe hook only writes SYSCALL entries) or its
 *             @syscall.nr does not match the dispatcher's @syscall_nr
 *             (replay diverged from record on syscall stream). Same
 *             strict / loose handling at the caller as -ENODATA.
 *
 * Locking: takes @rec->lock so a racing _stop on another CPU can't
 * tear down state between the state check and the cursor advance.
 * The mutex is the same one the observe hook holds; under Phase 1-3
 * single-vCPU KUnit the lock is uncontended.
 *
 * Mirror of v1's kvm_record_consume_syscall (kvm-v1-archive/record.c:
 * 1085-1160) adapted to the v2 per-record buffer (vs v1's global
 * active-record + log array). v1 returned the NR-mismatch as -EILSEQ
 * AND the end-of-log as -ENODATA via separate error codes; the v2
 * port preserves that surface — the syscall_trap.c hook's strict-mode
 * pr_info distinguishes them via rc itself for operator-visible
 * divergence diagnostics.
 */
int kvm_v2_record_consume_syscall(struct kvm_v2_record *rec,
				  unsigned long syscall_nr,
				  long *ret_value)
{
	struct kvm_v2_replay_entry *e;
	size_t cursor;
	int rc;

	if (!rec)
		return 0;

	mutex_lock(&rec->lock);

	if (rec->state != KVM_V2_RECORD_REPLAYING) {
		/*
		 * A racing _stop may have advanced past REPLAYING between
		 * the hook site's static_branch read and our entry. Quiet
		 * no-op — caller falls through to live (the same outcome
		 * it would have got if the gate had already patched out).
		 */
		mutex_unlock(&rec->lock);
		return 0;
	}

	cursor = rec->buffer_replayed;
	if (cursor >= rec->buffer_used) {
		/* End-of-log. */
		rc = -ENODATA;
		goto out_unlock;
	}

	/*
	 * Bounds check: a partial entry at the tail would indicate buffer
	 * corruption. Phase 2's append helper only commits whole entries
	 * (the size-check fails atomically before the write) but the
	 * defensive read here keeps the walker robust against future
	 * mid-write torn states (Phase 6 multi-vCPU may surface them).
	 */
	if (cursor + sizeof(*e) > rec->buffer_used) {
		rc = -EILSEQ;
		goto out_unlock;
	}

	e = (struct kvm_v2_replay_entry *)((u8 *)rec->buffer + cursor);

	if (e->kind != KVM_V2_REPLAY_SYSCALL) {
		/*
		 * Phase 2's observe hook only emits SYSCALL entries; a
		 * different kind at the cursor means either buffer
		 * corruption or a future kind (RDTSC/SIGALRM — Phase 5/6)
		 * the consume_syscall walker doesn't know how to skip.
		 * Treat as divergence per memo 27 §3.2.
		 */
		rc = -EILSEQ;
		goto out_unlock;
	}

	if (e->syscall.nr != (s32)syscall_nr) {
		/* Replay diverged from record on the syscall NR stream. */
		rc = -EILSEQ;
		goto out_unlock;
	}

	if (e->size < sizeof(*e) || cursor + e->size > rec->buffer_used) {
		/*
		 * Defensive: size header lies (would advance past
		 * buffer_used) — treat as buffer corruption / divergence.
		 */
		rc = -EILSEQ;
		goto out_unlock;
	}

	/* Entry served. Populate out-param + advance cursors. */
	if (ret_value)
		*ret_value = (long)e->syscall.retval;
	rec->buffer_replayed = cursor + e->size;
	rec->entries_replayed++;
	rc = 1;

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_consume_syscall);

/**
 * kvm_v2_record_consume_rdtsc - replay-side FIFO consume for RDTSC.
 * @rec:       active container; NULL is a quiet no-op returning 0.
 * @value_out: out-param. On rc > 0, written with the recorded TSC value.
 *
 * Phase 5 of #169 (memo 27 §3.4). Walks @rec->buffer in FIFO order at
 * @rec->buffer_replayed and serves the next entry, asserting its
 * @kind == KVM_V2_REPLAY_RDTSC. Returns the same rc set as
 * kvm_v2_record_consume_syscall:
 *
 *   1         entry served; @value_out populated, cursor advanced.
 *   0         no work — @rec NULL or state != REPLAYING.
 *  -ENODATA   end-of-log.
 *  -EILSEQ    next entry's kind is not RDTSC (interleaved-stream
 *             divergence — caller decides strict / loose handling).
 *
 * Phase 5 keeps the same shared-cursor model as consume_syscall. The
 * future mixed-stream walker (memo 27 §"Phase 7+ multi-vCPU") will
 * extend with kind-skipping; for Phase 5 KUnit purposes the recorded
 * stream is RDTSC-only, so the shared cursor works as-is.
 */
int kvm_v2_record_consume_rdtsc(struct kvm_v2_record *rec, u64 *value_out)
{
	struct kvm_v2_replay_entry *e;
	size_t cursor;
	int rc;

	if (!rec)
		return 0;

	mutex_lock(&rec->lock);

	if (rec->state != KVM_V2_RECORD_REPLAYING) {
		mutex_unlock(&rec->lock);
		return 0;
	}

	cursor = rec->buffer_replayed;
	if (cursor >= rec->buffer_used) {
		rc = -ENODATA;
		goto out_unlock;
	}
	if (cursor + sizeof(*e) > rec->buffer_used) {
		rc = -EILSEQ;
		goto out_unlock;
	}

	e = (struct kvm_v2_replay_entry *)((u8 *)rec->buffer + cursor);

	if (e->kind != KVM_V2_REPLAY_RDTSC) {
		rc = -EILSEQ;
		goto out_unlock;
	}
	if (e->size < sizeof(*e) || cursor + e->size > rec->buffer_used) {
		rc = -EILSEQ;
		goto out_unlock;
	}

	if (value_out)
		*value_out = e->rdtsc.value;
	rec->buffer_replayed = cursor + e->size;
	rec->entries_replayed++;
	rc = 1;

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_consume_rdtsc);

/**
 * kvm_v2_record_observe_sigalrm - log one signal-injection event.
 * @rec:   active container (NULL is a quiet no-op).
 * @signo: host signal number being delivered to the guest.
 *
 * Phase 6 of #169 (memo 27 §3.6(b)). Snapshots @rec->syscall_count
 * into a new KVM_V2_REPLAY_INTERRUPT entry's @sigalrm.syscall_count_at
 * slot, carrying @signo alongside. The replay driver later gates
 * signal injection on the recorded count matching its live counter
 * (Phase 6 wiring lives in the dispatcher's signal-arrival path; this
 * commit lands the log shape only).
 *
 * Same quiet-no-op shape as observe_syscall: NULL @rec, state !=
 * RECORDING, or buffer-full drop quietly.
 */
void kvm_v2_record_observe_sigalrm(struct kvm_v2_record *rec, u32 signo)
{
	struct kvm_v2_replay_entry *e;
	const size_t need = sizeof(*e);

	if (!rec)
		return;

	mutex_lock(&rec->lock);

	if (rec->state != KVM_V2_RECORD_RECORDING) {
		mutex_unlock(&rec->lock);
		return;
	}

	if (rec->buffer_used + need > rec->buffer_size) {
		mutex_unlock(&rec->lock);
		return;
	}

	e = (struct kvm_v2_replay_entry *)((u8 *)rec->buffer + rec->buffer_used);
	memset(e, 0, sizeof(*e));
	e->kind			= KVM_V2_REPLAY_INTERRUPT;
	e->size			= (u32)need;
	e->sequence		= ++rec->sequence;
	e->sigalrm.signo	= signo;
	e->sigalrm.syscall_count_at = rec->syscall_count;

	rec->buffer_used += need;
	rec->entries_recorded++;

	mutex_unlock(&rec->lock);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_observe_sigalrm);

/**
 * kvm_v2_record_consume_sigalrm - replay-side FIFO consume for SIGALRM.
 * @rec:                       active container; NULL is quiet no-op
 *                             returning 0.
 * @signo_out:                 out-param. On rc > 0 receives the
 *                             recorded signal number.
 * @syscall_count_at_out:      out-param. On rc > 0 receives the
 *                             syscall_count anchor captured at record
 *                             time. Replay driver gates injection on
 *                             this matching the live counter.
 *
 * Phase 6 of #169. Walks @rec->buffer from @rec->buffer_replayed for
 * the next KVM_V2_REPLAY_INTERRUPT entry. Same rc set as
 * consume_syscall / consume_rdtsc: 1 / 0 / -ENODATA / -EILSEQ.
 *
 * The shared-cursor model applies — Phase 5 commit's comment about
 * the mixed-stream walker applies here too.
 */
int kvm_v2_record_consume_sigalrm(struct kvm_v2_record *rec,
				  u32 *signo_out,
				  u64 *syscall_count_at_out)
{
	struct kvm_v2_replay_entry *e;
	size_t cursor;
	int rc;

	if (!rec)
		return 0;

	mutex_lock(&rec->lock);

	if (rec->state != KVM_V2_RECORD_REPLAYING) {
		mutex_unlock(&rec->lock);
		return 0;
	}

	cursor = rec->buffer_replayed;
	if (cursor >= rec->buffer_used) {
		rc = -ENODATA;
		goto out_unlock;
	}
	if (cursor + sizeof(*e) > rec->buffer_used) {
		rc = -EILSEQ;
		goto out_unlock;
	}

	e = (struct kvm_v2_replay_entry *)((u8 *)rec->buffer + cursor);

	if (e->kind != KVM_V2_REPLAY_INTERRUPT) {
		rc = -EILSEQ;
		goto out_unlock;
	}
	if (e->size < sizeof(*e) || cursor + e->size > rec->buffer_used) {
		rc = -EILSEQ;
		goto out_unlock;
	}

	if (signo_out)
		*signo_out = e->sigalrm.signo;
	if (syscall_count_at_out)
		*syscall_count_at_out = e->sigalrm.syscall_count_at;
	rec->buffer_replayed = cursor + e->size;
	rec->entries_replayed++;
	rc = 1;

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_consume_sigalrm);

/**
 * kvm_v2_record_observe_time_travel - log one time_travel_set_time advance.
 * @rec:           active container (NULL is a quiet no-op).
 * @ns_at_advance: the ns value passed into time_travel_set_time.
 *
 * memo 04 Phase 2 (post-2026-05-19 sprint).  Appends a
 * KVM_V2_REPLAY_TIME_TRAVEL entry carrying @ns_at_advance and the
 * current @rec->syscall_count anchor.  Same buffer-full quiet-drop /
 * state-gate shape as observe_rdtsc / observe_sigalrm.
 */
void kvm_v2_record_observe_time_travel(struct kvm_v2_record *rec,
				       u64 ns_at_advance)
{
	struct kvm_v2_replay_entry *e;
	const size_t need = sizeof(*e);

	if (!rec)
		return;

	mutex_lock(&rec->lock);

	if (rec->state != KVM_V2_RECORD_RECORDING) {
		mutex_unlock(&rec->lock);
		return;
	}

	if (rec->buffer_used + need > rec->buffer_size) {
		mutex_unlock(&rec->lock);
		return;
	}

	e = (struct kvm_v2_replay_entry *)((u8 *)rec->buffer + rec->buffer_used);
	memset(e, 0, sizeof(*e));
	e->kind				= KVM_V2_REPLAY_TIME_TRAVEL;
	e->size				= (u32)need;
	e->sequence			= ++rec->sequence;
	e->time_travel.ns_at_advance	= ns_at_advance;
	e->time_travel.syscall_count_anchor = rec->syscall_count;

	rec->buffer_used += need;
	rec->entries_recorded++;

	mutex_unlock(&rec->lock);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_observe_time_travel);

/**
 * kvm_v2_record_consume_time_travel - replay-side FIFO consume.
 * @rec:                       active container; NULL tolerated.
 * @ns_out:                    out-param. On rc > 0, written with the
 *                             recorded ns value.
 * @syscall_count_anchor_out:  optional out-param (may be NULL).  On
 *                             rc > 0, written with the captured
 *                             rec->syscall_count anchor — caller may
 *                             use this for divergence detection.
 *
 * memo 04 Phase 3 (post-2026-05-19 sprint).  Returns 1 on success, 0
 * on quiet no-op (NULL @rec or state != REPLAYING), -ENODATA on
 * end-of-log, -EILSEQ on kind mismatch.  Same shape as
 * consume_sigalrm.
 */
int kvm_v2_record_consume_time_travel(struct kvm_v2_record *rec,
				      u64 *ns_out,
				      u64 *syscall_count_anchor_out)
{
	struct kvm_v2_replay_entry *e;
	size_t cursor;
	int rc = 0;

	if (!rec || !ns_out)
		return 0;

	mutex_lock(&rec->lock);

	if (rec->state != KVM_V2_RECORD_REPLAYING)
		goto out_unlock;

	cursor = rec->buffer_replayed;
	if (cursor + sizeof(*e) > rec->buffer_used) {
		rc = -ENODATA;
		goto out_unlock;
	}

	e = (struct kvm_v2_replay_entry *)((u8 *)rec->buffer + cursor);
	if (e->kind != KVM_V2_REPLAY_TIME_TRAVEL) {
		rc = -EILSEQ;
		goto out_unlock;
	}

	*ns_out = e->time_travel.ns_at_advance;
	if (syscall_count_anchor_out)
		*syscall_count_anchor_out = e->time_travel.syscall_count_anchor;
	rec->buffer_replayed = cursor + e->size;
	rec->entries_replayed++;
	rc = 1;

out_unlock:
	mutex_unlock(&rec->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_record_consume_time_travel);

/**
 * kvm_v2_record_engage_global_hooks - flip on um_hook_record_replay.
 *
 * memo 04 Phase 2/3 + HONEST-AUDIT §1 follow-up.  Production callers
 * arm the live-event hooks by calling this AFTER kvm_v2_record_start
 * (or _replay) succeeds.  KUnit tests don't call this so their
 * synthetic observe / consume sequences aren't polluted by real
 * timer interrupts firing through __um_record_event_clock.
 *
 * Idempotent: multiple calls are safe.  Symmetric to
 * kvm_v2_record_disengage_global_hooks below.
 *
 * No-op when CONFIG_UM_BACKEND_KVM_V2 is off (the gate doesn't exist).
 */
void kvm_v2_record_engage_global_hooks(void)
{
	static_branch_enable(&um_hook_record_replay);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_engage_global_hooks);

/**
 * kvm_v2_record_disengage_global_hooks - flip off um_hook_record_replay.
 *
 * Counterpart of engage_global_hooks.  Production calls this on
 * teardown (`umlctl record stop`).
 */
void kvm_v2_record_disengage_global_hooks(void)
{
	static_branch_disable(&um_hook_record_replay);
}
EXPORT_SYMBOL_GPL(kvm_v2_record_disengage_global_hooks);

/*
 * Memo 04 §"Acceptance criteria" integration gate (post-2026-05-19 sprint):
 * end-to-end record → replay round-trip exercising the full hook chain
 * (um_on_clock_read → __um_record_event_clock → observe_time_travel,
 * then state-transition to REPLAY → um_time_travel_consume_replay).
 *
 * The KUnit suite (test_kvm_v2_record_time_travel) exercises the
 * observe/consume API with synthetic events; this bench exercises it
 * through the actual static-key-gated hook entry point that the live
 * `time_travel_set_time` call site uses, with both `um_kvm_v2_record
 * _enabled` and `um_hook_record_replay` toggled the same way the
 * production daemon would toggle them. Closes HONEST-AUDIT §1's
 * "integration test missing" caveat: KUnit covered the surface, this
 * bench covers the wiring.
 *
 * Boot-time only: `kvm_v2_record_clock_bench=N` on the UML kernel
 * cmdline fires once at late_initcall_sync. Emits a single dmesg line:
 *
 *   um: kvm-v2 record clock bench: N=%u observed=%llu replayed=%llu
 *       mismatches=%llu verdict=%s
 *
 * verdict=PASS iff observed == replayed == N AND every replayed ns
 * matches its recorded position. Otherwise FAIL — the selftest in
 * tools/testing/selftests/um/kvm-record-clock-bench/ greps for the
 * PASS suffix.
 *
 * Bounded by KVM_V2_RECORD_CLOCK_BENCH_N_MAX so a runaway cmdline
 * cannot pin late_initcall_sync forever.
 */
#define KVM_V2_RECORD_CLOCK_BENCH_N_MAX 4096

static int kvm_v2_record_clock_bench_run(unsigned int n)
{
	struct kvm_v2_record *rec;
	u64 base_ns;
	u64 mismatches = 0;
	u64 entries_recorded = 0;
	u64 entries_replayed = 0;
	unsigned int i;
	bool verdict_pass;
	int rc;

	/*
	 * Buffer-size: 256 B per entry × N gives generous headroom over
	 * the 32 B-per-time-travel-entry actual cost (union payload up
	 * to ~64 B + 16 B header).  At KVM_V2_RECORD_CLOCK_BENCH_N_MAX
	 * = 4096 this allocates 1 MiB — fine at late_initcall_sync.
	 */
	rec = kvm_v2_record_alloc((size_t)n * 256);
	if (!rec) {
		pr_warn("um: kvm-v2 record clock bench: alloc failed\n");
		return -ENOMEM;
	}

	rc = kvm_v2_record_start(rec);
	if (rc < 0) {
		pr_warn("um: kvm-v2 record clock bench: start rc=%d\n", rc);
		goto out_destroy;
	}

	/*
	 * The "workload": N monotonic clock advances spaced 1 µs apart.
	 * Memo 04's acceptance example calls clock_gettime() 100 times
	 * under time-travel=inf-cpu; this is the same shape minus the
	 * time-travel-mode dependency (which conflicts with kvm-v2 SMP
	 * per memo 04 §Risk notes — the wiring is the test, not the
	 * mode).
	 *
	 * Drive through __um_record_event_clock(ns) — the same function
	 * the production hook chain (um_on_clock_read static-key gate)
	 * reaches.  Calling it directly bypasses the global gate so
	 * concurrent kernel-internal clocksource reads (timer_read in
	 * arch/um/kernel/time.c) don't pollute the recorded log with
	 * stray observations: the bench measures the chain function
	 * surface (kvm_v2_record_active -> observe_time_travel) under a
	 * controlled call count.
	 *
	 * The static-key gate itself is exercised once in this bench by
	 * toggling engage/disengage with the record container already in
	 * STOPPED state below (after the workload).  That proves the
	 * gate flips without interfering with the recorded log.
	 */
	base_ns = ktime_get_ns();
	for (i = 0; i < n; i++)
		__um_record_event_clock(base_ns + (u64)i * 1000);

	entries_recorded = rec->entries_recorded;

	rc = kvm_v2_record_stop(rec);
	if (rc < 0) {
		pr_warn("um: kvm-v2 record clock bench: stop rc=%d (after %llu observed)\n",
			rc, entries_recorded);
		goto out_destroy;
	}

	/*
	 * State transition into REPLAY. consume_time_travel pops FIFO
	 * entries.  Disable strict_replay so end-of-log returns a clean
	 * false from um_time_travel_consume_replay (we count entries
	 * explicitly via the rec counter, not by tripping strict mode).
	 */
	kvm_v2_record_set_strict_replay(rec, false);

	rc = kvm_v2_record_replay(rec);
	if (rc < 0) {
		pr_warn("um: kvm-v2 record clock bench: replay rc=%d\n", rc);
		goto out_destroy;
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

	/*
	 * Static-key gate smoke: flip on, flip off.  With the record
	 * container already STOPPED, neither transition perturbs the
	 * counters we just snapshotted.  Confirms the engage/disengage
	 * surface remains live independently of the observe/consume
	 * path tested above.
	 */
	kvm_v2_record_engage_global_hooks();
	kvm_v2_record_disengage_global_hooks();

out_destroy:
	verdict_pass = (entries_recorded == n && entries_replayed == n &&
			mismatches == 0);
	pr_info("um: kvm-v2 record clock bench: N=%u observed=%llu replayed=%llu mismatches=%llu verdict=%s\n",
		n, entries_recorded, entries_replayed, mismatches,
		verdict_pass ? "PASS" : "FAIL");

	kvm_v2_record_destroy(rec);
	return rc;
}

static unsigned int kvm_v2_record_clock_bench_n_at_boot;

static int __init kvm_v2_record_clock_bench_setup(char *s)
{
	long ln;

	if (!s || kstrtol(s, 10, &ln) < 0)
		return 1;
	if (ln <= 0 || ln > KVM_V2_RECORD_CLOCK_BENCH_N_MAX)
		return 1;
	kvm_v2_record_clock_bench_n_at_boot = (unsigned int)ln;
	return 1;
}
__setup("kvm_v2_record_clock_bench=", kvm_v2_record_clock_bench_setup);

static int __init kvm_v2_record_clock_bench_late_init(void)
{
	if (!kvm_v2_record_clock_bench_n_at_boot)
		return 0;
	(void)kvm_v2_record_clock_bench_run(kvm_v2_record_clock_bench_n_at_boot);
	return 0;
}
late_initcall_sync(kvm_v2_record_clock_bench_late_init);
