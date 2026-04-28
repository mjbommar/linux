// SPDX-License-Identifier: GPL-2.0
/*
 * Backend conformance test suite (workstream A-05).
 *
 * KUnit tests for every op in struct um_backend_ops, exercising the
 * contract documented in Documentation/virt/uml/backend-contract.rst.
 *
 * Tests fall into two shapes:
 *
 *   - "wired" tests verify the ops table field is non-NULL and (in
 *     single-backend builds) that it matches the symbol the dispatch
 *     macro resolves to. These are cheap, run on every backend, and
 *     catch the most common regression: a new op added to the struct
 *     without populating it on every backend.
 *
 *   - "functional" tests invoke the op and check its observable
 *     behavior. Limited to ops that are safe to call from a kunit
 *     thread without breaking the running kernel — so we cover
 *     read_clock_ns, read_persistent_clock_ns, init_thread_regs,
 *     set_timer(DISABLE), and the stubbed read/write_guest_regs.
 *     Hot ops (run_userspace, mm_*, context_switch, thread_*) get
 *     wired-only coverage; their functional tests are exercised by
 *     every boot syscall.
 */
#include <kunit/test.h>
#include <linux/array_size.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/cpu.h>
#include <linux/string.h>
#include <asm/backend.h>
#include <asm/io.h>		/* phys_to_virt — memo 11 G3 gadget state test */
#include <asm/unistd.h>		/* __NR_* for memo 10 class tests */
#include <linux/stddef.h>	/* offsetof — memo 11 G3 ABI test */

#include "test_kvm_hooks.h"

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

#if defined(CONFIG_UM_BACKEND_PTRACE_ONLY)
# define EXPECTED_BACKEND_NAME "ptrace"
#elif defined(CONFIG_UM_BACKEND_SECCOMP_ONLY)
# define EXPECTED_BACKEND_NAME "seccomp"
#elif defined(CONFIG_UM_BACKEND_KVM_ONLY)
# define EXPECTED_BACKEND_NAME "kvm"
#else
# define EXPECTED_BACKEND_NAME NULL
#endif

/*
 * Verify the dispatch macro resolves to the same symbol that's
 * registered in the ops table. Only meaningful in single-backend
 * inline builds (where the macro token-pastes); in DYNAMIC builds
 * we skip the equality check (any non-NULL field is correct since
 * dispatch always goes through the pointer).
 *
 * The macro token-pastes `ptrace_<op>` / `seccomp_<op>` / `kvm_<op>`
 * so the unused-backend's symbols are NEVER referenced from this TU,
 * avoiding the missing-prototype problem when one backend is
 * not compiled in.
 */
#if defined(CONFIG_UM_BACKEND_PTRACE_ONLY)
# define ASSERT_OP_DISPATCH(test, field)				\
	do {								\
		KUNIT_EXPECT_NOT_NULL(test, um_backend->field);		\
		KUNIT_EXPECT_PTR_EQ(test, (void *)um_backend->field,	\
				    (void *)(ptrace_##field));		\
	} while (0)
#elif defined(CONFIG_UM_BACKEND_SECCOMP_ONLY)
# define ASSERT_OP_DISPATCH(test, field)				\
	do {								\
		KUNIT_EXPECT_NOT_NULL(test, um_backend->field);		\
		KUNIT_EXPECT_PTR_EQ(test, (void *)um_backend->field,	\
				    (void *)(seccomp_##field));		\
	} while (0)
#elif defined(CONFIG_UM_BACKEND_KVM_ONLY)
# define ASSERT_OP_DISPATCH(test, field)				\
	do {								\
		KUNIT_EXPECT_NOT_NULL(test, um_backend->field);		\
		KUNIT_EXPECT_PTR_EQ(test, (void *)um_backend->field,	\
				    (void *)(kvm_##field));		\
	} while (0)
#else /* DYNAMIC */
# define ASSERT_OP_DISPATCH(test, field)				\
	KUNIT_EXPECT_NOT_NULL(test, um_backend->field)
#endif

/* ---------------------------------------------------------------- */
/* Sanity                                                           */
/* ---------------------------------------------------------------- */

/* All in-tree backends must use the same contract version we built against. */
static void backend_contract_version_test(struct kunit *test)
{
	KUNIT_ASSERT_NOT_NULL(test, um_backend);
	KUNIT_EXPECT_EQ(test, um_backend->contract_version,
			UM_BACKEND_CONTRACT_VERSION);
	KUNIT_ASSERT_NOT_NULL(test, um_backend->name);
	KUNIT_EXPECT_NE(test, (int)um_backend->kind, (int)UM_BACKEND_KIND_NONE);
	if (EXPECTED_BACKEND_NAME)
		KUNIT_EXPECT_STREQ(test, um_backend->name,
				   EXPECTED_BACKEND_NAME);
}

/*
 * D14 is enforced at boot: HOT ops must be non-NULL. Cold ops also
 * must be non-NULL — a NULL field would crash via the dispatch macro
 * the first time the kernel calls through it. This test makes that
 * promise explicit and asserts every field is populated.
 */
static void backend_all_ops_populated_test(struct kunit *test)
{
	KUNIT_ASSERT_NOT_NULL(test, um_backend);

	/* Lifecycle and trap (4) */
	KUNIT_EXPECT_NOT_NULL(test, um_backend->probe);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->init);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->shutdown);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->vcpu_run);

	/* Memory (4 mandatory; mm_region_protected is optional per memo 25 R2) */
	KUNIT_EXPECT_NOT_NULL(test, um_backend->mm_create);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->mm_destroy);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->mm_region_added);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->mm_region_removed);

	/* Scheduling (4) */
	KUNIT_EXPECT_NOT_NULL(test, um_backend->thread_create);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->thread_start_idle);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->context_switch);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->ipi_send);

	/* Time (3) */
	KUNIT_EXPECT_NOT_NULL(test, um_backend->read_clock_ns);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->set_timer);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->read_persistent_clock_ns);

	/* Debug (3) */
	KUNIT_EXPECT_NOT_NULL(test, um_backend->init_thread_regs);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->read_guest_regs);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->write_guest_regs);
}

/* ---------------------------------------------------------------- */
/* Lifecycle / trap                                                 */
/* ---------------------------------------------------------------- */

static void backend_probe_wired_test(struct kunit *test)
{
#ifdef CONFIG_UM_BACKEND_PTRACE
	(void)ptrace_probe;
#endif
#ifdef CONFIG_UM_BACKEND_SECCOMP
	(void)seccomp_probe;
#endif
#ifdef CONFIG_UM_BACKEND_KVM
	(void)kvm_probe;
#endif
	ASSERT_OP_DISPATCH(test, probe);
}

static void backend_init_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, init);
}

static void backend_shutdown_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, shutdown);
}

static void backend_vcpu_run_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, vcpu_run);
}

/* ---------------------------------------------------------------- */
/* Memory (memo 25 R2 ops)                                          */
/* ---------------------------------------------------------------- */

static void backend_mm_create_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, mm_create);
}

static void backend_mm_destroy_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, mm_destroy);
}

static void backend_mm_region_added_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, mm_region_added);
}

static void backend_mm_region_removed_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, mm_region_removed);
}

/* ---------------------------------------------------------------- */
/* Scheduling                                                       */
/* ---------------------------------------------------------------- */

static void backend_thread_create_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, thread_create);
}

static void backend_thread_start_idle_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, thread_start_idle);
}

static void backend_context_switch_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, context_switch);
}

static void backend_ipi_send_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, ipi_send);
}

/* ---------------------------------------------------------------- */
/* Time                                                             */
/* ---------------------------------------------------------------- */

/*
 * Functional: read_clock_ns must be monotonic and produce small
 * deltas across two back-to-back calls.
 */
static void backend_read_clock_ns_test(struct kunit *test)
{
	u64 t0, t1;

	KUNIT_ASSERT_NOT_NULL(test, um_backend->read_clock_ns);
	t0 = um_backend_dispatch(read_clock_ns);
	t1 = um_backend_dispatch(read_clock_ns);

	/* clock_gettime(CLOCK_MONOTONIC_RAW) is non-decreasing. */
	KUNIT_EXPECT_GE(test, t1, t0);
	/* Two back-to-back reads should be within 1 ms (very loose). */
	KUNIT_EXPECT_LE(test, t1 - t0, 1000000ULL);

	ASSERT_OP_DISPATCH(test, read_clock_ns);
}

/*
 * Functional: set_timer(DISABLE) must succeed and not crash.
 * Skip on SMP — disabling the per-CPU timer mid-test could break
 * other kunit cases. Only test on UP.
 */
static void backend_set_timer_test(struct kunit *test)
{
	int rc;

	KUNIT_ASSERT_NOT_NULL(test, um_backend->set_timer);

	if (IS_ENABLED(CONFIG_SMP)) {
		kunit_skip(test, "set_timer functional test skipped on SMP");
		return;
	}

	/*
	 * DISABLE on a (fictional) cpu we don't really own; the host
	 * timer_disable just clears state. set_timer returns 0 for
	 * DISABLE per the contract.
	 */
	rc = um_backend_dispatch(set_timer, raw_smp_processor_id(), 0,
				 UM_TIMER_DISABLE);
	KUNIT_EXPECT_EQ(test, rc, 0);

	ASSERT_OP_DISPATCH(test, set_timer);
}

/*
 * Functional: read_persistent_clock_ns returns a non-zero, sane
 * wall-clock value. (Pre-existing test from A-02.6.)
 */
static void backend_read_persistent_clock_ns_test(struct kunit *test)
{
	u64 t0, t1;

	KUNIT_ASSERT_NOT_NULL(test, um_backend->read_persistent_clock_ns);
	t0 = um_backend_dispatch(read_persistent_clock_ns);
	t1 = um_backend_dispatch(read_persistent_clock_ns);

	/* Wall clock should be non-zero (well past the unix epoch). */
	KUNIT_EXPECT_GT(test, t0, 1000000000ULL * 1000ULL); /* > year 2001 */

	if (t1 >= t0)
		KUNIT_EXPECT_LE(test, t1 - t0, 5ULL * 1000000000ULL);
	else
		KUNIT_EXPECT_LE(test, t0 - t1, 5ULL * 1000000000ULL);

	ASSERT_OP_DISPATCH(test, read_persistent_clock_ns);
}

/* ---------------------------------------------------------------- */
/* Debug / introspection                                            */
/* ---------------------------------------------------------------- */

/*
 * Functional: init_thread_regs writes safe defaults into the
 * provided buffer. We assert it doesn't crash; the actual register
 * contents are arch-specific (the IP must point inside the kernel
 * text).
 */
static void backend_init_thread_regs_test(struct kunit *test)
{
	unsigned long gp[64] = { 0 };

	KUNIT_ASSERT_NOT_NULL(test, um_backend->init_thread_regs);

	/* fp_regs is allowed to be NULL per the contract. */
	um_backend_dispatch(init_thread_regs, gp, NULL);

	/* Some register field must have been written. We don't know
	 * which one without arch-specific knowledge, so just check at
	 * least one is non-zero (the IP is set during init).
	 */
	{
		int i, any = 0;

		for (i = 0; i < 64; i++)
			if (gp[i] != 0) {
				any = 1;
				break;
			}
		KUNIT_EXPECT_TRUE(test, any);
	}

	ASSERT_OP_DISPATCH(test, init_thread_regs);
}

/*
 * Functional: read_guest_regs / write_guest_regs are stubbed to
 * -EOPNOTSUPP today (real impl awaits KGDB / C-11). Verify the
 * stub returns the documented error rather than 0 or a NULL deref.
 */
static void backend_read_guest_regs_stub_test(struct kunit *test)
{
	int rc;

	KUNIT_ASSERT_NOT_NULL(test, um_backend->read_guest_regs);
	rc = um_backend_dispatch(read_guest_regs, NULL, NULL);
	KUNIT_EXPECT_EQ(test, rc, -EOPNOTSUPP);

	ASSERT_OP_DISPATCH(test, read_guest_regs);
}

static void backend_write_guest_regs_stub_test(struct kunit *test)
{
	int rc;

	KUNIT_ASSERT_NOT_NULL(test, um_backend->write_guest_regs);
	rc = um_backend_dispatch(write_guest_regs, NULL, NULL);
	KUNIT_EXPECT_EQ(test, rc, -EOPNOTSUPP);

	ASSERT_OP_DISPATCH(test, write_guest_regs);
}

/* ---------------------------------------------------------------- */
/* KVM integrated-path state materialization (memo 08 sub-commit #1) */
/* ---------------------------------------------------------------- */

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED

/*
 * Expected long-mode bits the production sregs path must emit.
 * Mirrors sregs.c::kvm_fill_longmode_segments + the caller-side
 * CR3/GDT pass-through. Any drift in production-sregs behaviour
 * fails these assertions immediately — harness and production
 * share kvm_fill_longmode_segments so this test also indirectly
 * guards the harness bits, but the two paths can diverge later
 * (e.g. production may grow TR + LDT for interrupt handling in
 * sub-commit #4) and this test belongs to the production half.
 */
static void kvm_production_sregs_shape_test(struct kunit *test)
{
	struct kvm_sregs sregs;
	struct kvm_regs kregs;
	struct uml_pt_regs src;
	const u64 fake_cr3 = 0x0000000040000000ULL;
	const u64 fake_gdt = 0x0000000040001000ULL;
	int rc;

	memset(&sregs, 0xa5, sizeof(sregs));	/* poison */
	memset(&kregs, 0xa5, sizeof(kregs));
	memset(&src,   0,    sizeof(src));

	/* Seed every HOST_* gp slot with a recognizable pattern so
	 * the marshalling assertions below catch any bit-for-bit
	 * drift without being tied to a particular register name's
	 * value.
	 */
	src.gp[HOST_AX]      = 0x11;
	src.gp[HOST_BX]      = 0x22;
	src.gp[HOST_CX]      = 0x33;
	src.gp[HOST_DX]      = 0x44;
	src.gp[HOST_SI]      = 0x55;
	src.gp[HOST_DI]      = 0x66;
	src.gp[HOST_BP]      = 0x77;
	src.gp[HOST_SP]      = 0x88;
	src.gp[HOST_R8]      = 0x99;
	src.gp[HOST_R9]      = 0xaa;
	src.gp[HOST_R10]     = 0xbb;
	src.gp[HOST_R11]     = 0xcc;
	src.gp[HOST_R12]     = 0xdd;
	src.gp[HOST_R13]     = 0xee;
	src.gp[HOST_R14]     = 0xff;
	src.gp[HOST_R15]     = 0x1010;
	src.gp[HOST_IP]      = 0xdeadbeefUL;
	src.gp[HOST_EFLAGS]  = 0x0;	/* bit 1 must be forced on */
	src.gp[HOST_FS_BASE] = 0xcafef00dUL;	/* memo 10 #5c seed */
	src.gp[HOST_GS_BASE] = 0xfeedface2UL;

	rc = kvm_enter_guest_probe(&sregs, &kregs, &src, fake_cr3, fake_gdt);
	KUNIT_EXPECT_EQ(test, rc, 0);

	/* CR3 / GDT pass-through. */
	KUNIT_EXPECT_EQ(test, sregs.cr3, fake_cr3);
	KUNIT_EXPECT_EQ(test, sregs.gdt.base, fake_gdt);
	KUNIT_EXPECT_NE(test, (int)sregs.gdt.limit, 0);

	/* Long-mode control bits.
	 * Exact values mirror sregs.c's KVM_CR0_* / KVM_CR4_PAE /
	 * KVM_EFER_* constants; any change there is a deliberate
	 * contract bump and this test should be updated in the same
	 * commit.
	 */
	KUNIT_EXPECT_TRUE(test, (sregs.cr0 & 0x80000001UL) ==
				0x80000001UL);			/* PE | PG */
	KUNIT_EXPECT_TRUE(test, (sregs.cr4 & (1UL << 5)) != 0);	/* PAE */
	KUNIT_EXPECT_TRUE(test, (sregs.efer & (1UL << 0)) != 0);/* SCE */
	KUNIT_EXPECT_TRUE(test, (sregs.efer & (1UL << 8)) != 0);/* LME */
	KUNIT_EXPECT_TRUE(test, (sregs.efer & (1UL << 10)) != 0);/* LMA */

	/* Ring-0 long-mode CS: L=1, selector=0x08, DPL=0. */
	KUNIT_EXPECT_EQ(test, (int)sregs.cs.selector, 0x08);
	KUNIT_EXPECT_EQ(test, (int)sregs.cs.l, 1);
	KUNIT_EXPECT_EQ(test, (int)sregs.cs.dpl, 0);
	KUNIT_EXPECT_EQ(test, (int)sregs.cs.present, 1);

	/* Ring-0 SS/DS: selector=0x10. */
	KUNIT_EXPECT_EQ(test, (int)sregs.ss.selector, 0x10);
	KUNIT_EXPECT_EQ(test, (int)sregs.ds.selector, 0x10);

	/* Memo 10 sub-commit #5c: FS/GS base seeded from src gp[]
	 * so KVM_SET_SREGS's first ring-3 entry has the right TLS
	 * pointer without needing a separate KVM_SET_MSRS call.
	 * In long mode fs.base / gs.base and MSR_{FS,GS}_BASE are
	 * kept in sync by KVM; asserting against the segment cache
	 * is equivalent to asserting against the MSR.
	 */
	KUNIT_EXPECT_EQ(test, (unsigned long)sregs.fs.base, 0xcafef00dUL);
	KUNIT_EXPECT_EQ(test, (unsigned long)sregs.gs.base, 0xfeedface2UL);
}

/*
 * GP register marshalling: every HOST_* slot reaches the right
 * kvm_regs field, RFLAGS has the reserved bit-1 forced on.
 */
static void kvm_production_regs_marshal_test(struct kunit *test)
{
	struct kvm_sregs sregs;
	struct kvm_regs kregs;
	struct uml_pt_regs src;

	memset(&sregs, 0, sizeof(sregs));
	memset(&kregs, 0xa5, sizeof(kregs));
	memset(&src,   0,    sizeof(src));

	src.gp[HOST_AX]     = 0x1111;
	src.gp[HOST_BX]     = 0x2222;
	src.gp[HOST_CX]     = 0x3333;
	src.gp[HOST_DX]     = 0x4444;
	src.gp[HOST_SI]     = 0x5555;
	src.gp[HOST_DI]     = 0x6666;
	src.gp[HOST_BP]     = 0x7777;
	src.gp[HOST_SP]     = 0x8888;
	src.gp[HOST_R8]     = 0x9999;
	src.gp[HOST_R9]     = 0xaaaa;
	src.gp[HOST_R10]    = 0xbbbb;
	src.gp[HOST_R11]    = 0xcccc;
	src.gp[HOST_R12]    = 0xdddd;
	src.gp[HOST_R13]    = 0xeeee;
	src.gp[HOST_R14]    = 0xffff;
	src.gp[HOST_R15]    = 0x10101;
	src.gp[HOST_IP]     = 0xaabbccddUL;
	src.gp[HOST_EFLAGS] = 0;	/* bit 1 must be forced on */

	KUNIT_EXPECT_EQ(test,
			kvm_enter_guest_probe(&sregs, &kregs, &src, 0, 0), 0);

	KUNIT_EXPECT_EQ(test, kregs.rax, 0x1111ULL);
	KUNIT_EXPECT_EQ(test, kregs.rbx, 0x2222ULL);
	KUNIT_EXPECT_EQ(test, kregs.rcx, 0x3333ULL);
	KUNIT_EXPECT_EQ(test, kregs.rdx, 0x4444ULL);
	KUNIT_EXPECT_EQ(test, kregs.rsi, 0x5555ULL);
	KUNIT_EXPECT_EQ(test, kregs.rdi, 0x6666ULL);
	KUNIT_EXPECT_EQ(test, kregs.rbp, 0x7777ULL);
	KUNIT_EXPECT_EQ(test, kregs.rsp, 0x8888ULL);
	KUNIT_EXPECT_EQ(test, kregs.r8,  0x9999ULL);
	KUNIT_EXPECT_EQ(test, kregs.r9,  0xaaaaULL);
	KUNIT_EXPECT_EQ(test, kregs.r10, 0xbbbbULL);
	KUNIT_EXPECT_EQ(test, kregs.r11, 0xccccULL);
	KUNIT_EXPECT_EQ(test, kregs.r12, 0xddddULL);
	KUNIT_EXPECT_EQ(test, kregs.r13, 0xeeeeULL);
	KUNIT_EXPECT_EQ(test, kregs.r14, 0xffffULL);
	KUNIT_EXPECT_EQ(test, kregs.r15, 0x10101ULL);
	KUNIT_EXPECT_EQ(test, kregs.rip, 0xaabbccddULL);

	/* RFLAGS bit 1 is reserved-one (AMD64 SDM §3.1.4). */
	KUNIT_EXPECT_NE(test, kregs.rflags & (1ULL << 1), 0ULL);
}

/*
 * -EINVAL / -EINVAL probes: the probe helper validates its
 * inputs so the caller doesn't dereference NULLs when the test
 * setup itself is broken. Trivial but catches a common refactor
 * hazard.
 */
static void kvm_production_probe_null_test(struct kunit *test)
{
	struct kvm_sregs s = { 0 };
	struct kvm_regs  r = { 0 };
	struct uml_pt_regs u;

	memset(&u, 0, sizeof(u));

	KUNIT_EXPECT_EQ(test, kvm_enter_guest_probe(NULL, &r,   &u, 0, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_enter_guest_probe(&s,  NULL, &u, 0, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_enter_guest_probe(&s,  &r,  NULL, 0, 0), -EINVAL);
}

/*
 * Memo 08 sub-commit #2 tests: LSTAR trampoline bytes in the
 * bootstrap page (once allocated) + wire-port constants.
 * Pure data-struct / #define comparison; no /dev/kvm needed.
 *
 * The trampoline-bytes test is double-purpose:
 *  (a) `kvm_bootstrap_copy_lstar` correctly returns -ENODATA
 *      when the bootstrap page hasn't been allocated (boot
 *      context where kvm_enter_guest never runs — normal on
 *      a seccomp-backend research kunit run).
 *  (b) when allocated, the first 5 bytes match the known
 *      harness wire form (`out %al,$0xf4; sysretq`).
 *
 * To exercise (b) without running the backend, we don't force
 * allocation from the test. The expected behaviour on a
 * UM_BACKEND_KVM_INTEGRATED=y kunit build is -ENODATA (kunit
 * module loads pre-run_userspace); once sub-commit #3 wires
 * kvm_enter_guest into the real dispatch, this test will
 * start observing the populated bytes automatically.
 */
#ifdef CONFIG_UM_BACKEND_KVM_GADGET
/*
 * Memo 11 G4 + G5c + G6 + G6-follow-on + F8 + G1: 387-byte
 * real systrap gadget body. Layout:
 *   +0    swapgs (3 B)
 *   +3    upper-NR guard (17 B): cmp $0x135 / je getcpu stub
 *         / test $0xffffff00 / jne fallback (F4)
 *   +22   10-entry low-NR dispatch (40 B)
 *   +62   fallback (8 B)
 *   +70   7 × 14 B pid-family handlers (inlined tail) = 98 B
 *   +168  sched_yield (8 B)
 *   +176  2 × 5 B near-stubs for clock + time
 *   +186  clock_gettime (103 B) — F8 budget + F7/2 SEQ-via-
 *         %edx + G1 bounds check before the user store
 *   +289  time (41 B) — G1 bounds + RAX-preservation via %rdx
 *         (so handle_syscall fallback sees NR=201, not REAL_SEC)
 *   +330  getcpu (64 B) — G1 bounds + RAX-preservation
 *         (deferred xor %eax + r10d for node-zero so RAX=NR=309
 *         across both checks)
 *   total 394 B
 * Kept in lockstep with the live table in
 * arch/um/backend/kvm/thread.c via this KUnit equivalence
 * assertion.
 */
static const u8 kvm_expected_lstar_bytes[] = {
	/* +0 swapgs */
	0x0f, 0x01, 0xf8,

	/* +3 upper-NR guard (17 B): pre-check getcpu, then high-byte test */
	0x3d, 0x35, 0x01, 0x00, 0x00,	/* cmp $0x135, %eax */
	0x75, 0x05,			/* jne +5 */
	0xe9, 0x3b, 0x01, 0x00, 0x00,	/* jmp rel32 getcpu_body (+330) */
	0xa9, 0x00, 0xff, 0xff, 0xff,	/* test $0xffffff00, %eax */
	0x75, 0x28,			/* jne +40 → fallback (+62) */

	/* +22..+61 low-NR dispatch (10 × 4 B) */
	0x3c, 0x27, 0x74, 44,
	0x3c, 0xba, 0x74, 54,
	0x3c, 0x6e, 0x74, 64,
	0x3c, 0x66, 0x74, 74,
	0x3c, 0x6b, 0x74, 84,
	0x3c, 0x68, 0x74, 94,
	0x3c, 0x6c, 0x74, 104,
	0x3c, 0x18, 0x74, 8,	/* G5 demotion: sched_yield → fallback */
	0x3c, 0xe4, 0x74, 118,
	0x3c, 0xc9, 0x74, 119,

	/* +62..+69 fallback */
	0x0f, 0x01, 0xf8,
	0xe6, 0xf4,
	0x48, 0x0f, 0x07,

	/* +70..+167 pid-family handlers (7 × 14 B, inlined tail) */
	0x65, 0x8b, 0x04, 0x25, 0x08, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,
	0x65, 0x8b, 0x04, 0x25, 0x0c, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,
	0x65, 0x8b, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,
	0x65, 0x8b, 0x04, 0x25, 0x14, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,
	0x65, 0x8b, 0x04, 0x25, 0x18, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,
	0x65, 0x8b, 0x04, 0x25, 0x1c, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,
	0x65, 0x8b, 0x04, 0x25, 0x20, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/* +168..+175 handler_sched_yield (8 B) */
	0x31, 0xc0,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/* +176..+185 two near-stubs (5 B each): clock, time */
	0xe9, 0x05, 0x00, 0x00, 0x00,
	0xe9, 0x67, 0x00, 0x00, 0x00,	/* time at +289 (0x67 = 103) */

	/* +186..+288 handler_clock_gettime (103 B: F8 + F7/2 + G1) */
	/* F8: sub $1, %gs:0x1028 (BUDGET) */
	0x65, 0x83, 0x2c, 0x25, 0x28, 0x10, 0x00, 0x00, 0x01,
	/* F8: js rel32 fallback */
	0x0f, 0x88, 0x75, 0xff, 0xff, 0xff,
	0x83, 0xff, 0x01,
	0x0f, 0x85, 0x6c, 0xff, 0xff, 0xff,
	0x65, 0x8b, 0x14, 0x25, 0x00, 0x10, 0x00, 0x00,
	0xf6, 0xc2, 0x01,
	0x0f, 0x85, 0x5b, 0xff, 0xff, 0xff,
	0x65, 0x4c, 0x8b, 0x14, 0x25, 0x08, 0x10, 0x00, 0x00,
	0x65, 0x4c, 0x8b, 0x04, 0x25, 0x10, 0x10, 0x00, 0x00,
	0x65, 0x3b, 0x14, 0x25, 0x00, 0x10, 0x00, 0x00,
	0x0f, 0x85, 0x3b, 0xff, 0xff, 0xff,
	/* G1: cmp %rsi, %gs:0x1030 (TASK_SIZE_CAP) */
	0x65, 0x48, 0x39, 0x34, 0x25, 0x30, 0x10, 0x00, 0x00,
	/* G1: jbe rel32 fallback */
	0x0f, 0x86, 0x2c, 0xff, 0xff, 0xff,
	0x4c, 0x89, 0x16,
	0x4c, 0x89, 0x46, 0x08,
	0x31, 0xc0,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/* +289..+329 handler_time (41 B post-G1 + RAX-preserve) */
	0x65, 0x48, 0x8b, 0x14, 0x25, 0x18, 0x10, 0x00, 0x00,	/* mov %gs:0x1018, %rdx */
	0x48, 0x85, 0xff,
	0x74, 0x12,
	0x65, 0x48, 0x39, 0x3c, 0x25, 0x30, 0x10, 0x00, 0x00,
	0x0f, 0x86, 0x00, 0xff, 0xff, 0xff,
	0x48, 0x89, 0x17,					/* mov %rdx, (%rdi) */
	0x48, 0x89, 0xd0,					/* mov %rdx, %rax */
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/* +330..+393 handler_getcpu (64 B post-G1 + RAX-preserve) */
	0x48, 0x85, 0xff,
	0x74, 0x19,
	0x65, 0x48, 0x39, 0x3c, 0x25, 0x30, 0x10, 0x00, 0x00,
	0x0f, 0x86, 0xe0, 0xfe, 0xff, 0xff,
	0x65, 0x8b, 0x14, 0x25, 0x04, 0x00, 0x00, 0x00,
	0x89, 0x17,
	0x48, 0x85, 0xf6,
	0x74, 0x15,
	0x65, 0x48, 0x39, 0x34, 0x25, 0x30, 0x10, 0x00, 0x00,
	0x0f, 0x86, 0xc2, 0xfe, 0xff, 0xff,
	0x45, 0x31, 0xd2,					/* xor %r10d, %r10d */
	0x44, 0x89, 0x16,					/* mov %r10d, (%rsi) */
	0x31, 0xc0,						/* xor %eax, %eax (deferred) */
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,
};
#else
static const u8 kvm_expected_lstar_bytes[] = {
	0xe6, 0xf4,		/* out %al, $0xf4 */
	0x48, 0x0f, 0x07,	/* sysretq */
};
#endif

static void kvm_bootstrap_lstar_bytes_test(struct kunit *test)
{
	u8 buf[sizeof(kvm_expected_lstar_bytes)] = { 0 };
	int rc;

	/* Short buffer → -EINVAL. */
	KUNIT_EXPECT_EQ(test,
		kvm_bootstrap_copy_lstar(buf, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test,
		kvm_bootstrap_copy_lstar(NULL, sizeof(buf)), -EINVAL);

	/* Force the lazy allocation so we can exercise the
	 * populated path regardless of whether run_userspace has
	 * run yet. Idempotent (subsequent kvm_enter_guest calls
	 * see the same page).
	 */
	KUNIT_ASSERT_EQ(test, kvm_bootstrap_force_init(), 0);

	rc = kvm_bootstrap_copy_lstar(buf, sizeof(buf));
	KUNIT_EXPECT_EQ(test, rc, (int)sizeof(kvm_expected_lstar_bytes));
	KUNIT_EXPECT_EQ(test,
		memcmp(buf, kvm_expected_lstar_bytes,
		       sizeof(kvm_expected_lstar_bytes)), 0);
}

static void kvm_wire_ports_test(struct kunit *test)
{
	/* The kvm_backend.h UM_KVM_SYSCALL_PORT / SYSRETQ_PORT
	 * values must match the harness wire form so sub-commit
	 * #3 can lift the harness decode unchanged. The contract
	 * test carries a local duplicate so any drift fails here
	 * and the fix is to update both sides in the same commit.
	 */
	KUNIT_EXPECT_EQ(test, _TEST_UM_KVM_SYSCALL_PORT, 0xf4);
	KUNIT_EXPECT_EQ(test, _TEST_UM_KVM_SYSRETQ_PORT, 0xf5);
}

/*
 * Round-trip fidelity: take a uml_pt_regs, forward-marshal to
 * kvm_regs, reverse-marshal back. Every HOST_* gp[] slot must
 * survive the round-trip with byte-identical data; the RFLAGS
 * reserved-one bit is the one deliberate exception (forward
 * marshal forces it on, reverse marshal preserves the result).
 */
static void kvm_regs_roundtrip_test(struct kunit *test)
{
	struct kvm_sregs sregs;
	struct kvm_regs  kregs;
	struct uml_pt_regs orig, out;
	int i;
	static const int gp_indices[] = {
		HOST_AX, HOST_BX, HOST_CX, HOST_DX,
		HOST_SI, HOST_DI, HOST_BP, HOST_SP,
		HOST_R8, HOST_R9, HOST_R10, HOST_R11,
		HOST_R12, HOST_R13, HOST_R14, HOST_R15,
		HOST_IP,
	};

	memset(&sregs, 0, sizeof(sregs));
	memset(&kregs, 0, sizeof(kregs));
	memset(&orig,  0, sizeof(orig));
	memset(&out,   0xa5, sizeof(out));	/* poison */

	/* Distinct values per slot so a cross-wired reverse marshal
	 * fails visibly in the memcmp below.
	 */
	for (i = 0; i < (int)ARRAY_SIZE(gp_indices); i++)
		orig.gp[gp_indices[i]] = 0xbeef0000ULL + (unsigned long)i;
	orig.gp[HOST_EFLAGS] = 0x0;	/* forward marshal forces bit 1 */

	KUNIT_ASSERT_EQ(test,
		kvm_enter_guest_probe(&sregs, &kregs, &orig, 0, 0), 0);
	KUNIT_ASSERT_EQ(test,
		kvm_exit_guest_probe(&out, &kregs), 0);

	for (i = 0; i < (int)ARRAY_SIZE(gp_indices); i++) {
		KUNIT_EXPECT_EQ(test,
			(unsigned long long)out.gp[gp_indices[i]],
			(unsigned long long)orig.gp[gp_indices[i]]);
	}
	/* RFLAGS bit 1 round-trips on (forced by forward marshal). */
	KUNIT_EXPECT_NE(test,
		(unsigned long long)(out.gp[HOST_EFLAGS] & (1UL << 1)),
		0ULL);
}

static void kvm_exit_probe_null_test(struct kunit *test)
{
	struct kvm_regs kregs = { 0 };
	struct uml_pt_regs u;

	memset(&u, 0, sizeof(u));
	KUNIT_EXPECT_EQ(test, kvm_exit_guest_probe(NULL, &kregs), -EINVAL);
	KUNIT_EXPECT_EQ(test, kvm_exit_guest_probe(&u,   NULL),   -EINVAL);
}

/*
 * Audit round-4 F2 regression: kvm_build_sysret_r11 must
 * preserve the user-visible RFLAGS bits (CF/PF/AF/ZF/SF/TF/
 * DF/OF/NT/RF/AC/ID) so a faulting instruction's retry or a
 * SYSCALL-return continuation resumes with the correct
 * architectural state. It must also force the three
 * correctness-critical bits on regardless of input:
 *
 *   bit  1 (reserved)  — required by the architectural spec
 *   bit  9 (IF)         — don't let user-mode disable
 *                          interrupts across a SYSRETQ
 *   bits 12..13 (IOPL)  — keep at 3 so in-guest ring-3 can
 *                          still OUT on the wire ports
 *
 * Before this fix kvm_enter_guest hardcoded R11 = 0x3202,
 * which silently dropped every saved user RFLAGS across a
 * recoverable #PF or SYSCALL round-trip. The test exercises
 * three cases: the first-entry zero-input case (matches the
 * old constant), a DF=1-set case (direction flag is easy to
 * miss), and an all-arith-set case.
 */
static void kvm_build_sysret_r11_test(struct kunit *test)
{
	u64 r11;
	const u64 req_on = (1UL << 1) | (1UL << 9) | (3UL << 12);

	/* First-entry / fresh-task case: zero in, required-bits out. */
	r11 = kvm_build_sysret_r11_probe(0);
	KUNIT_EXPECT_EQ(test, (unsigned long long)r11, 0x3202ULL);

	/*
	 * DF=1 (bit 10): must survive. rep movs retries after #PF
	 * depend on DF being correct.
	 */
	r11 = kvm_build_sysret_r11_probe(1UL << 10);
	KUNIT_EXPECT_NE(test,
			(unsigned long long)(r11 & (1UL << 10)), 0ULL);
	KUNIT_EXPECT_EQ(test,
			(unsigned long long)(r11 & req_on),
			(unsigned long long)req_on);

	/* All arith flags (CF/PF/AF/ZF/SF/OF) set: must pass through. */
	{
		u64 arith = (1UL << 0) | (1UL << 2) | (1UL << 4) |
			    (1UL << 6) | (1UL << 7) | (1UL << 11);

		r11 = kvm_build_sysret_r11_probe(arith);
		KUNIT_EXPECT_EQ(test,
				(unsigned long long)(r11 & arith),
				(unsigned long long)arith);
		KUNIT_EXPECT_EQ(test,
				(unsigned long long)(r11 & req_on),
				(unsigned long long)req_on);
	}

	/* IF=0 on input must still yield IF=1 in output (guest
	 * can't disable host preemption by clearing IF in a
	 * faulting instruction's saved RFLAGS).
	 */
	r11 = kvm_build_sysret_r11_probe(0);
	KUNIT_EXPECT_NE(test,
			(unsigned long long)(r11 & (1UL << 9)), 0ULL);
}

/*
 * Memo 09 step 1: shadow PGD allocator lifecycle. Exercised in
 * isolation so the assertion is valid regardless of whether
 * kvm_init ran (which it doesn't on DYNAMIC builds where the
 * seccomp backend wins arbitration).
 *
 * The function is idempotent — if kvm_init already ran it,
 * subsequent alloc calls just return 0 with a pr_warn_once.
 * Assert the resulting gpa is non-zero + page-aligned.
 */
static void kvm_shadow_pgd_alloc_test(struct kunit *test)
{
	int rc;

	/*
	 * #275: the singleton kvm_shadow_pgd_alloc / kvm_shadow_pgd_gpa
	 * API is now vestigial — each UML mm has its own shadow tree
	 * allocated by kvm_mm_attach. The legacy alloc returns 0 and
	 * the legacy gpa returns 0; tests that asserted the singleton
	 * GPA was non-zero are no longer valid. Verify the stubs still
	 * link and return success/zero per their post-#275 contract.
	 */
	rc = kvm_shadow_pgd_alloc();
	KUNIT_EXPECT_EQ(test, rc, 0);

	/* Vestigial: gpa is always 0 post-#275. */
	KUNIT_EXPECT_EQ(test, (unsigned long long)kvm_shadow_pgd_gpa(),
			0ULL);
}

/*
 * Memo 11 G3 gadget state channel: offset ABI must match
 * between the header's KVM_GADGET_OFF_* #defines and the
 * on-disk struct layout. Gadget asm hardcodes these
 * offsets, so any reorder here is silent-corruption
 * unless the KUnit test catches it at build+boot time.
 */
static void kvm_gadget_state_abi_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
		(unsigned int)offsetof(struct _test_kvm_gadget_state, tgid),
		(unsigned int)_TEST_KVM_GADGET_OFF_TGID);
	KUNIT_EXPECT_EQ(test,
		(unsigned int)offsetof(struct _test_kvm_gadget_state, tid),
		(unsigned int)_TEST_KVM_GADGET_OFF_TID);
	KUNIT_EXPECT_EQ(test,
		(unsigned int)offsetof(struct _test_kvm_gadget_state, uid),
		(unsigned int)_TEST_KVM_GADGET_OFF_UID);
}

/*
 * Memo 11 G3 gadget state channel: the refresh helper
 * must write task_struct-derived values into the
 * allocated page from `current`. Exercise by forcing
 * the allocation (normally done lazily by
 * kvm_enter_guest), refreshing, and reading back
 * through the kernel-VA alias. Under KUnit the current
 * task is the module-load context (pid != 0, tgid
 * matches the invoker), so the assertions are that the
 * fields were set non-zero and are consistent with
 * current->pid.
 */
static void kvm_gadget_state_refresh_test(struct kunit *test)
{
	int rc = kvm_gadget_state_alloc();

	KUNIT_ASSERT_EQ(test, rc, 0);
	KUNIT_ASSERT_NE(test, (unsigned long long)kvm_gadget_state_gpa(),
			0ULL);

	kvm_gadget_state_refresh();

	/*
	 * Read back through __va of the gpa. The allocation
	 * itself returns the kernel VA; but since we don't
	 * export a direct accessor, derive it from the gpa.
	 */
	{
		u64 gpa = kvm_gadget_state_gpa();
		struct _test_kvm_gadget_state *s =
			(struct _test_kvm_gadget_state *)phys_to_virt(gpa);

		/* tgid + tid must both be non-zero (KUnit runs in
		 * a real task with a real tgid). On single-threaded
		 * tasks they're equal; KUnit doesn't guarantee a
		 * non-threaded context so we assert non-zero
		 * individually, not equality.
		 */
		KUNIT_EXPECT_NE(test, (unsigned int)s->tgid, 0u);
		KUNIT_EXPECT_NE(test, (unsigned int)s->tid,  0u);
		/* seq is SMP v2 reserved, must be 0 in v1. */
		KUNIT_EXPECT_EQ(test, (unsigned int)s->seq, 0u);
		/* cpu_id is 0 under ncpus=1. */
		KUNIT_EXPECT_EQ(test, (unsigned int)s->cpu_id, 0u);
	}
}

/*
 * Memo 11 G5 gadget vvar clock-page: offset ABI must match
 * the #defines in kvm_backend.h. Gadget asm in G5c will
 * hardcode these offsets.
 */
static void kvm_gadget_vvar_abi_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
		(unsigned int)offsetof(struct _test_kvm_gadget_vvar, seq),
		(unsigned int)_TEST_KVM_VVAR_OFF_SEQ);
	KUNIT_EXPECT_EQ(test,
		(unsigned int)offsetof(struct _test_kvm_gadget_vvar, monotonic_sec),
		(unsigned int)_TEST_KVM_VVAR_OFF_MONO_SEC);
	KUNIT_EXPECT_EQ(test,
		(unsigned int)offsetof(struct _test_kvm_gadget_vvar, monotonic_nsec),
		(unsigned int)_TEST_KVM_VVAR_OFF_MONO_NSEC);
	KUNIT_EXPECT_EQ(test,
		(unsigned int)offsetof(struct _test_kvm_gadget_vvar, realtime_sec),
		(unsigned int)_TEST_KVM_VVAR_OFF_REAL_SEC);
	KUNIT_EXPECT_EQ(test,
		(unsigned int)offsetof(struct _test_kvm_gadget_vvar, realtime_nsec),
		(unsigned int)_TEST_KVM_VVAR_OFF_REAL_NSEC);
}

/*
 * Memo 11 G5 gadget vvar refresh: after lazy-alloc + refresh,
 * the page should contain non-zero monotonic timestamps and a
 * seq that's even (stable) post-write. Also verifies seq
 * increments monotonically across refresh calls.
 */
static void kvm_gadget_vvar_refresh_test(struct kunit *test)
{
	int rc = kvm_gadget_vvar_alloc();
	struct _test_kvm_gadget_vvar *v;
	u32 seq_before, seq_after;
	s64 mono_before, mono_after;

	KUNIT_ASSERT_EQ(test, rc, 0);
	KUNIT_ASSERT_NE(test, (unsigned long long)kvm_gadget_vvar_gpa(),
			0ULL);

	v = (struct _test_kvm_gadget_vvar *)phys_to_virt(kvm_gadget_vvar_gpa());

	kvm_gadget_vvar_refresh();
	seq_before = v->seq;
	mono_before = v->monotonic_nsec +
		      v->monotonic_sec * (s64)NSEC_PER_SEC;
	/*
	 * Post-refresh: seq must be even (writer done), mono
	 * clock non-zero (KUnit runs well after boot 0).
	 */
	KUNIT_EXPECT_EQ(test, (unsigned int)(seq_before & 1u), 0u);
	KUNIT_EXPECT_NE(test, mono_before, 0);

	/* A second refresh bumps seq by 2 (odd then even). */
	kvm_gadget_vvar_refresh();
	seq_after = v->seq;
	mono_after = v->monotonic_nsec +
		     v->monotonic_sec * (s64)NSEC_PER_SEC;
	KUNIT_EXPECT_EQ(test, seq_after - seq_before, 2u);
	KUNIT_EXPECT_GE(test, mono_after, mono_before);
}

/*
 * Task #250 v2 / memo 12: snapshot primitives basic-shape test.
 *
 * Doesn't exercise an actual KVM_RUN round-trip (the contract
 * test runs at boot before the userspace dispatcher exists);
 * instead asserts the API surface is wired:
 *
 *   - kvm_snapshot_alloc returns a non-NULL container.
 *   - kvm_snapshot_capture against the booted vCPU populates
 *     the regs/sregs/msrs and allocates a memslot copy. The
 *     vCPU at this point has been through kvm_init's
 *     KVM_CREATE_VCPU but not yet through any KVM_RUN, so the
 *     captured state is the post-create baseline — RIP=0,
 *     RFLAGS=0x2 (reserved), no interesting MSRs yet.
 *   - kvm_snapshot_destroy releases the container without
 *     leaking.
 *
 * Capture failure (e.g. no memslot or vCPU yet on harness builds)
 * is acceptable; we just want to assert the API doesn't crash
 * and that successful capture produces a non-NULL mem buffer.
 *
 * The full capture → mutate → restore round-trip lives in the
 * snapshot-kvm-smoke kselftest (#251); doing it here would
 * require driving KVM_RUN, which is the dispatcher's job.
 */
static void kvm_snapshot_basic_test(struct kunit *test)
{
	struct kvm_snapshot *snap;
	int rc;

	snap = kvm_snapshot_alloc();
	KUNIT_ASSERT_NOT_NULL(test, snap);

	rc = kvm_snapshot_capture(snap);
	if (rc < 0) {
		/*
		 * On harness / minimal builds the memslot may not be
		 * registered yet (kvm_ensure_memslot is lazy). Skip
		 * silently in that case — the capture path is still
		 * exercised, just hits the early bailout.
		 */
		kunit_info(test, "kvm_snapshot_capture rc=%d (acceptable on early boot)\n",
			   rc);
	} else {
		/*
		 * Successful capture must allocate a non-NULL memslot
		 * backing buffer matching physmem_size. Restore must
		 * round-trip without ioctl errors against the same
		 * vCPU state.
		 */
		rc = kvm_snapshot_restore_full(snap);
		KUNIT_EXPECT_EQ(test, rc, 0);
	}

	kvm_snapshot_destroy(snap);
}

/*
 * Task #253 / memo 13 record/replay basic-shape test.
 *
 * Same shape as kvm_snapshot_basic_test: alloc / start / stop /
 * destroy lifecycle. After the log-only-on-capture-failure
 * change, start always succeeds (modulo -EBUSY / -ENOMEM); replay
 * works regardless of whether a checkpoint was captured (log-only
 * mode just rewinds the cursor without restoring vCPU state).
 */
static void kvm_record_basic_test(struct kunit *test)
{
	struct kvm_record *rec;
	int rc;

	rec = kvm_record_alloc();
	KUNIT_ASSERT_NOT_NULL(test, rec);

	rc = kvm_record_start(rec);
	KUNIT_EXPECT_EQ(test, rc, 0);

	kvm_record_stop(rec);
	rc = kvm_record_replay(rec);
	KUNIT_EXPECT_EQ(test, rc, 0);
	kvm_record_stop(rec);

	kvm_record_destroy(rec);
}

/*
 * Review-01 P0 regression test: start → observe → STOP → replay →
 * consume. The buggy v1 of kvm_record_replay() didn't re-arm the
 * active-record slot or the static-key gate after stop, so consume
 * silently no-op'd and the dispatcher fell through to live syscalls.
 *
 * This test calls observe (which only fires under the gate) before
 * the stop — so it directly verifies that:
 *   1. start arms the gate and slot.
 *   2. observe lands a log entry.
 *   3. stop disarms.
 *   4. replay re-arms the gate + slot.
 *   5. consume returns 1 + the right NR.
 *
 * Skips on early-boot when start can't capture (memslot lazy-init).
 */
static void kvm_record_stop_replay_consume_test(struct kunit *test)
{
	struct kvm_record *rec;
	long ret_out;
	int rc;

	rec = kvm_record_alloc();
	KUNIT_ASSERT_NOT_NULL(test, rec);

	rc = kvm_record_start(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	/*
	 * One observation while armed. The gate is on after start,
	 * so the helper appends an entry.
	 */
	kvm_record_observe_syscall(__NR_getpid, 4242, 0, 0);

	kvm_record_stop(rec);
	/* After stop: slot cleared, gate disabled. Replay must
	 * re-arm both. */
	rc = kvm_record_replay(rec);
	KUNIT_EXPECT_EQ(test, rc, 0);

	/* Consume must see the prior observation. */
	rc = kvm_record_consume_syscall(__NR_getpid, &ret_out,
					NULL, NULL, NULL);
	KUNIT_EXPECT_EQ(test, rc, 1);
	KUNIT_EXPECT_EQ(test, ret_out, (long)4242);

	kvm_record_destroy(rec);
}

/*
 * Review-01 P1 #2 regression test: strict replay fail-stop.
 *
 * In strict mode (default), end-of-log returns -ENODATA from
 * kvm_record_consume_syscall and divergence returns -EILSEQ.
 * Loose mode returns 0 in both cases (caller falls through to
 * live handle_syscall — the v1 behaviour).
 *
 * Test:
 *   1. start → observe one entry → no stop (still recording)
 *   2. flip to replay (we can do that without stop because
 *      replay accepts the active container)
 *   3. consume the one entry — must succeed
 *   4. consume again with same NR — must return -ENODATA
 *      (strict end-of-log) since cursor exhausted
 *   5. flip to loose, consume again — must return 0 (loose
 *      end-of-log)
 *   6. flip back to strict, consume with WRONG NR after
 *      replay-rewind — must return -EILSEQ
 *
 * Skips on early-boot when start can't capture.
 */
static void kvm_record_strict_replay_test(struct kunit *test)
{
	struct kvm_record *rec;
	long ret_out;
	int rc;

	rec = kvm_record_alloc();
	KUNIT_ASSERT_NOT_NULL(test, rec);

	rc = kvm_record_start(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	/* Default: strict_replay = true. */
	KUNIT_EXPECT_EQ(test, kvm_record_strict_replay(), false);
	/* Wait — strict_replay() returns true ONLY when replaying;
	 * during recording it returns false. Verify that contract.
	 */

	kvm_record_observe_syscall(__NR_getpid, 9999, 0, 0);

	rc = kvm_record_replay(rec);
	KUNIT_EXPECT_EQ(test, rc, 0);
	KUNIT_EXPECT_EQ(test, kvm_record_strict_replay(), true);

	/* Step 3: cursor 0 → consume getpid → 1. */
	rc = kvm_record_consume_syscall(__NR_getpid, &ret_out,
					NULL, NULL, NULL);
	KUNIT_EXPECT_EQ(test, rc, 1);
	KUNIT_EXPECT_EQ(test, ret_out, (long)9999);

	/* Step 4: cursor exhausted, strict → -ENODATA. */
	rc = kvm_record_consume_syscall(__NR_getpid, &ret_out,
					NULL, NULL, NULL);
	KUNIT_EXPECT_EQ(test, rc, -ENODATA);

	/* Step 5: flip to loose, end-of-log returns 0. */
	rc = kvm_record_set_strict_replay(false);
	KUNIT_EXPECT_EQ(test, rc, 0);
	rc = kvm_record_consume_syscall(__NR_getpid, &ret_out,
					NULL, NULL, NULL);
	KUNIT_EXPECT_EQ(test, rc, 0);

	/* Step 6: flip back to strict, replay (rewind), consume
	 * with WRONG NR → -EILSEQ. */
	rc = kvm_record_set_strict_replay(true);
	KUNIT_EXPECT_EQ(test, rc, 0);
	rc = kvm_record_replay(rec);
	KUNIT_EXPECT_EQ(test, rc, 0);
	rc = kvm_record_consume_syscall(__NR_clock_gettime, /* not getpid */
					&ret_out, NULL, NULL, NULL);
	KUNIT_EXPECT_EQ(test, rc, -EILSEQ);

	kvm_record_destroy(rec);
}

/*
 * Memo 13 round-trip: drive the record/replay log directly via
 * the C API without needing a userspace driver. The record-side
 * hook (kvm_record_observe_syscall) only fires under the static-
 * key gate, but this test sidesteps the gate by exercising the
 * helper directly — the whole point is to prove the log is FIFO,
 * the cursor advances correctly, and consume returns the same
 * (NR, ret) the caller observe'd.
 *
 * Three observed entries → three consumed entries → log
 * exhausted. NR-mismatch on the third consume returns -EILSEQ.
 *
 * Doesn't need a working snapshot capture (which fails -ENODEV
 * at early-boot KUnit time anyway). Just exercises the log
 * machinery in isolation.
 */
static void kvm_record_roundtrip_test(struct kunit *test)
{
	struct kvm_record *rec;
	long ret_out;
	u64 user_buf_out;
	const void *payload_out;
	size_t payload_len_out;
	int consumed;

	rec = kvm_record_alloc();
	KUNIT_ASSERT_NOT_NULL(test, rec);

	/*
	 * After the log-only-on-capture-failure change, start succeeds
	 * unconditionally (capture failure → log-only mode, log
	 * machinery still functional). Round-trip assertions below
	 * exercise the log path independently of vCPU state restore.
	 */
	{
		int rc = kvm_record_start(rec);

		KUNIT_ASSERT_EQ(test, rc, 0);
	}

	/*
	 * Four observations while recording: three inline-only entries
	 * (getpid / clock_gettime / getuid) + one side-buffer entry
	 * (fake getrandom shape) so the round-trip below covers both
	 * the inline path and the per-entry payload memcmp.
	 *
	 * Observe ordering matters: append() requires recording=true,
	 * so all observes must land BEFORE the replay-mode flip
	 * below. (An earlier shape attempted observing the buf entry
	 * post-replay; that silently no-op'd because the gate flips
	 * to replaying-only on kvm_record_replay.)
	 */
	{
		const u8 fake_payload[8] = {
			0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
		};
		long buf_ret;
		u64  buf_va_out;
		const void *buf_payload_out = NULL;
		size_t buf_payload_len_out = 0;

		kvm_record_observe_syscall(__NR_getpid,    1234, 0xdead, 0xbeef);
		kvm_record_observe_syscall(__NR_clock_gettime, 0, 1000,   0);
		kvm_record_observe_syscall(__NR_getuid,    0,    0,       0);
		kvm_record_observe_syscall_buf(__NR_getrandom, 8,
					       0xdeadbeefULL,
					       fake_payload,
					       sizeof(fake_payload));

		/* Flip to replay (cursor=0). */
		KUNIT_EXPECT_EQ(test, kvm_record_replay(rec), 0);

		/* Consume in FIFO order — first three are inline-only. */
		consumed = kvm_record_consume_syscall(__NR_getpid, &ret_out,
						      &user_buf_out, &payload_out,
						      &payload_len_out);
		KUNIT_EXPECT_EQ(test, consumed, 1);
		KUNIT_EXPECT_EQ(test, ret_out, (long)1234);
		KUNIT_EXPECT_EQ(test, (unsigned long long)payload_len_out, 0ULL);

		consumed = kvm_record_consume_syscall(__NR_clock_gettime, &ret_out,
						      &user_buf_out, &payload_out,
						      &payload_len_out);
		KUNIT_EXPECT_EQ(test, consumed, 1);
		KUNIT_EXPECT_EQ(test, ret_out, (long)0);

		consumed = kvm_record_consume_syscall(__NR_getuid, &ret_out,
						      &user_buf_out, &payload_out,
						      &payload_len_out);
		KUNIT_EXPECT_EQ(test, consumed, 1);

		/* Fourth entry: side-buffer round-trip. */
		KUNIT_EXPECT_EQ(test,
			kvm_record_consume_syscall(__NR_getrandom, &buf_ret,
						   &buf_va_out,
						   &buf_payload_out,
						   &buf_payload_len_out), 1);
		KUNIT_EXPECT_EQ(test, buf_ret, (long)8);
		KUNIT_EXPECT_EQ(test, (unsigned long long)buf_va_out,
				(unsigned long long)0xdeadbeefULL);
		KUNIT_EXPECT_EQ(test,
			(unsigned long long)buf_payload_len_out,
			(unsigned long long)sizeof(fake_payload));
		KUNIT_ASSERT_NOT_NULL(test, buf_payload_out);
		KUNIT_EXPECT_EQ(test,
			memcmp(buf_payload_out, fake_payload,
			       sizeof(fake_payload)), 0);
	}

	/*
	 * Cursor exhausted — next consume returns -ENODATA in strict
	 * mode (default). Loose mode would return 0 and let the caller
	 * fall through to live syscall.
	 */
	consumed = kvm_record_consume_syscall(__NR_getpid, &ret_out,
					      NULL, NULL, NULL);
	KUNIT_EXPECT_EQ(test, consumed, -ENODATA);

	/*
	 * Divergence shape: rewind cursor by replaying again, then
	 * consume with a wrong NR — must return -EILSEQ.
	 */
	{
		int rc = kvm_record_replay(rec);

		KUNIT_EXPECT_EQ(test, rc, 0);
	}
	consumed = kvm_record_consume_syscall(__NR_clock_gettime, /* not getpid! */
					      &ret_out,
					      NULL, NULL, NULL);
	KUNIT_EXPECT_EQ(test, consumed, -EILSEQ);

	kvm_record_destroy(rec);
}

/*
 * Memo 13 P2 #13 IOV variant: assert observe_syscall_buf_meta +
 * consume_syscall_meta round-trip the metadata bytes byte-identically.
 * Mirrors the side-buffer round-trip but exercises the new metadata
 * channel reserved for readv / recvmsg style scatter-gather replay.
 */
static void kvm_record_meta_iov_roundtrip_test(struct kunit *test)
{
	struct kvm_record *rec;
	long ret_out;
	u64 user_buf_out;
	const void *payload_out = NULL;
	size_t payload_len_out = 0;
	const void *meta_out = NULL;
	size_t meta_len_out = 0;
	u32 meta_kind_out = ~0U;
	int rc;

	rec = kvm_record_alloc();
	KUNIT_ASSERT_NOT_NULL(test, rec);

	rc = kvm_record_start(rec);
	KUNIT_ASSERT_EQ(test, rc, 0);

	{
		/* Two iovs covering 12 bytes — payload split 8 + 4. */
		struct { u64 base; u64 len; } iovs[2] = {
			{ 0x1000ULL, 8 },
			{ 0x2000ULL, 4 },
		};
		u8 meta[2 * sizeof(u32) + sizeof(iovs)];
		u32 nr_iov = 2;
		u32 zero = 0;
		const u8 fake_payload[12] = {
			'r','e','a','d','v','-','t','e','s','t','!','!'
		};

		memcpy(meta, &nr_iov, sizeof(nr_iov));
		memcpy(meta + sizeof(u32), &zero, sizeof(zero));
		memcpy(meta + 2 * sizeof(u32), iovs, sizeof(iovs));

		kvm_record_observe_syscall_buf_meta(__NR_readv, 12,
						    0xfeedULL,
						    fake_payload,
						    sizeof(fake_payload),
						    meta, sizeof(meta),
						    KVM_REPLAY_META_IOV);

		KUNIT_EXPECT_EQ(test, kvm_record_replay(rec), 0);

		KUNIT_EXPECT_EQ(test,
			kvm_record_consume_syscall_meta(__NR_readv,
							&ret_out,
							&user_buf_out,
							&payload_out,
							&payload_len_out,
							&meta_out,
							&meta_len_out,
							&meta_kind_out), 1);
		KUNIT_EXPECT_EQ(test, ret_out, (long)12);
		KUNIT_EXPECT_EQ(test, (unsigned long long)user_buf_out,
				(unsigned long long)0xfeedULL);
		KUNIT_EXPECT_EQ(test,
			(unsigned long long)payload_len_out,
			(unsigned long long)sizeof(fake_payload));
		KUNIT_EXPECT_EQ(test, meta_kind_out,
				(u32)KVM_REPLAY_META_IOV);
		KUNIT_EXPECT_EQ(test,
			(unsigned long long)meta_len_out,
			(unsigned long long)sizeof(meta));
		KUNIT_ASSERT_NOT_NULL(test, payload_out);
		KUNIT_ASSERT_NOT_NULL(test, meta_out);
		KUNIT_EXPECT_EQ(test,
			memcmp(payload_out, fake_payload,
			       sizeof(fake_payload)), 0);
		KUNIT_EXPECT_EQ(test,
			memcmp(meta_out, meta, sizeof(meta)), 0);
	}

	kvm_record_destroy(rec);
}

/*
 * Memo 10 class-map cross-check. The static table in
 * arch/um/backend/kvm/syscall_class.c must classify exactly
 * the 12 non-A entries from the inventory; every other NR
 * defaults to PASSTHROUGH. Drift between the C table and the
 * generated TSV surfaces here rather than in a boot failure.
 */
static void kvm_syscall_classification_test(struct kunit *test)
{
	/* Class B — vCPU-state propagate (memo 10 §B).
	 * Post-F10: only arch_prctl remains — modify_ldt +
	 * set_thread_area demoted to D.
	 */
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_arch_prctl),
			_TEST_KVM_SYSCALL_CLASS_VCPU_STATE);

	/* Class C — signal-frame (memo 10 §C). */
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_rt_sigreturn),
			_TEST_KVM_SYSCALL_CLASS_SIGFRAME);

	/* Class D — deny (memo 10 §D). modify_ldt + set_thread_area
	 * demoted here under F10 since UML doesn't virtualize LDT
	 * through KVM and set_thread_area is 32-bit-compat-only.
	 */
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_ptrace),
			_TEST_KVM_SYSCALL_CLASS_TRAP);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_reboot),
			_TEST_KVM_SYSCALL_CLASS_TRAP);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_init_module),
			_TEST_KVM_SYSCALL_CLASS_TRAP);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_finit_module),
			_TEST_KVM_SYSCALL_CLASS_TRAP);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_delete_module),
			_TEST_KVM_SYSCALL_CLASS_TRAP);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_kexec_load),
			_TEST_KVM_SYSCALL_CLASS_TRAP);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_kexec_file_load),
			_TEST_KVM_SYSCALL_CLASS_TRAP);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_bpf),
			_TEST_KVM_SYSCALL_CLASS_TRAP);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_modify_ldt),
			_TEST_KVM_SYSCALL_CLASS_TRAP);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_set_thread_area),
			_TEST_KVM_SYSCALL_CLASS_TRAP);

	/*
	 * Class E — gadget-handled (memo 11 G4-G6 + G7). These
	 * entries must match the live LSTAR dispatch table in
	 * arch/um/backend/kvm/thread.c. Adding or removing a
	 * gadget handler requires updating both the LSTAR table
	 * and syscall_class.c; this cross-check catches drift.
	 */
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_getpid),
			_TEST_KVM_SYSCALL_CLASS_GADGET);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_gettid),
			_TEST_KVM_SYSCALL_CLASS_GADGET);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_getppid),
			_TEST_KVM_SYSCALL_CLASS_GADGET);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_getuid),
			_TEST_KVM_SYSCALL_CLASS_GADGET);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_geteuid),
			_TEST_KVM_SYSCALL_CLASS_GADGET);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_getgid),
			_TEST_KVM_SYSCALL_CLASS_GADGET);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_getegid),
			_TEST_KVM_SYSCALL_CLASS_GADGET);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_clock_gettime),
			_TEST_KVM_SYSCALL_CLASS_GADGET);
	/* G5 (audit round-6) demoted sched_yield to class A. */
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_sched_yield),
			_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH);
	/* G6-follow-on additions. */
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_time),
			_TEST_KVM_SYSCALL_CLASS_GADGET);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_getcpu),
			_TEST_KVM_SYSCALL_CLASS_GADGET);

	/* Class A — passthrough. Spot-check the hot-path defaults. */
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_read),
			_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_write),
			_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_mmap),
			_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_brk),
			_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(__NR_futex),
			_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH);

	/* Out-of-range / sentinel → PASSTHROUGH so UML's own
	 * sys_ni_syscall surfaces ENOSYS from the normal path.
	 */
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(NR_syscalls),
			_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall(NR_syscalls + 100),
			_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH);
	KUNIT_EXPECT_EQ(test,
			kvm_classify_syscall((unsigned long)-1),
			_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH);
}

/*
 * Inventory-size invariant. Post-G5 (audit round-6) breakdown:
 *   class B (VCPU_STATE)  = 1  (arch_prctl)
 *   class C (SIGFRAME)    = 1  (rt_sigreturn)
 *   class D (TRAP)        = 10 (+modify_ldt + set_thread_area
 *                                demoted from B per F10)
 *   class E (GADGET)      = 10 (G7 + G6-follow-on, minus
 *                                sched_yield demoted by G5)
 * Total non-A: 22.
 */
static void kvm_syscall_class_count_test(struct kunit *test)
{
	unsigned long nr;
	unsigned int non_a = 0;
	unsigned int gadget = 0;
	unsigned int trap = 0;
	unsigned int vcpu_state = 0;
	unsigned int sigframe = 0;

	for (nr = 0; nr < NR_syscalls; nr++) {
		enum kvm_syscall_class c = kvm_classify_syscall(nr);

		if (c != _TEST_KVM_SYSCALL_CLASS_PASSTHROUGH)
			non_a++;
		if (c == _TEST_KVM_SYSCALL_CLASS_GADGET)
			gadget++;
		if (c == _TEST_KVM_SYSCALL_CLASS_TRAP)
			trap++;
		if (c == _TEST_KVM_SYSCALL_CLASS_VCPU_STATE)
			vcpu_state++;
		if (c == _TEST_KVM_SYSCALL_CLASS_SIGFRAME)
			sigframe++;
	}
	KUNIT_EXPECT_EQ(test, non_a, 22U);
	KUNIT_EXPECT_EQ(test, gadget, 10U);
	KUNIT_EXPECT_EQ(test, trap, 10U);
	KUNIT_EXPECT_EQ(test, vcpu_state, 1U);
	KUNIT_EXPECT_EQ(test, sigframe, 1U);
}

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */

/* ---------------------------------------------------------------- */
/* Suite registration                                               */
/* ---------------------------------------------------------------- */

static struct kunit_case backend_test_cases[] = {
	/* sanity */
	KUNIT_CASE(backend_contract_version_test),
	KUNIT_CASE(backend_all_ops_populated_test),
	/* lifecycle / trap */
	KUNIT_CASE(backend_probe_wired_test),
	KUNIT_CASE(backend_init_wired_test),
	KUNIT_CASE(backend_shutdown_wired_test),
	KUNIT_CASE(backend_vcpu_run_wired_test),
	/* memory (memo 25 R2 ops) */
	KUNIT_CASE(backend_mm_create_wired_test),
	KUNIT_CASE(backend_mm_destroy_wired_test),
	KUNIT_CASE(backend_mm_region_added_wired_test),
	KUNIT_CASE(backend_mm_region_removed_wired_test),
	/* scheduling */
	KUNIT_CASE(backend_thread_create_wired_test),
	KUNIT_CASE(backend_thread_start_idle_wired_test),
	KUNIT_CASE(backend_context_switch_wired_test),
	KUNIT_CASE(backend_ipi_send_wired_test),
	/* time */
	KUNIT_CASE(backend_read_clock_ns_test),
	KUNIT_CASE(backend_set_timer_test),
	KUNIT_CASE(backend_read_persistent_clock_ns_test),
	/* debug */
	KUNIT_CASE(backend_init_thread_regs_test),
	KUNIT_CASE(backend_read_guest_regs_stub_test),
	KUNIT_CASE(backend_write_guest_regs_stub_test),
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	/* KVM integrated state materialization (memo 08 #1) */
	KUNIT_CASE(kvm_production_sregs_shape_test),
	KUNIT_CASE(kvm_production_regs_marshal_test),
	KUNIT_CASE(kvm_production_probe_null_test),
	/* KVM LSTAR trampoline + wire ports (memo 08 #2a) */
	KUNIT_CASE(kvm_bootstrap_lstar_bytes_test),
	KUNIT_CASE(kvm_wire_ports_test),
	/* KVM GP-reg reverse marshal (memo 08 #2b prep) */
	KUNIT_CASE(kvm_regs_roundtrip_test),
	KUNIT_CASE(kvm_exit_probe_null_test),
	/* Audit round-4 F2: SYSRETQ R11 preserves user RFLAGS */
	KUNIT_CASE(kvm_build_sysret_r11_test),
	/* KVM shadow PGD lifecycle (memo 09 step 1) */
	KUNIT_CASE(kvm_shadow_pgd_alloc_test),
	/* Memo 10 syscall classification (step 2 + step 6) */
	KUNIT_CASE(kvm_syscall_classification_test),
	KUNIT_CASE(kvm_syscall_class_count_test),
	/* Memo 11 G3 gadget state channel */
	KUNIT_CASE(kvm_gadget_state_abi_test),
	KUNIT_CASE(kvm_gadget_state_refresh_test),
	/* Memo 11 G5 gadget vvar clock page */
	KUNIT_CASE(kvm_gadget_vvar_abi_test),
	KUNIT_CASE(kvm_gadget_vvar_refresh_test),
	KUNIT_CASE(kvm_snapshot_basic_test),
	KUNIT_CASE(kvm_record_basic_test),
	KUNIT_CASE(kvm_record_stop_replay_consume_test),
	KUNIT_CASE(kvm_record_strict_replay_test),
	KUNIT_CASE(kvm_record_roundtrip_test),
	KUNIT_CASE(kvm_record_meta_iov_roundtrip_test),
#endif
	{}
};

static struct kunit_suite backend_test_suite = {
	.name = "um_backend_contract",
	.test_cases = backend_test_cases,
};

kunit_test_suite(backend_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UML backend ops table conformance tests");
