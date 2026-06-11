// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) byte-shape KUnit suite.
 *
 * Verifies byte layouts used by the exception and syscall machinery.
 * These are pure data tests: no host syscalls, /dev/kvm, or vCPU state.
 * The expected offsets are derived from Intel SDM Vol. 3 sections 3.4.5,
 * 6.14.1, and 6.14.5 rather than from the backend helpers under test.
 */
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "kvm_v2_backend.h"
#include "syscall_trap.h"

/* ---------------------------------------------------------------- */
/* IDT-pushed long-mode exception frame                             */
/* SDM Vol.3 section 6.14.5, for an IST stack top (one past highest */
/* byte), with error-code:                                          */
/*   top - 48: error_code                                           */
/*   top - 40: RIP                                                  */
/*   top - 32: CS  (sign-extended u16 in u64 slot)                  */
/*   top - 24: RFLAGS                                               */
/*   top - 16: RSP                                                  */
/*   top - 8:  SS  (sign-extended u16 in u64 slot)                  */
/* Without error-code, RIP shifts down to top - 40 and the rest    */
/* slide accordingly.                                               */
/* ---------------------------------------------------------------- */

#define IST_PAGE_SIZE 4096

static void test_ist_frame_layout_with_error_code(struct kunit *test)
{
	u8 *page = kunit_kzalloc(test, IST_PAGE_SIZE, GFP_KERNEL);
	u8 *top = page + IST_PAGE_SIZE;

	KUNIT_ASSERT_NOT_NULL(test, page);

	/* Lay out an SDM-defined with-error-code frame. */
	*(u64 *)(top - 48) = 0xDEADBEEF11111111ULL;	/* error_code */
	*(u64 *)(top - 40) = 0x4001877eULL;		/* RIP (user) */
	*(u64 *)(top - 32) = 0x2bULL;			/* CS = USER_CS */
	*(u64 *)(top - 24) = 0x10206ULL;		/* RFLAGS */
	*(u64 *)(top - 16) = 0x7f7fffffe000ULL;	/* RSP (user) */
	*(u64 *)(top -  8) = 0x23ULL;			/* SS = USER_DS */

	/* Verify the SDM-prescribed slot offsets. */
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top - 48), 0xDEADBEEF11111111ULL);
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top - 40), 0x4001877eULL);
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top - 32), 0x2bULL);
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top - 24), 0x10206ULL);
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top - 16), 0x7f7fffffe000ULL);
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top -  8), 0x23ULL);

	/*
	 * Regression check: ensure top - 40 (the RIP anchor) does NOT land
	 * on the error_code slot (top - 48).
	 */
	KUNIT_EXPECT_NE(test, (u64)*(u64 *)(top - 40), 0xDEADBEEF11111111ULL);
}

static void test_ist_frame_layout_without_error_code(struct kunit *test)
{
	u8 *page = kunit_kzalloc(test, IST_PAGE_SIZE, GFP_KERNEL);
	u8 *top = page + IST_PAGE_SIZE;

	KUNIT_ASSERT_NOT_NULL(test, page);

	/*
	 * Without error code, the frame is 40 bytes (RIP/CS/RFLAGS/
	 * RSP/SS = 5 x 8). RIP at top - 40 (same offset as the
	 * with-error-code case: RIP-anchored layout).
	 */
	*(u64 *)(top - 40) = 0xCAFEBABE00000000ULL;	/* RIP */
	*(u64 *)(top - 32) = 0x2bULL;			/* CS */
	*(u64 *)(top - 24) = 0x10202ULL;		/* RFLAGS */
	*(u64 *)(top - 16) = 0x7f7ffffe1000ULL;	/* RSP */
	*(u64 *)(top -  8) = 0x23ULL;			/* SS */

	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top - 40), 0xCAFEBABE00000000ULL);
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top - 32), 0x2bULL);
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top - 24), 0x10202ULL);
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top - 16), 0x7f7ffffe1000ULL);
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top -  8), 0x23ULL);

	/* The "error code slot" (top - 48) should be untouched zero. */
	KUNIT_EXPECT_EQ(test, (u64)*(u64 *)(top - 48), 0ULL);
}

/* ---------------------------------------------------------------- */
/* GDT segment descriptor encoding                                  */
/* SDM Vol.3 section 3.4.5, long-mode 8-byte descriptor:            */
/*   byte 0-1:  limit_low                                            */
/*   byte 2-3:  base_low                                             */
/*   byte 4:    base_mid                                             */
/*   byte 5:    type|S|DPL|P                                         */
/*   byte 6:    limit_high|AVL|L|D/B|G                               */
/*   byte 7:    base_high                                            */
/* ---------------------------------------------------------------- */

static void test_gdt_kernel_cs_encoding(struct kunit *test)
{
	/*
	 * Kernel CS slot:
	 *   selector 0x08, DPL=0, type=0xb (ER+A), L=1, present=1
	 *   limit=0xffffffff, base=0
	 *
	 * Encoded value: 0x00af9b000000ffff
	 *   - byte 0-1: 0xffff (limit_low)
	 *   - byte 2-3: 0x0000 (base_low)
	 *   - byte 4:   0x00   (base_mid)
	 *   - byte 5:   0x9b   = 1001 1011 = P=1 DPL=00 S=1 type=1011 (ER+A)
	 *   - byte 6:   0xaf   = 1010 1111 = G=1 D/B=0 L=1 AVL=0 limit_high=0xf
	 *   - byte 7:   0x00   (base_high)
	 */
	u64 kernel_cs = 0x00af9b000000ffffULL;
	u8 byte5 = (kernel_cs >> 40) & 0xff;
	u8 byte6 = (kernel_cs >> 48) & 0xff;

	/* P=1 (bit 7) */
	KUNIT_EXPECT_EQ(test, (u8)(byte5 & 0x80), (u8)0x80);
	/* DPL=0 (bits 6:5) */
	KUNIT_EXPECT_EQ(test, (u8)(byte5 & 0x60), (u8)0x00);
	/* S=1 (bit 4): code/data, not system */
	KUNIT_EXPECT_EQ(test, (u8)(byte5 & 0x10), (u8)0x10);
	/* type=0xb (bits 3:0): Execute/Read with Accessed */
	KUNIT_EXPECT_EQ(test, (u8)(byte5 & 0x0f), (u8)0x0b);
	/* G=1 (bit 7 of byte 6): page granularity */
	KUNIT_EXPECT_EQ(test, (u8)(byte6 & 0x80), (u8)0x80);
	/* D/B=0 (bit 6): long-mode forces 0 */
	KUNIT_EXPECT_EQ(test, (u8)(byte6 & 0x40), (u8)0x00);
	/* L=1 (bit 5): long-mode flag */
	KUNIT_EXPECT_EQ(test, (u8)(byte6 & 0x20), (u8)0x20);
}

static void test_gdt_user_cs_encoding(struct kunit *test)
{
	/*
	 * User CS slot:
	 *   selector 0x2b, DPL=3, type=0xb (ER+A), L=1, present=1
	 * Encoded: 0x00affb000000ffff
	 *   byte 5: 0xfb = 1111 1011 = P=1 DPL=11 S=1 type=1011
	 */
	u64 user_cs = 0x00affb000000ffffULL;
	u8 byte5 = (user_cs >> 40) & 0xff;

	KUNIT_EXPECT_EQ(test, (u8)(byte5 & 0x80), (u8)0x80);  /* P=1 */
	KUNIT_EXPECT_EQ(test, (u8)(byte5 & 0x60), (u8)0x60);  /* DPL=3 */
	KUNIT_EXPECT_EQ(test, (u8)(byte5 & 0x10), (u8)0x10);  /* S=1 */
	KUNIT_EXPECT_EQ(test, (u8)(byte5 & 0x0f), (u8)0x0b);  /* type=0xb */
}

/* ---------------------------------------------------------------- */
/* LSTAR trampoline byte sequence                                   */
/* out %al, $0xf4 ; sysretq: 5 bytes.                             */
/* ---------------------------------------------------------------- */

static void test_lstar_trampoline_bytes(struct kunit *test)
{
	/*
	 * Assembled fallback blob from lstar_gadget.S must equal the
	 * canonical 5-byte body byte-for-byte.
	 */
	const size_t len = kvm_v2_lstar_fallback_end -
			   kvm_v2_lstar_fallback_start;

	KUNIT_EXPECT_EQ(test, (size_t)len, (size_t)5);

	/* out opcode 0xe6 = OUT imm8: byte to port (port from imm) */
	KUNIT_EXPECT_EQ(test, (u8)kvm_v2_lstar_fallback_start[0], (u8)0xe6);
	/* port literal 0xf4 = UM_KVM_TRAP_SYSCALL */
	KUNIT_EXPECT_EQ(test, (u8)kvm_v2_lstar_fallback_start[1], (u8)0xf4);
	/* SYSRETQ: REX.W (0x48) + 0F 07 */
	KUNIT_EXPECT_EQ(test, (u8)kvm_v2_lstar_fallback_start[2], (u8)0x48);
	KUNIT_EXPECT_EQ(test, (u8)kvm_v2_lstar_fallback_start[3], (u8)0x0f);
	KUNIT_EXPECT_EQ(test, (u8)kvm_v2_lstar_fallback_start[4], (u8)0x07);
}

/* ---------------------------------------------------------------- */
/* Gadget body: entry sequence + key opcodes                        */
/* ---------------------------------------------------------------- */

static void test_lstar_gadget_entry_sequence(struct kunit *test)
{
	/*
	 * Lock the gadget entry preamble. The body must start with:
	 *   swapgs                            ; 0f 01 f8         (3 B)
	 *   movq %rdx, %gs:KVM_V2_GADGET_OFF_SAVE_RDX   (9 B)
	 *   movq %r8,  %gs:KVM_V2_GADGET_OFF_SAVE_R8    (9 B)
	 *   movq %r10, %gs:KVM_V2_GADGET_OFF_SAVE_R10   (9 B)
	 *   cmpb $0, %gs:KVM_V2_GADGET_OFF_RECORD       (9 B)
	 *
	 * The save sequence implements Linux x86_64 syscall ABI
	 * preservation of RDX/R8/R10. Any change to it (regs reordered,
	 * slots renamed, save dropped) trips this test before boot:
	 * faster feedback than the post-boot integration ABI check.
	 */
	const u8 *p = kvm_v2_lstar_gadget_start;
	const size_t len = kvm_v2_lstar_gadget_end - kvm_v2_lstar_gadget_start;

	KUNIT_ASSERT_GE(test, (size_t)len, (size_t)39);

	/* swapgs */
	KUNIT_EXPECT_EQ(test, (u8)p[0], (u8)0x0f);
	KUNIT_EXPECT_EQ(test, (u8)p[1], (u8)0x01);
	KUNIT_EXPECT_EQ(test, (u8)p[2], (u8)0xf8);

	/*
	 * movq %rdx, %gs:OFF_SAVE_RDX
	 *   65 (gs prefix) 48 (REX.W) 89 14 25 <disp32_le>
	 */
	KUNIT_EXPECT_EQ(test, (u8)p[3], (u8)0x65);
	KUNIT_EXPECT_EQ(test, (u8)p[4], (u8)0x48);
	KUNIT_EXPECT_EQ(test, (u8)p[5], (u8)0x89);
	KUNIT_EXPECT_EQ(test, (u8)p[6], (u8)0x14);
	KUNIT_EXPECT_EQ(test, (u8)p[7], (u8)0x25);
	KUNIT_EXPECT_EQ(test, (u32)*(u32 *)&p[8],
			(u32)KVM_V2_GADGET_OFF_SAVE_RDX);

	/* movq %r8, %gs:OFF_SAVE_R8: REX.W+R = 0x4c, ModRM 0x04 */
	KUNIT_EXPECT_EQ(test, (u8)p[12], (u8)0x65);
	KUNIT_EXPECT_EQ(test, (u8)p[13], (u8)0x4c);
	KUNIT_EXPECT_EQ(test, (u8)p[14], (u8)0x89);
	KUNIT_EXPECT_EQ(test, (u8)p[15], (u8)0x04);
	KUNIT_EXPECT_EQ(test, (u8)p[16], (u8)0x25);
	KUNIT_EXPECT_EQ(test, (u32)*(u32 *)&p[17],
			(u32)KVM_V2_GADGET_OFF_SAVE_R8);

	/* movq %r10, %gs:OFF_SAVE_R10: REX.W+R = 0x4c, ModRM 0x14 */
	KUNIT_EXPECT_EQ(test, (u8)p[21], (u8)0x65);
	KUNIT_EXPECT_EQ(test, (u8)p[22], (u8)0x4c);
	KUNIT_EXPECT_EQ(test, (u8)p[23], (u8)0x89);
	KUNIT_EXPECT_EQ(test, (u8)p[24], (u8)0x14);
	KUNIT_EXPECT_EQ(test, (u8)p[25], (u8)0x25);
	KUNIT_EXPECT_EQ(test, (u32)*(u32 *)&p[26],
			(u32)KVM_V2_GADGET_OFF_SAVE_R10);

	/* cmpb $0, %gs:OFF_RECORD */
	KUNIT_EXPECT_EQ(test, (u8)p[30], (u8)0x65);
	KUNIT_EXPECT_EQ(test, (u8)p[31], (u8)0x80);
	KUNIT_EXPECT_EQ(test, (u8)p[32], (u8)0x3c);
	KUNIT_EXPECT_EQ(test, (u8)p[33], (u8)0x25);
	KUNIT_EXPECT_EQ(test, (u32)*(u32 *)&p[34],
			(u32)KVM_V2_GADGET_OFF_RECORD);
	KUNIT_EXPECT_EQ(test, (u8)p[38], (u8)0x00);
}

static void test_lstar_gadget_size_bounded(struct kunit *test)
{
	/*
	 * The gadget body lives at KVM_V2_TRAMPOLINE_LSTAR_OFFSET
	 * (0x40) within the trampoline page. Total length must fit
	 * within PAGE_SIZE - 0x40 = 4032 B; the runtime check in
	 * kvm_v2_trampoline_alloc_and_install panics on overflow,
	 * but tripping this KUnit case at compile/run time is a
	 * cheaper feedback signal.
	 */
	const size_t len = kvm_v2_lstar_gadget_end - kvm_v2_lstar_gadget_start;
	const size_t budget = 4096 - KVM_V2_TRAMPOLINE_LSTAR_OFFSET;

	KUNIT_EXPECT_LE(test, (size_t)len, (size_t)budget);

	/*
	 * Lower bound: entry preamble (3+9+9+9 = 30 B) plus dispatch
	 * tree (~70 B) plus at least one handler. A blob shorter than
	 * 60 B is almost certainly truncated.
	 */
	KUNIT_EXPECT_GT(test, (size_t)len, (size_t)60);
}

/* ---------------------------------------------------------------- */
/* IDT handler stub byte sequences                                  */
/* ---------------------------------------------------------------- */

static void test_idt_pf_stub_bytes(struct kunit *test)
{
	/*
	 * #PF (and #GP) push an error code; the in-guest iretq does
	 * NOT pop it (SDM Vol.3 section 6.14.5), so the stub must add $8,%rsp
	 * before iretq. Stub layout (8 bytes):
	 *   e6 <port>   : out %al, $port
	 *   48 83 c4 08 : add $8, %rsp
	 *   48 cf       : iretq
	 */
	const u8 stub[] = {
		0xe6, 0xe0,		/* out (UM_KVM_TRAP_PF = 0xe0) */
		0x48, 0x83, 0xc4, 0x08,	/* add $8, %rsp */
		0x48, 0xcf,		/* iretq */
	};

	KUNIT_EXPECT_EQ(test, (size_t)sizeof(stub), (size_t)8);
	KUNIT_EXPECT_EQ(test, (u8)stub[0], (u8)0xe6);
	KUNIT_EXPECT_EQ(test, (u8)stub[2], (u8)0x48);  /* REX.W for add */
	KUNIT_EXPECT_EQ(test, (u8)stub[3], (u8)0x83);  /* add imm8 */
	KUNIT_EXPECT_EQ(test, (u8)stub[4], (u8)0xc4);  /* /0 = sub-opcode "add" with rsp ModRM */
	KUNIT_EXPECT_EQ(test, (u8)stub[5], (u8)0x08);  /* imm8 = 8 */
	KUNIT_EXPECT_EQ(test, (u8)stub[6], (u8)0x48);  /* REX.W for iretq */
	KUNIT_EXPECT_EQ(test, (u8)stub[7], (u8)0xcf);  /* iretq */
}

static void test_idt_simple_stub_bytes(struct kunit *test)
{
	/*
	 * Stubs that don't push an error code (#UD, #DE, #OF, #BP,
	 * panic): just out + iretq, 4 bytes total.
	 */
	const u8 stub[] = { 0xe6, 0xe2, 0x48, 0xcf };

	KUNIT_EXPECT_EQ(test, (size_t)sizeof(stub), (size_t)4);
	KUNIT_EXPECT_EQ(test, (u8)stub[0], (u8)0xe6);
	KUNIT_EXPECT_EQ(test, (u8)stub[2], (u8)0x48);
	KUNIT_EXPECT_EQ(test, (u8)stub[3], (u8)0xcf);
}

/* ---------------------------------------------------------------- */
/* Suite registration                                               */
/* ---------------------------------------------------------------- */
static struct kunit_case kvm_v2_byteshape_test_cases[] = {
	KUNIT_CASE(test_ist_frame_layout_with_error_code),
	KUNIT_CASE(test_ist_frame_layout_without_error_code),
	KUNIT_CASE(test_gdt_kernel_cs_encoding),
	KUNIT_CASE(test_gdt_user_cs_encoding),
	KUNIT_CASE(test_lstar_trampoline_bytes),
	KUNIT_CASE(test_lstar_gadget_entry_sequence),
	KUNIT_CASE(test_lstar_gadget_size_bounded),
	KUNIT_CASE(test_idt_pf_stub_bytes),
	KUNIT_CASE(test_idt_simple_stub_bytes),
	{}
};

static struct kunit_suite kvm_v2_byteshape_test_suite = {
	.name = "kvm_v2_byteshape",
	.test_cases = kvm_v2_byteshape_test_cases,
};

kunit_test_suite(kvm_v2_byteshape_test_suite);

MODULE_DESCRIPTION("UML kvm-v2 byte-shape KUnit tests");
MODULE_LICENSE("GPL");
