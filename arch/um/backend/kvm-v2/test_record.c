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
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/types.h>
#include <uapi/linux/time_types.h>

#include <asm/unistd.h>
#include <sysdep/ptrace.h>

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

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

static void test_record_gadget_bypass_page(struct kunit *test)
{
	u8 state_page[128] = { 0 };

	KUNIT_ASSERT_LT(test, KVM_V2_GADGET_OFF_RECORD,
			(unsigned int)sizeof(state_page));

	kvm_v2_record_set_gadget_bypass_page(state_page, true);
	KUNIT_EXPECT_EQ(test, state_page[KVM_V2_GADGET_OFF_RECORD], (u8)1);

	kvm_v2_record_set_gadget_bypass_page(state_page, false);
	KUNIT_EXPECT_EQ(test, state_page[KVM_V2_GADGET_OFF_RECORD], (u8)0);
}

static void test_record_strict_syscall_policy(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_supported(__NR_getpid));
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_supported(__NR_getppid));
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_supported(__NR_gettid));
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_supported(__NR_getcwd));
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_supported(__NR_uname));
#ifdef __NR_clock_gettime
	KUNIT_EXPECT_TRUE(test,
			  kvm_v2_record_syscall_supported(__NR_clock_gettime));
#endif
#ifdef __NR_time
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_supported(__NR_time));
#endif

	KUNIT_EXPECT_FALSE(test, kvm_v2_record_syscall_has_payload(__NR_getpid));
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_has_payload(__NR_getcwd));
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_has_payload(__NR_uname));
#ifdef __NR_clock_gettime
	KUNIT_EXPECT_TRUE(test,
			  kvm_v2_record_syscall_has_payload(__NR_clock_gettime));
#endif
#ifdef __NR_time
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_has_payload(__NR_time));
#endif
	KUNIT_EXPECT_FALSE(test, kvm_v2_record_syscall_supported(__NR_getuid));
#ifdef __NR_getrandom
	KUNIT_EXPECT_FALSE(test,
			   kvm_v2_record_syscall_supported(__NR_getrandom));
#endif
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
	KUNIT_EXPECT_EQ(test, rec->first_syscall_pid, current->pid);
	KUNIT_EXPECT_EQ(test, rec->last_syscall_pid, current->pid);
	KUNIT_EXPECT_EQ(test, rec->syscalls_from_snapshot_task, 0ULL);
	KUNIT_EXPECT_EQ(test, rec->syscalls_from_other_tasks, 0ULL);

	entry = rec->buffer;
	KUNIT_EXPECT_EQ(test, entry->kind, (u32)KVM_V2_REPLAY_SYSCALL);
	KUNIT_EXPECT_EQ(test, entry->size, (u32)sizeof(*entry));
	KUNIT_EXPECT_EQ(test, entry->sequence, 1ULL);
	KUNIT_EXPECT_EQ(test, entry->version, KVM_V2_RECORD_FORMAT_VERSION);
	KUNIT_EXPECT_EQ(test, entry->flags, KVM_V2_RECORD_ENTRY_F_NONE);
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

static void test_record_entry_format_contract(struct kunit *test)
{
	struct kvm_v2_replay_entry *entry;
	struct kvm_v2_record *rec;
	size_t cursor;
	long got = -1;

	KUNIT_EXPECT_EQ(test, KVM_V2_RECORD_FORMAT_VERSION, 1U);
	KUNIT_EXPECT_EQ(test, KVM_V2_RECORD_ENTRY_F_NONE, 0U);
	KUNIT_EXPECT_GT(test, (size_t)KVM_V2_RECORD_ENTRY_HEADER_SIZE, 0UL);
	KUNIT_EXPECT_LT(test, (size_t)KVM_V2_RECORD_ENTRY_HEADER_SIZE,
			sizeof(struct kvm_v2_replay_entry));

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	kvm_v2_record_observe_syscall(rec, 39, 1234, NULL);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);

	entry = rec->buffer;
	KUNIT_ASSERT_EQ(test, entry->version, KVM_V2_RECORD_FORMAT_VERSION);
	KUNIT_ASSERT_EQ(test, entry->flags, KVM_V2_RECORD_ENTRY_F_NONE);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);
	cursor = rec->buffer_replayed;
	entry->version++;
	KUNIT_EXPECT_EQ(test, kvm_v2_record_consume_syscall(rec, 39, &got),
			-EILSEQ);
	KUNIT_EXPECT_EQ(test, rec->buffer_replayed, cursor);
	entry->version = KVM_V2_RECORD_FORMAT_VERSION;

	entry->flags = 1;
	KUNIT_EXPECT_EQ(test, kvm_v2_record_consume_syscall(rec, 39, &got),
			-EILSEQ);
	KUNIT_EXPECT_EQ(test, rec->buffer_replayed, cursor);
	entry->flags = KVM_V2_RECORD_ENTRY_F_NONE;

	KUNIT_EXPECT_EQ(test, kvm_v2_record_consume_syscall(rec, 39, &got),
			1);
	KUNIT_EXPECT_EQ(test, got, 1234L);

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

static void test_record_syscall_payload_fifo(struct kunit *test)
{
	static const char payload[] = "recorded-uname-payload";
	struct kvm_v2_record *rec;
	struct uml_pt_regs regs;
	char got_payload[64];
	size_t got_len = 0;
	long got_ret = -1;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	fill_syscall_regs(&regs);
	regs.gp[HOST_DI] = 0x12345000ULL;

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_observe_syscall_payload(rec, __NR_uname, 0,
							      &regs, 0,
							      payload,
							      sizeof(payload)),
			1);
	KUNIT_EXPECT_EQ(test, rec->entries_recorded, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->payload_entries_recorded, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->payload_bytes_recorded,
			(u64)sizeof(payload));

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_consume_syscall_payload(rec, __NR_uname,
							      &regs,
							      &got_ret,
							      got_payload,
							      sizeof(got_payload),
							      &got_len),
			1);
	KUNIT_EXPECT_EQ(test, got_ret, 0L);
	KUNIT_EXPECT_EQ(test, got_len, sizeof(payload));
	KUNIT_EXPECT_EQ(test, memcmp(got_payload, payload, sizeof(payload)), 0);
	KUNIT_EXPECT_EQ(test, rec->entries_replayed, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->payload_entries_replayed, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->payload_bytes_replayed,
			(u64)sizeof(payload));

	kvm_v2_record_destroy(rec);
}

static void test_record_syscall_payload_getcwd_fifo(struct kunit *test)
{
	static const char payload[] = "/record-root";
	struct kvm_v2_record *rec;
	struct uml_pt_regs regs;
	char got_payload[32];
	size_t got_len = 0;
	long got_ret = -1;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	fill_syscall_regs(&regs);
	regs.gp[HOST_DI] = 0x12345000ULL;
	regs.gp[HOST_SI] = sizeof(got_payload);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_observe_syscall_payload(rec, __NR_getcwd,
							      sizeof(payload),
							      &regs, 0,
							      payload,
							      sizeof(payload)),
			1);
	KUNIT_EXPECT_EQ(test, rec->payload_entries_recorded, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->payload_bytes_recorded,
			(u64)sizeof(payload));

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_consume_syscall_payload(rec, __NR_getcwd,
							      &regs, &got_ret,
							      got_payload,
							      sizeof(got_payload),
							      &got_len),
			1);
	KUNIT_EXPECT_EQ(test, got_ret, (long)sizeof(payload));
	KUNIT_EXPECT_EQ(test, got_len, sizeof(payload));
	KUNIT_EXPECT_EQ(test, memcmp(got_payload, payload, sizeof(payload)), 0);

	kvm_v2_record_destroy(rec);
}

#ifdef __NR_clock_gettime
static void test_record_syscall_payload_clock_gettime_fifo(struct kunit *test)
{
	const struct __kernel_timespec payload = {
		.tv_sec = 123,
		.tv_nsec = 456789,
	};
	struct kvm_v2_record *rec;
	struct uml_pt_regs regs;
	struct uml_pt_regs replay_regs;
	struct __kernel_timespec got_payload;
	size_t cursor;
	size_t got_len = 0;
	long got_ret = -1;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	fill_syscall_regs(&regs);
	regs.gp[HOST_DI] = 1;
	regs.gp[HOST_SI] = 0x12345000ULL;
	replay_regs = regs;
	replay_regs.gp[HOST_SI] = 0x12346000ULL;

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_observe_syscall_payload(rec,
							      __NR_clock_gettime,
							      0, &regs, 1,
							      &payload,
							      sizeof(payload)),
			1);
	KUNIT_EXPECT_EQ(test, rec->payload_entries_recorded, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->payload_bytes_recorded,
			(u64)sizeof(payload));

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

	replay_regs.gp[HOST_DI] = 2;
	cursor = rec->buffer_replayed;
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_consume_syscall_payload(rec,
							      __NR_clock_gettime,
							      &replay_regs,
							      &got_ret,
							      &got_payload,
							      sizeof(got_payload),
							      &got_len),
			-EILSEQ);
	KUNIT_EXPECT_EQ(test, rec->buffer_replayed, cursor);

	replay_regs.gp[HOST_DI] = regs.gp[HOST_DI];
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_consume_syscall_payload(rec,
							      __NR_clock_gettime,
							      &replay_regs,
							      &got_ret,
							      &got_payload,
							      sizeof(got_payload),
							      &got_len),
			1);
	KUNIT_EXPECT_EQ(test, got_ret, 0L);
	KUNIT_EXPECT_EQ(test, got_len, sizeof(payload));
	KUNIT_EXPECT_EQ(test, memcmp(&got_payload, &payload, sizeof(payload)),
			0);

	kvm_v2_record_destroy(rec);
}
#endif

#ifdef __NR_time
static void test_record_syscall_payload_time_fifo(struct kunit *test)
{
	const __kernel_old_time_t payload = 12345;
	struct kvm_v2_record *rec;
	struct uml_pt_regs regs;
	struct uml_pt_regs replay_regs;
	__kernel_old_time_t got_payload;
	size_t cursor;
	size_t got_len = 0;
	long got_ret = -1;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	fill_syscall_regs(&regs);
	regs.gp[HOST_DI] = 0x12345000ULL;
	replay_regs = regs;
	replay_regs.gp[HOST_DI] = 0x12346000ULL;

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_observe_syscall_payload(rec, __NR_time,
							      payload, &regs,
							      0, &payload,
							      sizeof(payload)),
			1);
	KUNIT_EXPECT_EQ(test, rec->payload_entries_recorded, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->payload_bytes_recorded,
			(u64)sizeof(payload));

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

	cursor = rec->buffer_replayed;
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_consume_syscall_payload(rec, __NR_time,
							      &replay_regs,
							      &got_ret,
							      &got_payload,
							      sizeof(got_payload),
							      &got_len),
			1);
	KUNIT_EXPECT_NE(test, rec->buffer_replayed, cursor);
	KUNIT_EXPECT_EQ(test, got_ret, (long)payload);
	KUNIT_EXPECT_EQ(test, got_len, sizeof(payload));
	KUNIT_EXPECT_EQ(test, got_payload, payload);

	kvm_v2_record_destroy(rec);
}
#endif

static void test_record_syscall_payload_short_buffer(struct kunit *test)
{
	static const char payload[] = "payload-too-large";
	struct kvm_v2_record *rec;
	struct uml_pt_regs regs;
	char small[4];
	size_t cursor;
	size_t got_len;
	long got_ret;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	fill_syscall_regs(&regs);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_ASSERT_EQ(test,
			kvm_v2_record_observe_syscall_payload(rec, __NR_uname, 0,
							      &regs, 0,
							      payload,
							      sizeof(payload)),
			1);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

	cursor = rec->buffer_replayed;
	got_len = 1234;
	got_ret = -1;
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_consume_syscall_payload(rec, __NR_uname,
							      &regs,
							      &got_ret, small,
							      sizeof(small),
							      &got_len),
			-ENOSPC);
	KUNIT_EXPECT_EQ(test, rec->buffer_replayed, cursor);
	KUNIT_EXPECT_EQ(test, rec->entries_replayed, 0ULL);
	KUNIT_EXPECT_EQ(test, got_len, 0UL);
	KUNIT_EXPECT_EQ(test, got_ret, -1L);

	kvm_v2_record_destroy(rec);
}

static void test_record_syscall_payload_arg_mismatch(struct kunit *test)
{
	static const char payload[] = "payload-arg-match";
	struct kvm_v2_record *rec;
	struct uml_pt_regs regs;
	struct uml_pt_regs replay_regs;
	char got_payload[64];
	size_t cursor;
	size_t got_len;
	long got_ret;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	fill_syscall_regs(&regs);
	replay_regs = regs;
	replay_regs.gp[HOST_SI]++;

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_ASSERT_EQ(test,
			kvm_v2_record_observe_syscall_payload(rec, __NR_getcwd, 0,
							      &regs, 0,
							      payload,
							      sizeof(payload)),
			1);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

	cursor = rec->buffer_replayed;
	got_len = 0;
	got_ret = -1;
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_consume_syscall_payload(rec, __NR_getcwd,
							      &replay_regs,
							      &got_ret,
							      got_payload,
							      sizeof(got_payload),
							      &got_len),
			-EILSEQ);
	KUNIT_EXPECT_EQ(test, rec->buffer_replayed, cursor);
	KUNIT_EXPECT_EQ(test, rec->entries_replayed, 0ULL);

	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_consume_syscall_payload(rec, __NR_getcwd,
							      &regs,
							      &got_ret,
							      got_payload,
							      sizeof(got_payload),
							      &got_len),
			1);
	KUNIT_EXPECT_EQ(test, got_len, sizeof(payload));
	KUNIT_EXPECT_EQ(test, memcmp(got_payload, payload, sizeof(payload)), 0);

	kvm_v2_record_destroy(rec);
}

static void test_record_syscall_payload_overflow(struct kunit *test)
{
	static const char payload[] = "payload-overflow";
	struct kvm_v2_record *rec;
	struct uml_pt_regs regs;

	rec = kvm_v2_record_alloc(sizeof(struct kvm_v2_replay_entry));
	KUNIT_ASSERT_NOT_NULL(test, rec);
	fill_syscall_regs(&regs);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_observe_syscall_payload(rec, __NR_uname, 0,
							      &regs, 0,
							      payload,
							      sizeof(payload)),
			-ENOSPC);
	KUNIT_EXPECT_EQ(test, rec->entries_recorded, 0ULL);
	KUNIT_EXPECT_EQ(test, rec->entries_dropped, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->payload_entries_recorded, 0ULL);

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

static void test_record_strict_replay_failure_is_counted(struct kunit *test)
{
	struct kvm_v2_record *rec;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);

	KUNIT_EXPECT_EQ(test, kvm_v2_record_start(rec), 0);
	kvm_v2_record_note_replay_failure(rec, 999, -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, 0ULL);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);
	kvm_v2_record_note_replay_failure(rec, 999, -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->last_replay_failure_syscall, 999L);
	KUNIT_EXPECT_EQ(test, rec->last_replay_failure_rc, -EOPNOTSUPP);

	kvm_v2_record_note_replay_failure(rec, 39, -EILSEQ);
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, 2ULL);
	KUNIT_EXPECT_EQ(test, rec->last_replay_failure_syscall, 39L);
	KUNIT_EXPECT_EQ(test, rec->last_replay_failure_rc, -EILSEQ);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_EXPECT_EQ(test, kvm_v2_record_reset(rec), 0);
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, 0ULL);
	KUNIT_EXPECT_EQ(test, rec->last_replay_failure_syscall, -1L);
	KUNIT_EXPECT_EQ(test, rec->last_replay_failure_rc, 0);

	kvm_v2_record_destroy(rec);
}

static void test_record_strict_syscall_gate(struct kunit *test)
{
	struct kvm_v2_record *rec;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_check_strict_syscall(rec, __NR_getpid),
			0);
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, 0ULL);

	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_check_strict_syscall(rec, __NR_getuid),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, 1ULL);
	KUNIT_EXPECT_EQ(test, rec->last_replay_failure_syscall,
			(long)__NR_getuid);
	KUNIT_EXPECT_EQ(test, rec->last_replay_failure_rc, -EOPNOTSUPP);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_set_strict_replay(rec, false), 0);
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_check_strict_syscall(rec, __NR_getuid),
			0);
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, 1ULL);

	kvm_v2_record_destroy(rec);
}

static void test_record_strict_time_policy(struct kunit *test)
{
	struct kvm_v2_record *rec;
	unsigned long rejected = 0;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

#ifdef __NR_clock_gettime
	KUNIT_EXPECT_TRUE(test,
			  kvm_v2_record_syscall_has_payload(__NR_clock_gettime));
	KUNIT_EXPECT_TRUE(test,
			  kvm_v2_record_syscall_supported(__NR_clock_gettime));
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_check_strict_syscall(rec,
							   __NR_clock_gettime),
			0);
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, rejected);
#endif
#ifdef __NR_gettimeofday
	KUNIT_EXPECT_FALSE(test,
			   kvm_v2_record_syscall_has_payload(__NR_gettimeofday));
	KUNIT_EXPECT_FALSE(test,
			   kvm_v2_record_syscall_supported(__NR_gettimeofday));
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_check_strict_syscall(rec,
							   __NR_gettimeofday),
			-EOPNOTSUPP);
	rejected++;
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, rejected);
	KUNIT_EXPECT_EQ(test, rec->last_replay_failure_syscall,
			(long)__NR_gettimeofday);
#endif
#ifdef __NR_time
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_has_payload(__NR_time));
	KUNIT_EXPECT_TRUE(test, kvm_v2_record_syscall_supported(__NR_time));
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_check_strict_syscall(rec, __NR_time),
			0);
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, rejected);
#endif

	kvm_v2_record_destroy(rec);
}

static void expect_strict_rejects_syscall(struct kunit *test,
					  struct kvm_v2_record *rec,
					  unsigned long syscall_nr,
					  unsigned long *rejected)
{
	KUNIT_EXPECT_FALSE(test, kvm_v2_record_syscall_has_payload(syscall_nr));
	KUNIT_EXPECT_FALSE(test, kvm_v2_record_syscall_supported(syscall_nr));
	KUNIT_EXPECT_EQ(test,
			kvm_v2_record_check_strict_syscall(rec, syscall_nr),
			-EOPNOTSUPP);
	(*rejected)++;
	KUNIT_EXPECT_EQ(test, rec->strict_replay_failures, *rejected);
	KUNIT_EXPECT_EQ(test, rec->last_replay_failure_syscall,
			(long)syscall_nr);
}

static void test_record_strict_external_io_policy(struct kunit *test)
{
	struct kvm_v2_record *rec;
	unsigned long rejected = 0;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);

	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

#ifdef __NR_getrandom
	expect_strict_rejects_syscall(test, rec, __NR_getrandom, &rejected);
#endif
#ifdef __NR_openat
	expect_strict_rejects_syscall(test, rec, __NR_openat, &rejected);
#endif
#ifdef __NR_read
	expect_strict_rejects_syscall(test, rec, __NR_read, &rejected);
#endif
#ifdef __NR_write
	expect_strict_rejects_syscall(test, rec, __NR_write, &rejected);
#endif
#ifdef __NR_ioctl
	expect_strict_rejects_syscall(test, rec, __NR_ioctl, &rejected);
#endif

	KUNIT_EXPECT_GT(test, rejected, 0UL);
	kvm_v2_record_destroy(rec);
}

static void test_record_time_travel_fifo(struct kunit *test)
{
	static const u64 ns_values[] = {
		1000ULL,
		2500ULL,
		1000000ULL,
		1000000000ULL,
	};
	struct kvm_v2_record *rec;
	u64 ns_got;
	u64 anchor_got;
	u64 syscalls_consumed = 0;
	int rc;
	int i;

	rec = kvm_v2_record_alloc(4096);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_start(rec), 0);

	for (i = 0; i < ARRAY_SIZE(ns_values); i++) {
		int j;

		for (j = 0; j < i; j++)
			kvm_v2_record_observe_syscall(rec, 39, 0, NULL);
		kvm_v2_record_observe_time_travel(rec, ns_values[i]);
	}

	KUNIT_ASSERT_EQ(test, kvm_v2_record_stop(rec), 0);
	KUNIT_ASSERT_EQ(test, kvm_v2_record_replay(rec), 0);

	for (i = 0; i < ARRAY_SIZE(ns_values); i++) {
		int j;

		for (j = 0; j < i; j++) {
			long ret = -1;

			rc = kvm_v2_record_consume_syscall(rec, 39, &ret);
			KUNIT_ASSERT_EQ(test, rc, 1);
			KUNIT_EXPECT_EQ(test, ret, 0L);
			syscalls_consumed++;
		}

		ns_got = 0;
		anchor_got = 0;
		rc = kvm_v2_record_consume_time_travel(rec, &ns_got,
						       &anchor_got);
		KUNIT_ASSERT_EQ(test, rc, 1);
		KUNIT_EXPECT_EQ(test, ns_got, ns_values[i]);
		KUNIT_EXPECT_EQ(test, anchor_got, syscalls_consumed);
	}

	ns_got = 0xdeadbeefULL;
	rc = kvm_v2_record_consume_time_travel(rec, &ns_got, &anchor_got);
	KUNIT_EXPECT_EQ(test, rc, -ENODATA);
	KUNIT_EXPECT_EQ(test, ns_got, 0xdeadbeefULL);

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

static void test_record_reset_releases_snapshot(struct kunit *test)
{
	struct kvm_v2_record *rec;
	struct kvm_v2_snapshot *snapshot;

	rec = kvm_v2_record_alloc(0);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	snapshot = kvm_v2_snapshot_alloc();
	KUNIT_ASSERT_NOT_NULL(test, snapshot);

	snapshot->task_state_captured = true;
	snapshot->task_source_pid = 123;
	rec->snapshot = snapshot;
	rec->snapshot_attempted = true;
	rec->snapshot_valid = true;
	rec->snapshot_rc = 0;
	rec->state = KVM_V2_RECORD_STOPPED;

	KUNIT_EXPECT_EQ(test, kvm_v2_record_reset(rec), 0);
	KUNIT_EXPECT_PTR_EQ(test, rec->snapshot,
			    (struct kvm_v2_snapshot *)NULL);
	KUNIT_EXPECT_FALSE(test, rec->snapshot_attempted);
	KUNIT_EXPECT_FALSE(test, rec->snapshot_valid);
	KUNIT_EXPECT_EQ(test, rec->snapshot_rc, 0);
	KUNIT_EXPECT_EQ(test, rec->state, KVM_V2_RECORD_INIT);
	KUNIT_EXPECT_TRUE(test, rec->strict_replay);

	kvm_v2_record_destroy(rec);
}

static struct kunit_case kvm_v2_record_test_cases[] = {
	KUNIT_CASE(test_record_lifecycle),
	KUNIT_CASE(test_record_invalid_transitions),
	KUNIT_CASE(test_record_single_active_owner),
	KUNIT_CASE(test_record_gadget_bypass_page),
	KUNIT_CASE(test_record_strict_syscall_policy),
	KUNIT_CASE(test_record_observe_syscall),
	KUNIT_CASE(test_record_entry_format_contract),
	KUNIT_CASE(test_record_replay_syscall_fifo),
	KUNIT_CASE(test_record_syscall_payload_fifo),
	KUNIT_CASE(test_record_syscall_payload_getcwd_fifo),
#ifdef __NR_clock_gettime
	KUNIT_CASE(test_record_syscall_payload_clock_gettime_fifo),
#endif
#ifdef __NR_time
	KUNIT_CASE(test_record_syscall_payload_time_fifo),
#endif
	KUNIT_CASE(test_record_syscall_payload_short_buffer),
	KUNIT_CASE(test_record_syscall_payload_arg_mismatch),
	KUNIT_CASE(test_record_syscall_payload_overflow),
	KUNIT_CASE(test_record_replay_divergence_preserves_cursor),
	KUNIT_CASE(test_record_strict_replay_failure_is_counted),
	KUNIT_CASE(test_record_strict_syscall_gate),
	KUNIT_CASE(test_record_strict_time_policy),
	KUNIT_CASE(test_record_strict_external_io_policy),
	KUNIT_CASE(test_record_time_travel_fifo),
	KUNIT_CASE(test_record_buffer_overflow_is_counted),
	KUNIT_CASE(test_record_reset_releases_snapshot),
	{}
};

static struct kunit_suite kvm_v2_record_suite = {
	.name = "um_kvm_v2_record",
	.test_cases = kvm_v2_record_test_cases,
};

kunit_test_suite(kvm_v2_record_suite);

MODULE_DESCRIPTION("UML KVM v2 record/replay KUnit tests");
MODULE_LICENSE("GPL");
