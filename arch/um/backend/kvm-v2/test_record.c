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
#include <linux/types.h>

#include "kvm_v2_backend.h"

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

static struct kunit_case kvm_v2_record_test_cases[] = {
	KUNIT_CASE(test_kvm_v2_record_basic),
	KUNIT_CASE(test_kvm_v2_record_state_transitions),
	{}
};

static struct kunit_suite kvm_v2_record_test_suite = {
	.name		= "kvm_v2_record",
	.test_cases	= kvm_v2_record_test_cases,
};

kunit_test_suite(kvm_v2_record_test_suite);

MODULE_DESCRIPTION("UML kvm-v2 record/replay KUnit tests (memo 27 Phase 1)");
MODULE_LICENSE("GPL");
