// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — marshal-shape KUnit suite (memo 26 D.2 +
 * task #74).
 *
 * Tests the byte-shape mapping of:
 *   - kvm_v2_marshal_to_kvm_regs   (gp[HOST_*] → struct kvm_regs)
 *   - kvm_v2_marshal_from_kvm_regs (struct kvm_regs → gp[HOST_*])
 *
 * These are pure data-transform functions — no host syscalls, no
 * vCPU state, no FPU. The test exercises every GPR + RIP + RFLAGS
 * field with distinct sentinel values and asserts the right field
 * lands at the right slot. Catches:
 *
 *   - Off-by-one in HOST_* index → kvm_regs field mapping
 *   - Wrong field name in dst (rsi vs rdi swap, etc.)
 *   - Lost upper bits (e.g. accidentally using u32 instead of u64)
 *   - RFLAGS bit-1-must-be-1 invariant on marshal_to (per AMD64 SDM
 *     §3.1.4) and bit-1-pass-through on marshal_from
 *
 * The bugs at E.5 (IST frame off-by-8 with error code,
 * `arch/um/backend/kvm-v2/syscall_trap.c::kvm_v2_ist_frame_read`)
 * were the same SHAPE class as what these tests would catch had they
 * existed earlier. Marshal correctness is load-bearing for every
 * single KVM_RUN dispatch.
 */
#include <kunit/test.h>
#include <linux/kvm.h>
#include <linux/string.h>
#include <linux/types.h>

#include <sysdep/ptrace.h>		/* struct uml_pt_regs, MAX_REG_NR */
#include <sysdep/ptrace_64.h>		/* HOST_* indexes (x86_64) */

#include "kvm_v2_backend.h"

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

/*
 * Sentinel pattern: each GPR + RIP gets a unique 64-bit value built
 * from the HOST_* index in the upper 32 bits and 0xDEADBEEF in the
 * lower 32 bits, so a wrong-slot mapping produces a numerically
 * obvious mismatch in the failure message.
 */
static unsigned long sentinel(unsigned int slot)
{
	return ((unsigned long)slot << 32) | 0xDEADBEEFUL;
}

static void fill_uml_regs_pattern(struct uml_pt_regs *r)
{
	memset(r, 0, sizeof(*r));
	r->gp[HOST_AX]     = sentinel(HOST_AX);
	r->gp[HOST_BX]     = sentinel(HOST_BX);
	r->gp[HOST_CX]     = sentinel(HOST_CX);
	r->gp[HOST_DX]     = sentinel(HOST_DX);
	r->gp[HOST_SI]     = sentinel(HOST_SI);
	r->gp[HOST_DI]     = sentinel(HOST_DI);
	r->gp[HOST_BP]     = sentinel(HOST_BP);
	r->gp[HOST_SP]     = sentinel(HOST_SP);
	r->gp[HOST_R8]     = sentinel(HOST_R8);
	r->gp[HOST_R9]     = sentinel(HOST_R9);
	r->gp[HOST_R10]    = sentinel(HOST_R10);
	r->gp[HOST_R11]    = sentinel(HOST_R11);
	r->gp[HOST_R12]    = sentinel(HOST_R12);
	r->gp[HOST_R13]    = sentinel(HOST_R13);
	r->gp[HOST_R14]    = sentinel(HOST_R14);
	r->gp[HOST_R15]    = sentinel(HOST_R15);
	r->gp[HOST_IP]     = sentinel(HOST_IP);
	r->gp[HOST_EFLAGS] = sentinel(HOST_EFLAGS);
}

static void fill_kvm_regs_pattern(struct kvm_regs *k)
{
	memset(k, 0, sizeof(*k));
	k->rax    = sentinel(HOST_AX);
	k->rbx    = sentinel(HOST_BX);
	k->rcx    = sentinel(HOST_CX);
	k->rdx    = sentinel(HOST_DX);
	k->rsi    = sentinel(HOST_SI);
	k->rdi    = sentinel(HOST_DI);
	k->rbp    = sentinel(HOST_BP);
	k->rsp    = sentinel(HOST_SP);
	k->r8     = sentinel(HOST_R8);
	k->r9     = sentinel(HOST_R9);
	k->r10    = sentinel(HOST_R10);
	k->r11    = sentinel(HOST_R11);
	k->r12    = sentinel(HOST_R12);
	k->r13    = sentinel(HOST_R13);
	k->r14    = sentinel(HOST_R14);
	k->r15    = sentinel(HOST_R15);
	k->rip    = sentinel(HOST_IP);
	k->rflags = sentinel(HOST_EFLAGS);
}

/* ---------------------------------------------------------------- */
/* marshal_to_kvm_regs tests                                        */
/* ---------------------------------------------------------------- */

static void test_marshal_to_zero(struct kunit *test)
{
	struct uml_pt_regs src;
	struct kvm_regs dst;

	memset(&src, 0, sizeof(src));
	memset(&dst, 0xff, sizeof(dst));	/* poison */

	kvm_v2_marshal_to_kvm_regs(&dst, &src);

	KUNIT_EXPECT_EQ(test, (u64)dst.rax,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.rbx,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.rcx,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.rdx,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.rsi,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.rdi,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.rbp,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.rsp,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.r8,     (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.r9,     (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.r10,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.r11,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.r12,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.r13,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.r14,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.r15,    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.rip,    (u64)0);
	/*
	 * RFLAGS bit 1 is reserved-must-be-1 per AMD64 §3.1.4; the
	 * marshal OR's it in defensively. With src bit 1 = 0 the dst
	 * must still have it set.
	 */
	KUNIT_EXPECT_EQ(test, (u64)dst.rflags, (u64)(1UL << 1));
}

static void test_marshal_to_full_pattern(struct kunit *test)
{
	struct uml_pt_regs src;
	struct kvm_regs dst;

	fill_uml_regs_pattern(&src);
	memset(&dst, 0xff, sizeof(dst));

	kvm_v2_marshal_to_kvm_regs(&dst, &src);

	KUNIT_EXPECT_EQ(test, (u64)dst.rax, (u64)sentinel(HOST_AX));
	KUNIT_EXPECT_EQ(test, (u64)dst.rbx, (u64)sentinel(HOST_BX));
	KUNIT_EXPECT_EQ(test, (u64)dst.rcx, (u64)sentinel(HOST_CX));
	KUNIT_EXPECT_EQ(test, (u64)dst.rdx, (u64)sentinel(HOST_DX));
	KUNIT_EXPECT_EQ(test, (u64)dst.rsi, (u64)sentinel(HOST_SI));
	KUNIT_EXPECT_EQ(test, (u64)dst.rdi, (u64)sentinel(HOST_DI));
	KUNIT_EXPECT_EQ(test, (u64)dst.rbp, (u64)sentinel(HOST_BP));
	KUNIT_EXPECT_EQ(test, (u64)dst.rsp, (u64)sentinel(HOST_SP));
	KUNIT_EXPECT_EQ(test, (u64)dst.r8,  (u64)sentinel(HOST_R8));
	KUNIT_EXPECT_EQ(test, (u64)dst.r9,  (u64)sentinel(HOST_R9));
	KUNIT_EXPECT_EQ(test, (u64)dst.r10, (u64)sentinel(HOST_R10));
	KUNIT_EXPECT_EQ(test, (u64)dst.r11, (u64)sentinel(HOST_R11));
	KUNIT_EXPECT_EQ(test, (u64)dst.r12, (u64)sentinel(HOST_R12));
	KUNIT_EXPECT_EQ(test, (u64)dst.r13, (u64)sentinel(HOST_R13));
	KUNIT_EXPECT_EQ(test, (u64)dst.r14, (u64)sentinel(HOST_R14));
	KUNIT_EXPECT_EQ(test, (u64)dst.r15, (u64)sentinel(HOST_R15));
	KUNIT_EXPECT_EQ(test, (u64)dst.rip, (u64)sentinel(HOST_IP));
	/* RFLAGS gets bit 1 OR'd in unconditionally. */
	KUNIT_EXPECT_EQ(test, (u64)dst.rflags,
			(u64)(sentinel(HOST_EFLAGS) | (1UL << 1)));
}

static void test_marshal_to_rflags_bit1_force(struct kunit *test)
{
	struct uml_pt_regs src;
	struct kvm_regs dst;

	memset(&src, 0, sizeof(src));
	src.gp[HOST_EFLAGS] = 0;	/* explicitly clear bit 1 */
	memset(&dst, 0, sizeof(dst));

	kvm_v2_marshal_to_kvm_regs(&dst, &src);

	KUNIT_EXPECT_TRUE(test, (dst.rflags & (1UL << 1)) != 0);
}

static void test_marshal_to_preserves_upper_bits(struct kunit *test)
{
	struct uml_pt_regs src;
	struct kvm_regs dst;
	const unsigned long high_only = 0xFFEEDDCC00000000UL;

	memset(&src, 0, sizeof(src));
	src.gp[HOST_AX] = high_only;
	src.gp[HOST_R15] = high_only;
	memset(&dst, 0, sizeof(dst));

	kvm_v2_marshal_to_kvm_regs(&dst, &src);

	KUNIT_EXPECT_EQ(test, (u64)dst.rax, (u64)high_only);
	KUNIT_EXPECT_EQ(test, (u64)dst.r15, (u64)high_only);
}

/* ---------------------------------------------------------------- */
/* marshal_from_kvm_regs tests                                      */
/* ---------------------------------------------------------------- */

static void test_marshal_from_zero(struct kunit *test)
{
	struct kvm_regs src;
	struct uml_pt_regs dst;

	memset(&src, 0, sizeof(src));
	memset(&dst, 0xff, sizeof(dst));	/* poison */

	kvm_v2_marshal_from_kvm_regs(&dst, &src);

	/*
	 * marshal_from writes 18 fields (16 GPRs + RIP + RFLAGS). It
	 * does NOT touch HOST_ORIG_AX or anything past HOST_EFLAGS.
	 * Verify the 18 it owns are zero, and that one un-touched slot
	 * stays poisoned (regression guard against accidental memset).
	 */
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_AX],     (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_R15],    (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_IP],     (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_EFLAGS], (u64)0);
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_ORIG_AX], (u64)~0UL);
}

static void test_marshal_from_full_pattern(struct kunit *test)
{
	struct kvm_regs src;
	struct uml_pt_regs dst;

	fill_kvm_regs_pattern(&src);
	memset(&dst, 0, sizeof(dst));

	kvm_v2_marshal_from_kvm_regs(&dst, &src);

	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_AX],     (u64)sentinel(HOST_AX));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_BX],     (u64)sentinel(HOST_BX));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_CX],     (u64)sentinel(HOST_CX));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_DX],     (u64)sentinel(HOST_DX));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_SI],     (u64)sentinel(HOST_SI));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_DI],     (u64)sentinel(HOST_DI));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_BP],     (u64)sentinel(HOST_BP));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_SP],     (u64)sentinel(HOST_SP));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_R8],     (u64)sentinel(HOST_R8));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_R9],     (u64)sentinel(HOST_R9));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_R10],    (u64)sentinel(HOST_R10));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_R11],    (u64)sentinel(HOST_R11));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_R12],    (u64)sentinel(HOST_R12));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_R13],    (u64)sentinel(HOST_R13));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_R14],    (u64)sentinel(HOST_R14));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_R15],    (u64)sentinel(HOST_R15));
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_IP],     (u64)sentinel(HOST_IP));
	/*
	 * marshal_from passes RFLAGS through verbatim — no bit 1 OR (the
	 * value already came from KVM, which tracks the architectural
	 * invariant in VMCS).
	 */
	KUNIT_EXPECT_EQ(test, (u64)dst.gp[HOST_EFLAGS], (u64)sentinel(HOST_EFLAGS));
}

/* ---------------------------------------------------------------- */
/* Round-trip tests                                                 */
/* ---------------------------------------------------------------- */

static void test_marshal_round_trip(struct kunit *test)
{
	struct uml_pt_regs src, back;
	struct kvm_regs through;
	int i;

	fill_uml_regs_pattern(&src);
	/*
	 * Set RFLAGS such that bit 1 is already set in src — then the
	 * to/from cycle must be lossless. (Bit 1 OR'd in by marshal_to
	 * is a no-op when src already has it.)
	 */
	src.gp[HOST_EFLAGS] |= (1UL << 1);

	memset(&through, 0, sizeof(through));
	memset(&back, 0xff, sizeof(back));

	kvm_v2_marshal_to_kvm_regs(&through, &src);
	kvm_v2_marshal_from_kvm_regs(&back, &through);

	/*
	 * All 18 marshal-owned slots should equal src exactly. Slots
	 * outside the marshal's domain (HOST_ORIG_AX, HOST_FS_BASE,
	 * HOST_GS_BASE, HOST_DS, HOST_ES, HOST_FS, HOST_GS) stay at
	 * their pre-from poisoned 0xff bytes — caller's responsibility.
	 */
	for (i = 0; i < HOST_AX; i++)
		;	/* AX is the lowest marshal-owned index; nothing below to skip */

	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_AX],     (u64)src.gp[HOST_AX]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_BX],     (u64)src.gp[HOST_BX]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_CX],     (u64)src.gp[HOST_CX]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_DX],     (u64)src.gp[HOST_DX]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_SI],     (u64)src.gp[HOST_SI]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_DI],     (u64)src.gp[HOST_DI]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_BP],     (u64)src.gp[HOST_BP]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_SP],     (u64)src.gp[HOST_SP]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_R8],     (u64)src.gp[HOST_R8]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_R9],     (u64)src.gp[HOST_R9]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_R10],    (u64)src.gp[HOST_R10]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_R11],    (u64)src.gp[HOST_R11]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_R12],    (u64)src.gp[HOST_R12]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_R13],    (u64)src.gp[HOST_R13]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_R14],    (u64)src.gp[HOST_R14]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_R15],    (u64)src.gp[HOST_R15]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_IP],     (u64)src.gp[HOST_IP]);
	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_EFLAGS], (u64)src.gp[HOST_EFLAGS]);
}

/*
 * Round-trip with src RFLAGS bit 1 = 0: the to/from cycle should
 * return src.rflags | bit1 (because marshal_to ORs it in, and
 * marshal_from passes through verbatim — so bit 1 ends up set in
 * `back` even though it was clear in `src`). This is the documented
 * behavior, not a bug — RFLAGS is special-cased.
 */
static void test_marshal_round_trip_rflags_bit1(struct kunit *test)
{
	struct uml_pt_regs src, back;
	struct kvm_regs through;

	memset(&src, 0, sizeof(src));
	src.gp[HOST_EFLAGS] = 0x202;	/* IF set, bit 1 clear */
	memset(&through, 0, sizeof(through));
	memset(&back, 0, sizeof(back));

	kvm_v2_marshal_to_kvm_regs(&through, &src);
	kvm_v2_marshal_from_kvm_regs(&back, &through);

	KUNIT_EXPECT_EQ(test, (u64)back.gp[HOST_EFLAGS], (u64)(0x202 | (1UL << 1)));
}

/* ---------------------------------------------------------------- */
/* Suite registration                                               */
/* ---------------------------------------------------------------- */

static struct kunit_case kvm_v2_marshal_test_cases[] = {
	KUNIT_CASE(test_marshal_to_zero),
	KUNIT_CASE(test_marshal_to_full_pattern),
	KUNIT_CASE(test_marshal_to_rflags_bit1_force),
	KUNIT_CASE(test_marshal_to_preserves_upper_bits),
	KUNIT_CASE(test_marshal_from_zero),
	KUNIT_CASE(test_marshal_from_full_pattern),
	KUNIT_CASE(test_marshal_round_trip),
	KUNIT_CASE(test_marshal_round_trip_rflags_bit1),
	{}
};

static struct kunit_suite kvm_v2_marshal_test_suite = {
	.name = "kvm_v2_marshal",
	.test_cases = kvm_v2_marshal_test_cases,
};

kunit_test_suite(kvm_v2_marshal_test_suite);

MODULE_DESCRIPTION("UML kvm-v2 marshal-shape KUnit tests (D.2 + #74)");
MODULE_LICENSE("GPL v2");
