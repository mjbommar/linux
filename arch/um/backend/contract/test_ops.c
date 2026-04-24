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
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/cpu.h>
#include <linux/string.h>
#include <asm/backend.h>

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
# include <linux/kvm.h>
# include <sysdep/ptrace.h>
/*
 * kvm_enter_guest_probe lives in arch/um/backend/kvm/thread.c
 * (EXPORT_SYMBOL_GPL'd there). Prototype-local-to-test avoids
 * pulling `arch/um/backend/kvm/kvm_backend.h` in via a fragile
 * cross-subdir relative include; the probe signature is the
 * stable contract, identical to what kvm_backend.h declares.
 */
int kvm_enter_guest_probe(struct kvm_sregs *sregs, struct kvm_regs *regs,
			  const struct uml_pt_regs *src,
			  u64 cr3_gpa, u64 gdt_gpa);
#endif

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
	KUNIT_EXPECT_NOT_NULL(test, um_backend->run_userspace);

	/* Memory (4) */
	KUNIT_EXPECT_NOT_NULL(test, um_backend->mm_attach);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->mm_detach);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->mm_map);
	KUNIT_EXPECT_NOT_NULL(test, um_backend->mm_unmap);

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

static void backend_run_userspace_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, run_userspace);
}

/* ---------------------------------------------------------------- */
/* Memory                                                           */
/* ---------------------------------------------------------------- */

static void backend_mm_attach_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, mm_attach);
}

static void backend_mm_detach_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, mm_detach);
}

static void backend_mm_map_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, mm_map);
}

static void backend_mm_unmap_wired_test(struct kunit *test)
{
	ASSERT_OP_DISPATCH(test, mm_unmap);
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
	KUNIT_CASE(backend_run_userspace_wired_test),
	/* memory */
	KUNIT_CASE(backend_mm_attach_wired_test),
	KUNIT_CASE(backend_mm_detach_wired_test),
	KUNIT_CASE(backend_mm_map_wired_test),
	KUNIT_CASE(backend_mm_unmap_wired_test),
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
