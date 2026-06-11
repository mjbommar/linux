// SPDX-License-Identifier: GPL-2.0
/*
 * KVM v2 record/replay KUnit coverage.
 *
 * The suite stays independent of a live vCPU. It validates the state machine,
 * active-session ownership, bounded syscall logging, and replay cursor rules
 * that the runtime integration will use.
 */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/jump_label.h>
#include <linux/string.h>
#include <linux/types.h>

#include <sysdep/ptrace.h>

#include "kvm_v2_backend.h"

static void test_record_lifecycle(struct kunit *test)
{
	struct kvm_v2_record *rec;

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_INIT);
	KUNIT_EXPECT_TRUE(test, rec->strict_replay);
	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));

	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_RECORDING);
	KUNIT_EXPECT_PTR_EQ(test, kvm_v2_record_active(), rec);
	KUNIT_EXPECT_TRUE(test,
			  static_branch_unlikely(&um_kvm_v2_record_enabled));

	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_STOPPED);
	KUNIT_EXPECT_PTR_EQ(test, kvm_v2_record_active(),
			    (struct kvm_v2_record *)NULL);
	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));

	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_REPLAYING);
	KUNIT_EXPECT_PTR_EQ(test, kvm_v2_record_active(), rec);
	KUNIT_EXPECT_TRUE(test,
			  static_branch_unlikely(&um_kvm_v2_record_enabled));

	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_set_strict_replay(rec, false), 0);
	KUNIT_EXPECT_FALSE(test, kvm_v2_record_strict_replay(rec));
	KUNIT_EXPECT_EQ(test, kvm_v2_record_set_strict_replay(rec, true), 0);
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_strict_replay(rec));

	kvm_v2_record_destroy(rec);
	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));
}

static void test_record_invalid_transitions(struct kunit *test)
{
	struct kvm_v2_record *rec;

	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_set_strict_replay(NULL, true),
			-EINVAL);
	KUNIT_EXPECT_FALSE(test, kvm_v2_record_strict_replay(NULL));
	kvm_v2_record_destroy(NULL);
	kvm_v2_record_free(NULL);

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);

	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(rec), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(rec), -EINVAL);

	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(rec), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(rec), -EINVAL);

	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(rec), -EINVAL);

	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(rec), 0);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(rec), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_replay(rec), -EINVAL);

	kvm_v2_record_destroy(rec);
	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));
}

static void test_record_single_active_owner(struct kunit *test)
{
	struct kvm_v2_record *first;
	struct kvm_v2_record *second;

	first = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, first);
	second = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, second);

	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(first), 0);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(second), -EBUSY);

	KUNIT_EXPECT_EQ(test, kvm_v2_record_stop(first), 0);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(second), 0);

	kvm_v2_record_destroy(first);
	kvm_v2_record_destroy(second);
	KUNIT_EXPECT_FALSE(test,
			   static_branch_unlikely(&um_kvm_v2_record_enabled));
}

static void fill_syscall_regs(struct uml_pt_regs *regs)
{
	memset(regs, 0, sizeof(*regs));
	regs->gp[HOST_DI] = 0x1111111100000001ULL;
	regs->gp[HOST_SI] = 0x2222222200000002ULL;
	regs->gp[HOST_DX] = 0x3333333300000003ULL;
	regs->gp[HOST_R10] = 0x4444444400000004ULL;
	regs->gp[HOST_R8] = 0x5555555500000005ULL;
	regs->gp[HOST_R9] = 0x6666666600000006ULL;
}

static void test_record_observe_syscall(struct kunit *test)
{
	struct kvm_v2_replay_entry *entry;
	struct kvm_v2_record *rec;
	struct uml_pt_regs regs;
	size_t used;

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	fill_syscall_regs(&regs);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	kvm_v2_record_observe_syscall(rec, 39, -4, &regs);

	KUNIT_EXPECT_EQ(test, rec->buffer_used, sizeof(*entry));
	KUNIT_EXPECT_EQ(test, rec->entries_recorded, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->syscall_count, 1ULL);

	entry = rec->buffer;
	KUNIT_EXPECT_EQ(test, entry->kind, (u32)KVM_V2_REPLAY_SYSCALL);
	KUNIT_EXPECT_EQ(test, entry->size, (u32)sizeof(*entry));
	KUNIT_EXPECT_EQ(test, entry->sequence, 1ULL);
	KUNIT_EXPECT_EQ(test, entry->syscall.nr, 39);
	KUNIT_EXPECT_EQ(test, entry->syscall.retval, -4LL);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[0], regs.gp[HOST_DI]);
	KUNIT_EXPECT_EQ(test, entry->syscall.args[3], regs.gp[HOST_R10]);

	used = rec->buffer_used;
	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	kvm_v2_record_observe_syscall(rec, 39, 0, &regs);
	KUNIT_EXPECT_EQ(test, rec->buffer_used, used);

	kvm_v2_record_destroy(rec);
}

static void test_record_replay_syscall_fifo(struct kunit *test)
{
	static const long retvals[] = { 101, 102, 103, 104 };
	struct kvm_v2_record *rec;
	long got;
	int i;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);

	for (i = 0; i < ARRAY_SIZE(retvals); i++)
		kvm_v2_record_observe_syscall(rec, 39, retvals[i], NULL);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

	for (i = 0; i < ARRAY_SIZE(retvals); i++) {
		got = 0;
		KUNIT_EXPECT_EQ(test,
				kvm_v2_record_consume_syscall(rec, 39, &got),
				1);
		KUNIT_EXPECT_EQ(test, got, retvals[i]);
		KUNIT_EXPECT_EQ(test, rec->entries_replayed, (u64)(i + 1));
	}

	got = 0x7fff;
	KUNIT_EXPECT_EQ(test, kvm_v2_record_consume_syscall(rec, 39, &got),
			-ENODATA);
	KUNIT_EXPECT_EQ(test, got, 0x7fffL);

	kvm_v2_record_destroy(rec);
}

static void test_record_replay_divergence_preserves_cursor(struct kunit *test)
{
	struct kvm_v2_record *rec;
	size_t cursor;
	long got = 0;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);

	kvm_v2_record_observe_syscall(rec, 39, 1234, NULL);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

	cursor = rec->buffer_replayed;
	KUNIT_EXPECT_EQ(test, kvm_v2_record_consume_syscall(rec, 110, &got),
			-EILSEQ);
	KUNIT_EXPECT_EQ(test, rec->buffer_replayed, cursor);
	KUNIT_EXPECT_EQ(test, rec->entries_replayed, 0ULL);

	KUNIT_EXPECT_EQ(test, kvm_v2_record_consume_syscall(rec, 39, &got),
			1);
	KUNIT_EXPECT_EQ(test, got, 1234L);

	kvm_v2_record_destroy(rec);
}

static void test_record_buffer_overflow_is_counted(struct kunit *test)
{
	struct kvm_v2_record *rec;

	rec = kvm_v2_record_alloc(sizeof(struct kvm_v2_replay_entry));
	KUNIT_ASSERT_NOT_NULL(test, rec);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);

	kvm_v2_record_observe_syscall(rec, 39, 1, NULL);
	kvm_v2_record_observe_syscall(rec, 39, 2, NULL);

	KUNIT_EXPECT_EQ(test, rec->entries_recorded, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->entries_dropped, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->buffer_used,
			sizeof(struct kvm_v2_replay_entry));

	kvm_v2_record_destroy(rec);
}

static struct kunit_case kvm_v2_record_test_cases[] = {
	KUNIT_CASE(test_record_lifecycle),
	KUNIT_CASE(test_record_invalid_transitions),
	KUNIT_CASE(test_record_single_active_owner),
	KUNIT_CASE(test_record_observe_syscall),
	KUNIT_CASE(test_record_replay_syscall_fifo),
	KUNIT_CASE(test_record_replay_divergence_preserves_cursor),
	KUNIT_CASE(test_record_buffer_overflow_is_counted),
	{}
};

static struct kunit_suite kvm_v2_record_suite = {
	.name = "um_kvm_v2_record",
	.test_cases = kvm_v2_record_test_cases,
};

kunit_test_suite(kvm_v2_record_suite);

MODULE_DESCRIPTION("UML KVM v2 record/replay KUnit tests");
MODULE_LICENSE("GPL");
