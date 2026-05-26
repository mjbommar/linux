// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — record/replay KUnit suite (memo 27 §Phase 1).
 *
 * Exercises the Phase 1 state machine end-to-end. No live vCPU
 * dependency: the alloc/start/stop/replay/destroy primitives are
 * pure C state-machine transitions plus the static-key gate flip,
 * so the suite_init fixture is trivial — no priming, no memslot
 * registration, no KVM ioctls.
 *
 * Test scope:
 *
 *   - test_kvm_v2_record_basic: alloc → start → stop → replay →
 *     destroy. Asserts the state field matches expected at every
 *     transition AND that the static-key gate flips as expected
 *     (on after _start, off after _stop, on after _replay,
 *     off after _destroy).
 *
 *   - test_kvm_v2_record_state_transitions: walks the invalid
 *     edges of the state graph and asserts each returns -EINVAL.
 *     Covers: stop-without-start, start-after-start, replay-while-
 *     recording, replay-after-replay, stop-after-stop, replay-from-
 *     INIT.
 *
 * Single-active-record discipline is in play: each test case
 * destroys its container before returning so the next case starts
 * from a clean slot. KUnit serialises cases within a suite so no
 * cross-test interleaving is possible.
 *
 * Phase 2-3 will extend the suite with observe/consume + side-
 * buffer + round-trip cases against a live vCPU; that suite_init
 * will lift from test_snapshot.c's prime_for_kunit fixture.
 */
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/jump_label.h>
#include <linux/smp.h>		/* nr_cpu_ids */
#include <linux/string.h>
#include <linux/types.h>

#include <sysdep/ptrace.h>	/* struct uml_pt_regs + HOST_* slot offsets */

#include "kvm_v2_backend.h"
#include "syscall_trap.h"	/* KVM_V2_GADGET_OFF_RECORD */

/**
 * test_kvm_v2_record_basic - exercise the happy-path state machine.
 *
 * Sequence:
 *   1. alloc with the default buffer size (caller passes 0).
 *   2. assert state == INIT, strict_replay == true, gate off.
 *   3. start → assert state == RECORDING, gate on.
 *   4. stop  → assert state == STOPPED, gate off.
 *   5. replay → assert state == REPLAYING, gate on.
 *   6. destroy → gate off again.
 *
 * The static-key gate assertions catch the entire class of bugs
 * where _start forgets to flip the key (the Phase 2 hook then never
 * fires) or _stop forgets to clear it (the gate stays armed and
 * dispatches non-recorded syscalls into the hook). v1's Review-01
 * P0 was exactly this bug on the _replay path; the Phase 1 KUnit
 * locks the contract from day 1.
 */
static void test_kvm_v2_record_basic(struct kunit *test)
{
	struct kvm_v2_record *rec;

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_INIT);
	KUNIT_EXPECT_TRUE(test, rec->strict_replay);
	KUNIT_EXPECT_NOT_NULL(test, rec->buffer);
	KUNIT_EXPECT_GT(test, rec->buffer_size, (size_t)0);
	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));

	/* INIT → RECORDING */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_RECORDING);
	KUNIT_EXPECT_TRUE(test,
			  static_branch_unlikely(&um_kvm_v2_record_enabled));

	/* RECORDING → STOPPED */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_STOPPED);
	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));

	/* STOPPED → REPLAYING */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_REPLAYING);
	KUNIT_EXPECT_TRUE(test,
			  static_branch_unlikely(&um_kvm_v2_record_enabled));

	/* REPLAYING → STOPPED (via _stop, then destroy from STOPPED). */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_STOPPED);
	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));

	/*
	 * Toggle strict_replay so we exercise the setter on a live
	 * container; default is true, so flip to false then back.
	 */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_set_strict_replay(rec, false), 0);
	KUNIT_EXPECT_FALSE(test, kvm_v2_record_strict_replay(rec));
	KUNIT_EXPECT_EQ(test, kvm_v2_record_set_strict_replay(rec, true), 0);
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_strict_replay(rec));

	kvm_v2_record_destroy(rec);

	/*
	 * Post-destroy the gate must be off — even if the container
	 * was REPLAYING when destroy fired (the test path above stops
	 * first, but the destroy contract has to handle the
	 * caller-forgot-to-stop case too).
	 */
	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));
}

/**
 * test_kvm_v2_record_state_transitions - assert invalid edges fail.
 *
 * The state graph (memo 27 §Phase 1):
 *
 *     INIT ─start→ RECORDING ─stop→ STOPPED ─replay→ REPLAYING
 *                                        ↑               │
 *                                        └────stop───────┘
 *
 * Any edge NOT in the graph must return -EINVAL. This test walks
 * the forbidden edges:
 *
 *   1. stop  from INIT       → -EINVAL
 *   2. replay from INIT      → -EINVAL
 *   3. start from RECORDING  → -EINVAL (double-start)
 *   4. replay from RECORDING → -EINVAL (replay-without-stop)
 *   5. stop  from STOPPED    → -EINVAL (double-stop)
 *   6. start from REPLAYING  → -EINVAL
 *   7. replay from REPLAYING → -EINVAL (double-replay)
 *
 * Also asserts NULL-pointer cases return -EINVAL on the public
 * entry points — defensive coverage that catches the entire class
 * of caller-forgot-to-check-alloc bugs.
 */
static void test_kvm_v2_record_state_transitions(struct kunit *test)
{
	struct kvm_v2_record *rec;

	/* NULL handling on the public surface. */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_set_strict_replay(NULL, true),
			-EINVAL);
	KUNIT_EXPECT_FALSE(test, kvm_v2_record_strict_replay(NULL));
	/* _destroy / _free on NULL are no-ops (matches v1 + idiomatic). */
	kvm_v2_record_destroy(NULL);
	kvm_v2_record_free(NULL);

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);

	/* (1) stop from INIT */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(rec), -EINVAL);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_INIT);

	/* (2) replay from INIT */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(rec), -EINVAL);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_INIT);

	/* INIT → RECORDING */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_RECORDING);

	/* (3) start-after-start */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(rec), -EINVAL);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_RECORDING);

	/* (4) replay-without-stop */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(rec), -EINVAL);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_RECORDING);

	/* RECORDING → STOPPED */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_STOPPED);

	/* (5) stop-after-stop */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(rec), -EINVAL);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_STOPPED);

	/* STOPPED → REPLAYING */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_REPLAYING);

	/* (6) start from REPLAYING */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(rec), -EINVAL);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_REPLAYING);

	/* (7) replay-after-replay */
	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(rec), -EINVAL);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_REPLAYING);

	kvm_v2_record_destroy(rec);

	/*
	 * Post-destroy: the static-key gate must be off (the destroy
	 * helper drops the gate if @rec still held it). The next test
	 * case (or the next suite) relies on this clean slate.
	 */
	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));
}

/**
 * test_kvm_v2_record_observe - exercise the Phase 2 observe hook.
 *
 * Drives kvm_v2_record_observe_syscall directly with synthetic args
 * and asserts that the resulting entry in @rec->buffer carries the
 * right kind / nr / retval / args. Bypasses the syscall_trap.c hook
 * site so the suite stays a pure unit test (no live vCPU dependency
 * required to exercise the append path — same shape as the basic /
 * state-transitions cases).
 *
 * Sequence:
 *   1. alloc + start → state RECORDING, buffer_used 0.
 *   2. Build a synthetic uml_pt_regs with HOST_DI..HOST_R9 carrying
 *      a recognisable per-slot sentinel.
 *   3. Call observe_syscall with NR=__NR_read sentinel, retval=42.
 *   4. Walk the buffer head as a struct kvm_v2_replay_entry:
 *      - kind == KVM_V2_REPLAY_SYSCALL.
 *      - size == sizeof(entry).
 *      - sequence == 1 (first append).
 *      - syscall.nr matches the NR we passed.
 *      - syscall.retval matches.
 *      - syscall.args[0..5] match the per-slot sentinels.
 *   5. Assert rec->buffer_used == entry size and
 *      rec->entries_recorded == 1.
 *   6. Second append with NR=__NR_write sentinel; assert
 *      buffer_used == 2 * entry size, entries_recorded == 2,
 *      sequence == 2 on the second entry.
 *
 * Also exercises the STOPPED-state quiet-no-op: after stop(), a
 * follow-on observe_syscall call must not mutate the buffer.
 *
 * Doesn't (yet) exercise the buffer-full path — that's a follow-up
 * case once Phase 7's overflow handler lands the explicit drop
 * counter.
 */
static void test_kvm_v2_record_observe(struct kunit *test)
{
	struct kvm_v2_record *rec;
	struct uml_pt_regs synth_regs;
	struct kvm_v2_replay_entry *entry;
	size_t prev_used;
	u64 prev_recorded;

	/*
	 * Per-slot sentinels: high byte distinguishes the slot, low bytes
	 * are pattern-fillable so a mis-routed slot is loud in the
	 * assertion message.
	 */
	const unsigned long arg_di  = 0xa1a1a1a100000001ULL;
	const unsigned long arg_si  = 0xa2a2a2a200000002ULL;
	const unsigned long arg_dx  = 0xa3a3a3a300000003ULL;
	const unsigned long arg_r10 = 0xa4a4a4a400000004ULL;
	const unsigned long arg_r8  = 0xa5a5a5a500000005ULL;
	const unsigned long arg_r9  = 0xa6a6a6a600000006ULL;
	const unsigned long syscall_nr_read  = 0;	/* __NR_read on x86_64 */
	const unsigned long syscall_nr_write = 1;	/* __NR_write on x86_64 */
	const long retval_read  = 42;
	const long retval_write = -4;			/* -EINTR */

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_ASSERT_EQ(test, rec->state, KVM_V2_RECORD_RECORDING);
	KUNIT_ASSERT_EQ(test, rec->buffer_used, (size_t)0);

	/* kvm_v2_record_active should now return rec under the gate. */
	KUNIT_EXPECT_PTR_EQ(test, kvm_v2_record_active(), rec);
	KUNIT_EXPECT_TRUE(test,
			  static_branch_unlikely(&um_kvm_v2_record_enabled));

	/* Build a synthetic register frame. Zero-init then set arg slots. */
	memset(&synth_regs, 0, sizeof(synth_regs));
	synth_regs.gp[HOST_DI]  = arg_di;
	synth_regs.gp[HOST_SI]  = arg_si;
	synth_regs.gp[HOST_DX]  = arg_dx;
	synth_regs.gp[HOST_R10] = arg_r10;
	synth_regs.gp[HOST_R8]  = arg_r8;
	synth_regs.gp[HOST_R9]  = arg_r9;

	/* First append. */
	kvm_v2_record_observe_syscall(rec, syscall_nr_read,
				      retval_read, &synth_regs);

	KUNIT_EXPECT_EQ(test, rec->buffer_used,
			sizeof(struct kvm_v2_replay_entry));
	KUNIT_EXPECT_EQ(test, rec->entries_recorded, (u64)1);
	KUNIT_EXPECT_EQ(test, rec->sequence, (u64)1);

	entry = (struct kvm_v2_replay_entry *)rec->buffer;
	KUNIT_EXPECT_EQ(test, entry->kind, (u32)KVM_V2_REPLAY_SYSCALL);
	KUNIT_EXPECT_EQ(test, entry->size,
			(u32)sizeof(struct kvm_v2_replay_entry));
	KUNIT_EXPECT_EQ(test, entry->sequence, (u64)1);
	KUNIT_EXPECT_EQ(test, entry->syscall.nr, (s32)syscall_nr_read);
	KUNIT_EXPECT_EQ(test, entry->syscall._pad, (s32)0);
	KUNIT_EXPECT_EQ(test, entry->syscall.retval, (u64)retval_read);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[0], (u64)arg_di);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[1], (u64)arg_si);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[2], (u64)arg_dx);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[3], (u64)arg_r10);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[4], (u64)arg_r8);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[5], (u64)arg_r9);

	/*
	 * Second append: tweak the arg slots so we see distinct values
	 * in the second entry, change NR + retval.
	 */
	prev_used = rec->buffer_used;
	prev_recorded = rec->entries_recorded;

	synth_regs.gp[HOST_DI] = 0xbeef000000000007ULL;
	kvm_v2_record_observe_syscall(rec, syscall_nr_write,
				      retval_write, &synth_regs);

	KUNIT_EXPECT_EQ(test, rec->buffer_used,
			prev_used + sizeof(struct kvm_v2_replay_entry));
	KUNIT_EXPECT_EQ(test, rec->entries_recorded, prev_recorded + 1);
	KUNIT_EXPECT_EQ(test, rec->sequence, (u64)2);

	entry = (struct kvm_v2_replay_entry *)
		((u8 *)rec->buffer + prev_used);
	KUNIT_EXPECT_EQ(test, entry->kind, (u32)KVM_V2_REPLAY_SYSCALL);
	KUNIT_EXPECT_EQ(test, entry->sequence, (u64)2);
	KUNIT_EXPECT_EQ(test, entry->syscall.nr, (s32)syscall_nr_write);
	KUNIT_EXPECT_EQ(test, entry->syscall.retval, (u64)retval_write);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[0],
			(u64)0xbeef000000000007ULL);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[1], (u64)arg_si);

	/*
	 * NULL @regs path: the function must fall back to memset(0) on
	 * the args[] and keep nr/retval correct.
	 */
	prev_used = rec->buffer_used;
	prev_recorded = rec->entries_recorded;

	kvm_v2_record_observe_syscall(rec, 9 /* __NR_mmap */,
				      0x1234, NULL);

	KUNIT_EXPECT_EQ(test, rec->entries_recorded, prev_recorded + 1);
	entry = (struct kvm_v2_replay_entry *)
		((u8 *)rec->buffer + prev_used);
	KUNIT_EXPECT_EQ(test, entry->syscall.nr, (s32)9);
	KUNIT_EXPECT_EQ(test, entry->syscall.retval, (u64)0x1234);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[0], (u64)0);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[5], (u64)0);

	/*
	 * Stop the container; observe_syscall must become a no-op once
	 * the state machine leaves RECORDING. Buffer + counters frozen
	 * at the values they had at stop time.
	 */
	prev_used = rec->buffer_used;
	prev_recorded = rec->entries_recorded;

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, rec->state, KVM_V2_RECORD_STOPPED);

	kvm_v2_record_observe_syscall(rec, syscall_nr_read,
				      retval_read, &synth_regs);

	KUNIT_EXPECT_EQ(test, rec->buffer_used, prev_used);
	KUNIT_EXPECT_EQ(test, rec->entries_recorded, prev_recorded);

	/* NULL @rec — also a quiet no-op. */
	kvm_v2_record_observe_syscall(NULL, 0, 0, NULL);

	/* Active slot must be NULL after _stop disarms the gate. */
	KUNIT_EXPECT_PTR_EQ(test, kvm_v2_record_active(),
			    (struct kvm_v2_record *)NULL);

	kvm_v2_record_destroy(rec);

	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));
}

/**
 * test_kvm_v2_record_strict_replay - exercise the Phase 3 consume hook.
 *
 * Drives kvm_v2_record_consume_syscall directly through a record →
 * replay round trip and asserts:
 *
 *   1. Observed entries come back in FIFO order with the recorded
 *      retval.
 *   2. End-of-log returns -ENODATA.
 *   3. NR-mismatch (replay-time nr != record-time nr) returns
 *      -EILSEQ.
 *
 * No live vCPU dependency — same shape as test_kvm_v2_record_observe:
 * the consume_syscall walker is a pure C state-machine read over the
 * Phase 2 append buffer. The Phase 3 syscall_trap.c hook is exercised
 * indirectly (its contract is "consult kvm_v2_record_active; call
 * consume_syscall; act on the rc"); the hook's strict_replay
 * force_sig(SIGSEGV) path is implicit in the rc < 0 contract this
 * case validates.
 *
 * Sequence — happy path:
 *   1. alloc → start.
 *   2. Observe 5 entries with NR=__NR_getpid (39) + distinct retvals
 *      (4242..4246) so we can detect the FIFO order on consume.
 *   3. stop → assert buffer_used == 5 * sizeof(entry),
 *      entries_recorded == 5.
 *   4. replay → assert state == REPLAYING, buffer_replayed == 0,
 *      entries_replayed == 0.
 *   5. Consume × 5: each call returns 1 with the matching retval;
 *      buffer_replayed advances by sizeof(entry); entries_replayed
 *      grows monotonically.
 *   6. 6th consume → -ENODATA (end-of-log).
 *
 * Sequence — NR divergence:
 *   1. fresh alloc → start.
 *   2. Observe 1 entry with NR=39.
 *   3. stop → replay.
 *   4. Consume with NR=40 (different) → -EILSEQ. Cursor does NOT
 *      advance — operator-visible diagnosis (you can read out the
 *      next entry and see what record had).
 */
static void test_kvm_v2_record_strict_replay(struct kunit *test)
{
	struct kvm_v2_record *rec;
	const unsigned long nr_getpid = 39;
	const unsigned long nr_getppid = 110;
	const long retvals[5] = { 4242, 4243, 4244, 4245, 4246 };
	long got;
	size_t prev_cursor;
	int rc;
	int i;

	/* ---- Happy path: 5-entry FIFO consume + end-of-log. ---- */

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);

	/* Synthesize 5 SYSCALL entries via the observe path. NULL @regs
	 * is fine — Phase 3's consume walker doesn't care about args[],
	 * only kind + size + nr + retval.
	 */
	for (i = 0; i < 5; i++) {
		kvm_v2_record_observe_syscall(rec, nr_getpid,
					      retvals[i], NULL);
	}

	KUNIT_EXPECT_EQ(test, rec->buffer_used,
			5 * sizeof(struct kvm_v2_replay_entry));
	KUNIT_EXPECT_EQ(test, rec->entries_recorded, (u64)5);

	/* Stop → Replay. Read cursor must reset on replay arm. */
	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_REPLAYING);
	KUNIT_EXPECT_EQ(test, rec->buffer_replayed, (size_t)0);
	KUNIT_EXPECT_EQ(test, rec->entries_replayed, (u64)0);

	/* FIFO consume × 5. */
	for (i = 0; i < 5; i++) {
		got = 0;
		prev_cursor = rec->buffer_replayed;
		rc = kvm_v2_record_consume_syscall(rec, nr_getpid, &got);
		KUNIT_EXPECT_EQ(test, rc, 1);
		KUNIT_EXPECT_EQ(test, got, retvals[i]);
		KUNIT_EXPECT_EQ(test, rec->buffer_replayed,
				prev_cursor +
				sizeof(struct kvm_v2_replay_entry));
		KUNIT_EXPECT_EQ(test, rec->entries_replayed, (u64)(i + 1));
	}

	/* 6th consume — end-of-log. */
	got = 0xdead;
	prev_cursor = rec->buffer_replayed;
	rc = kvm_v2_record_consume_syscall(rec, nr_getpid, &got);
	KUNIT_EXPECT_EQ(test, rc, -ENODATA);
	/*
	 * Cursor does NOT advance past end-of-log; the operator can
	 * inspect buffer_replayed to see how far replay got.
	 */
	KUNIT_EXPECT_EQ(test, rec->buffer_replayed, prev_cursor);
	KUNIT_EXPECT_EQ(test, rec->entries_replayed, (u64)5);

	/* NULL @ret_value is tolerated on the cursor-advance path
	 * (caller wanted to skip the live syscall but didn't care about
	 * the retval; pathological but defensible — observed in v1
	 * kselftest harness when only counting consume calls).
	 */
	kvm_v2_record_destroy(rec);

	/* ---- Divergence: NR-mismatch returns -EILSEQ. ---- */

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);

	kvm_v2_record_observe_syscall(rec, nr_getpid, 100, NULL);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

	prev_cursor = rec->buffer_replayed;
	got = 0xbeef;
	rc = kvm_v2_record_consume_syscall(rec, nr_getppid, &got);
	KUNIT_EXPECT_EQ(test, rc, -EILSEQ);
	/*
	 * Cursor does NOT advance on divergence — strict-mode hook will
	 * SIGSEGV the task; the buffer position is preserved for
	 * diagnostic dump.
	 */
	KUNIT_EXPECT_EQ(test, rec->buffer_replayed, prev_cursor);
	KUNIT_EXPECT_EQ(test, rec->entries_replayed, (u64)0);

	/*
	 * Sanity: matching NR after the failed divergence still serves
	 * (the cursor never advanced, so the entry is still there).
	 */
	got = 0;
	rc = kvm_v2_record_consume_syscall(rec, nr_getpid, &got);
	KUNIT_EXPECT_EQ(test, rc, 1);
	KUNIT_EXPECT_EQ(test, got, 100L);

	/* ---- Not-REPLAYING state: consume is a quiet no-op. ---- */

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, rec->state, KVM_V2_RECORD_STOPPED);

	got = 0xabcd;
	rc = kvm_v2_record_consume_syscall(rec, nr_getpid, &got);
	KUNIT_EXPECT_EQ(test, rc, 0);
	/* Out-param untouched on no-op path. */
	KUNIT_EXPECT_EQ(test, got, 0xabcdL);

	/* NULL @rec: also a quiet no-op returning 0. */
	rc = kvm_v2_record_consume_syscall(NULL, nr_getpid, &got);
	KUNIT_EXPECT_EQ(test, rc, 0);

	kvm_v2_record_destroy(rec);

	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));
}

/**
 * test_kvm_v2_record_gadget_bypass - Phase 4 LSTAR gadget bypass byte.
 * @test: KUnit test handle.
 *
 * Asserts that `kvm_v2_record_start` writes the per-vCPU gadget
 * bypass byte (KVM_V2_GADGET_OFF_RECORD) to non-zero on every pool
 * vCPU that has a gadget_state_kva populated, and that
 * `kvm_v2_record_stop` re-clears it.
 *
 * Scope: walks the pool, samples the byte before/after start/stop.
 * Skips cleanly if NO pool vCPU has the gadget state installed —
 * the assertion would have nothing to observe (e.g. early-boot
 * KUnit env where gadget_state_install hasn't run, or a config
 * with CONFIG_UM_BACKEND_KVM_V2_GADGET=n).
 */
static void test_kvm_v2_record_gadget_bypass(struct kunit *test)
{
	struct kvm_v2_record *rec;
	int cpu;
	int seen_state_page = 0;
	int seen_on = 0;
	int seen_off = 0;
	int rc;

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);

	rc = kvm_v2_record_start(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	/* Post-start: every populated state page must show byte != 0. */
	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		struct kvm_v2_vcpu *v = kvm_v2_vcpu_get(cpu);
		const u8 *flag;

		if (!v || !v->gadget_state_kva)
			continue;
		seen_state_page++;
		flag = (const u8 *)v->gadget_state_kva +
			KVM_V2_GADGET_OFF_RECORD;
		if (READ_ONCE(*flag) != 0)
			seen_on++;
	}

	rc = kvm_v2_record_stop(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	/* Post-stop: every populated state page must show byte == 0. */
	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		struct kvm_v2_vcpu *v = kvm_v2_vcpu_get(cpu);
		const u8 *flag;

		if (!v || !v->gadget_state_kva)
			continue;
		flag = (const u8 *)v->gadget_state_kva +
			KVM_V2_GADGET_OFF_RECORD;
		if (READ_ONCE(*flag) == 0)
			seen_off++;
	}

	kvm_v2_record_destroy(rec);

	if (seen_state_page == 0) {
		kunit_skip(test, "no gadget_state_kva on any pool vCPU (kvm-v2 gadget disabled at boot?)");
		return;
	}
	KUNIT_EXPECT_EQ_MSG(test, seen_on, seen_state_page,
			    "record_start did not set bypass byte on %d vCPU(s)",
			    seen_state_page - seen_on);
	KUNIT_EXPECT_EQ_MSG(test, seen_off, seen_state_page,
			    "record_stop did not clear bypass byte on %d vCPU(s)",
			    seen_state_page - seen_off);
}

/**
 * test_kvm_v2_record_rdtsc - Phase 5 RDTSC observe/consume round-trip.
 * @test: KUnit test handle.
 *
 * Sequence:
 *   1. alloc.
 *   2. start.
 *   3. observe_rdtsc N times with a known pattern of TSC values.
 *   4. stop → replay.
 *   5. consume_rdtsc N times, assert each returned value matches the
 *      recorded pattern in the same order.
 *   6. extra consume must return -ENODATA (end-of-log).
 *   7. destroy.
 *
 * Confirms the Phase 5 observe/consume + union-payload plumbing
 * round-trips byte-identically. Independent of the trap-on-RDTSC
 * VMCS programming (memo 27 §6 Q1), which lands separately — the
 * API contract proven here is what that wiring will call into.
 */
static void test_kvm_v2_record_rdtsc(struct kunit *test)
{
	struct kvm_v2_record *rec;
	const u64 pattern[] = {
		0x0000000012345678ULL,
		0xa5a5a5a5cafebabeULL,
		0xfedcba9876543210ULL,
		0x0000000000000001ULL,	/* smallest interesting */
		0xffffffffffffffffULL,	/* saturated */
	};
	u64 got;
	int rc;
	size_t i;

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);

	rc = kvm_v2_record_start(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	for (i = 0; i < ARRAY_SIZE(pattern); i++)
		kvm_v2_record_observe_rdtsc(rec, pattern[i]);

	KUNIT_EXPECT_EQ_MSG(test,
			    rec->entries_recorded, (u64)ARRAY_SIZE(pattern),
			    "expected %zu entries recorded, got %llu",
			    ARRAY_SIZE(pattern), rec->entries_recorded);

	rc = kvm_v2_record_stop(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	rc = kvm_v2_record_replay(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	for (i = 0; i < ARRAY_SIZE(pattern); i++) {
		got = 0;
		rc = kvm_v2_record_consume_rdtsc(rec, &got);
		KUNIT_ASSERT_EQ_MSG(test, rc, 1,
				    "consume_rdtsc[%zu] rc=%d", i, rc);
		KUNIT_EXPECT_EQ_MSG(test, got, pattern[i],
				    "consume_rdtsc[%zu] got %#llx expected %#llx",
				    i, got, pattern[i]);
	}

	/* End-of-log: extra consume must return -ENODATA. */
	got = 0xdeadbeefdeadbeefULL;
	rc = kvm_v2_record_consume_rdtsc(rec, &got);
	KUNIT_EXPECT_EQ_MSG(test, rc, -ENODATA,
			    "consume past end returned %d (want -ENODATA)", rc);

	kvm_v2_record_destroy(rec);
}

/**
 * test_kvm_v2_record_sigalrm - Phase 6 SIGALRM observe/consume round-trip.
 * @test: KUnit test handle.
 *
 * Verifies the Phase 6 anchor mechanism: rec->syscall_count bumps on
 * each observe_syscall, the captured count is reflected in a sigalrm
 * entry's @syscall_count_at field, and consume_sigalrm returns the
 * pair byte-identically.
 *
 * Sequence:
 *   1. alloc + start.
 *   2. Interleave a 3-syscall + sigalrm + 2-syscall + sigalrm pattern.
 *      After the first sigalrm, syscall_count_at must == 3; after the
 *      second, syscall_count_at must == 5.
 *   3. stop → replay.
 *   4. consume_syscall × 3, consume_sigalrm, consume_syscall × 2,
 *      consume_sigalrm — assert each rc == 1 and the sigalrm anchors
 *      match the expected counts.
 *
 * Independent of the actual host signal-interception wiring. The
 * API contract proven here is what that wiring will call into.
 */
#include <linux/signal.h>	/* SIGALRM */
static void test_kvm_v2_record_sigalrm(struct kunit *test)
{
	struct kvm_v2_record *rec;
	const s32 nr_getpid = 39;
	struct uml_pt_regs regs;
	u32 signo;
	u64 anchor;
	long retval;
	int rc;
	int i;

	memset(&regs, 0, sizeof(regs));

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);

	rc = kvm_v2_record_start(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	/* First sigalrm should anchor at syscall_count == 3. */
	for (i = 0; i < 3; i++)
		kvm_v2_record_observe_syscall(rec, nr_getpid,
					      (long)(1000 + i), &regs);
	kvm_v2_record_observe_sigalrm(rec, SIGALRM);

	/* Second sigalrm at syscall_count == 5. */
	for (i = 3; i < 5; i++)
		kvm_v2_record_observe_syscall(rec, nr_getpid,
					      (long)(1000 + i), &regs);
	kvm_v2_record_observe_sigalrm(rec, SIGALRM);

	KUNIT_EXPECT_EQ_MSG(test, rec->syscall_count, 5ULL,
			    "expected syscall_count=5, got %llu",
			    rec->syscall_count);

	rc = kvm_v2_record_stop(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	rc = kvm_v2_record_replay(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	/* Walk: 3 syscall + 1 sigalrm + 2 syscall + 1 sigalrm. */
	for (i = 0; i < 3; i++) {
		retval = 0;
		rc = kvm_v2_record_consume_syscall(rec, nr_getpid, &retval);
		KUNIT_ASSERT_EQ_MSG(test, rc, 1,
				    "consume_syscall[%d] rc=%d", i, rc);
	}
	signo = 0; anchor = 0;
	rc = kvm_v2_record_consume_sigalrm(rec, &signo, &anchor);
	KUNIT_ASSERT_EQ(test, rc, 1);
	KUNIT_EXPECT_EQ(test, signo, (u32)SIGALRM);
	KUNIT_EXPECT_EQ_MSG(test, anchor, 3ULL,
			    "first sigalrm anchor expected 3, got %llu",
			    anchor);

	for (i = 0; i < 2; i++) {
		retval = 0;
		rc = kvm_v2_record_consume_syscall(rec, nr_getpid, &retval);
		KUNIT_ASSERT_EQ_MSG(test, rc, 1,
				    "consume_syscall[%d] (post-sigalrm) rc=%d",
				    i, rc);
	}
	signo = 0; anchor = 0;
	rc = kvm_v2_record_consume_sigalrm(rec, &signo, &anchor);
	KUNIT_ASSERT_EQ(test, rc, 1);
	KUNIT_EXPECT_EQ(test, signo, (u32)SIGALRM);
	KUNIT_EXPECT_EQ_MSG(test, anchor, 5ULL,
			    "second sigalrm anchor expected 5, got %llu",
			    anchor);

	/* End-of-log. */
	rc = kvm_v2_record_consume_sigalrm(rec, &signo, &anchor);
	KUNIT_EXPECT_EQ(test, rc, -ENODATA);

	kvm_v2_record_destroy(rec);
}

/**
 * test_kvm_v2_record_time_travel - memo 04 Phase 4 round-trip.
 * @test: KUnit test handle.
 *
 * Mirrors test_kvm_v2_record_rdtsc but for time_travel_set_time
 * advances.  Verifies that the (ns_at_advance, syscall_count_anchor)
 * pair round-trips byte-identically through observe + consume, and
 * that the anchor reflects rec->syscall_count at the observe site.
 *
 * Confirms the Phase 2 (observe) + Phase 3 (consume) API contract
 * the post-2026-05-19 sprint memo 04 sets up.  Independent of the
 * live wiring from arch/um/kernel/time.c::time_travel_set_time
 * (which lands as a follow-on: replace the current
 * __um_time_travel_clock hook with a record-aware version that
 * observes / consumes via these APIs).
 */
static void test_kvm_v2_record_time_travel(struct kunit *test)
{
	struct kvm_v2_record *rec;
	const u64 ns_pattern[] = {
		1000ULL,            /* 1 us */
		2500ULL,
		1000000ULL,         /* 1 ms */
		1000000000ULL,      /* 1 s  */
		0xffffffffffffffffULL,  /* saturated */
	};
	u64 ns_got, anchor_got;
	int rc;
	size_t i;

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);

	rc = kvm_v2_record_start(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	/*
	 * Interleave time-travel observes with syscall observes so the
	 * captured syscall_count_anchor varies between entries (same
	 * shape as the SIGALRM anchor coverage).  Anchor[i] == number
	 * of observe_syscall calls before the i-th observe_time_travel.
	 */
	for (i = 0; i < ARRAY_SIZE(ns_pattern); i++) {
		size_t k;

		for (k = 0; k < i; k++) {
			/* Dummy syscall observation — NULL regs is
			 * tolerated by observe_syscall (see existing
			 * test_kvm_v2_record_observe usage).
			 */
			kvm_v2_record_observe_syscall(rec, 39 /* __NR_getpid */,
						      0, NULL);
		}
		kvm_v2_record_observe_time_travel(rec, ns_pattern[i]);
	}

	rc = kvm_v2_record_stop(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	rc = kvm_v2_record_replay(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	/* Walk the stream — interleaved syscall/tt entries. */
	{
		size_t tt_idx = 0;
		size_t syscalls_consumed_total = 0;
		long got;

		for (i = 0; i < ARRAY_SIZE(ns_pattern); i++) {
			size_t k;

			for (k = 0; k < i; k++) {
				got = 0;
				rc = kvm_v2_record_consume_syscall(rec,
								   39, &got);
				KUNIT_ASSERT_EQ_MSG(test, rc, 1,
						    "consume_syscall pre-tt[%zu][%zu] rc=%d",
						    i, k, rc);
				syscalls_consumed_total++;
			}
			ns_got = 0;
			anchor_got = 0;
			rc = kvm_v2_record_consume_time_travel(rec, &ns_got,
							       &anchor_got);
			KUNIT_ASSERT_EQ_MSG(test, rc, 1,
					    "consume_time_travel[%zu] rc=%d",
					    tt_idx, rc);
			KUNIT_EXPECT_EQ_MSG(test, ns_got, ns_pattern[i],
					    "tt[%zu] ns got %#llx expected %#llx",
					    tt_idx, ns_got, ns_pattern[i]);
			KUNIT_EXPECT_EQ_MSG(test, anchor_got,
					    (u64)syscalls_consumed_total,
					    "tt[%zu] anchor got %llu expected %zu",
					    tt_idx, anchor_got,
					    syscalls_consumed_total);
			tt_idx++;
		}
	}

	/* End-of-log. */
	ns_got = 0xdeadbeefdeadbeefULL;
	anchor_got = 0;
	rc = kvm_v2_record_consume_time_travel(rec, &ns_got, &anchor_got);
	KUNIT_EXPECT_EQ_MSG(test, rc, -ENODATA,
			    "consume past end returned %d (want -ENODATA)", rc);

	kvm_v2_record_destroy(rec);
}

static struct kunit_case kvm_v2_record_test_cases[] = {
	KUNIT_CASE(test_kvm_v2_record_basic),
	KUNIT_CASE(test_kvm_v2_record_state_transitions),
	KUNIT_CASE(test_kvm_v2_record_observe),
	KUNIT_CASE(test_kvm_v2_record_strict_replay),
	KUNIT_CASE(test_kvm_v2_record_gadget_bypass),
	KUNIT_CASE(test_kvm_v2_record_rdtsc),
	KUNIT_CASE(test_kvm_v2_record_sigalrm),
	KUNIT_CASE(test_kvm_v2_record_time_travel),
	{}
};

static struct kunit_suite kvm_v2_record_test_suite = {
	.name		= "kvm_v2_record",
	.test_cases	= kvm_v2_record_test_cases,
};

kunit_test_suite(kvm_v2_record_test_suite);

MODULE_DESCRIPTION("UML kvm-v2 record/replay KUnit tests (memo 27 Phase 1)");
MODULE_LICENSE("GPL");
