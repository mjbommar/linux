// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — snapshot KUnit suite (memo 26-snapshot
 * §Phase 2).
 *
 * Exercises the Phase 1 capture/restore primitives end-to-end
 * against a primed pool vCPU. The suite_init fixture drives the
 * lazy first-dispatch arming sequence (CPUID + CR4.OSXSAVE +
 * XCR0=FP|SSE|YMM) on vcpus[0] so KVM accepts the snapshot ioctls;
 * production paths reach the same state on first KVM_RUN, but no
 * user task has dispatched yet at KUnit run-time (do_basic_setup
 * boundary in init/main.c — see memo 26-snapshot §Phase 2).
 *
 * Test scope:
 *
 *   - test_kvm_v2_snapshot_basic: capture regs_only, scribble the
 *     vCPU's RAX via KVM_SET_REGS, restore_full, assert post-restore
 *     RAX matches the captured value. Confirms the regs-only round-
 *     trip wires the right ioctl sequence (SREGS before REGS, XCRS
 *     before XSAVE, MSR list length matches) without depending on
 *     the (Phase 3) memslot copy.
 *
 * Why a separate TU from test_byteshape.c: byte-shape tests are
 * pure data — no vCPU state, no /dev/kvm. The snapshot tests need
 * a live VM + vCPU pool, so they pay the suite_init fixture cost.
 * Splitting the suites keeps the byte-shape gate cheap (it runs
 * even on builds that don't init kvm-v2 successfully).
 */
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <os.h>

#include "kvm_v2_backend.h"

/*
 * Suite-scoped handle to the fixture vCPU. Populated by suite_init
 * (kvm_v2_snapshot_suite_init); test cases read it. Single-threaded
 * — KUnit serialises test execution within a suite, and we only
 * touch this from suite_init / individual test cases on the same
 * host thread, so no locking is needed.
 */
static struct kvm_v2_vcpu *kvm_v2_test_vcpu;

/**
 * kvm_v2_snapshot_suite_init - prime vcpus[0] for the snapshot tests.
 * @suite: KUnit suite handle (unused).
 *
 * Memo 26-snapshot §Phase 2: KUnit fires at do_basic_setup() after
 * all initcalls but before kernel_init execs userspace, so no UML
 * task has driven a vCPU through kvm_v2_vcpu_run()'s lazy
 * cpuid_primed block yet. The snapshot ioctls that constrain that
 * ordering (KVM_SET_XSAVE / KVM_SET_XCRS reject without the curated
 * CPUID + CR4.OSXSAVE live) would therefore fail at capture time.
 *
 * kvm_v2_vcpu_prime_for_kunit (defined in vcpu.c) extracts the same
 * arming sequence as vcpu_run's `if (!cpuid_primed)` block, so this
 * fixture matches the production path's outcome exactly — just
 * driven from the suite_init context instead of first KVM_RUN.
 *
 * Returns 0 to let the suite run, -ENODEV when no vCPU pool exists
 * (boot did not select kvm-v2). KUnit treats the negative return as
 * "skip the suite" — the snapshot cases get a SKIP verdict with the
 * pr_warn explaining why.
 */
static int kvm_v2_snapshot_suite_init(struct kunit_suite *suite)
{
	struct kvm_v2_vcpu *v;
	int rc;

	v = kvm_v2_vcpu_get(0);
	if (!v || v->vcpu_fd < 0) {
		pr_warn("um: kvm-v2 snapshot kunit: no vCPU pool at suite_init (boot not under backend=force=kvm-v2?)\n");
		return -ENODEV;
	}

	rc = kvm_v2_vcpu_prime_for_kunit(v);
	if (rc < 0) {
		pr_warn("um: kvm-v2 snapshot kunit: prime_for_kunit (cpu=%d) failed: %d\n",
			v->cpu, rc);
		return rc;
	}

	kvm_v2_test_vcpu = v;
	pr_info("um: kvm-v2 snapshot kunit: vcpus[0] primed (vcpu_fd=%d)\n",
		v->vcpu_fd);
	return 0;
}

/**
 * test_kvm_v2_snapshot_basic - regs-only snapshot round-trip.
 * @test: KUnit test handle.
 *
 * Captures the fixture vCPU's state via the Phase 1 regs-only path,
 * scribbles RAX via KVM_SET_REGS, restores via restore_full, then
 * KVM_GET_REGS to read back. The post-restore RAX must match the
 * captured value (memo 26-snapshot §5).
 *
 * The capture path internally exercises the kvm_v2_snapshot_pick_vcpu
 * helper, which prefers vcpus[].last_task == current; under KUnit
 * we run from a kthread that's never dispatched, so the helper
 * lands on the per-host-CPU fallback (smp_processor_id()). That's
 * vcpus[0] on the ncpus=1 boot the test runs under, which matches
 * the suite_init fixture's prime target.
 */
static void test_kvm_v2_snapshot_basic(struct kunit *test)
{
	struct kvm_v2_snapshot *snap;
	struct kvm_v2_vcpu *vcpu = kvm_v2_test_vcpu;
	struct kvm_regs scratch;
	int rc;

	KUNIT_ASSERT_NOT_NULL_MSG(test, vcpu,
				  "suite_init fixture did not populate kvm_v2_test_vcpu");

	snap = kvm_v2_snapshot_alloc();
	KUNIT_ASSERT_NOT_NULL(test, snap);

	rc = kvm_v2_snapshot_capture_regs_only(snap);
	KUNIT_ASSERT_EQ(test, rc, 0);

	/*
	 * Stomp on the vCPU's RAX so the restore has something to
	 * undo. The captured snapshot still holds the original RAX
	 * value; after restore_full the vCPU should match.
	 */
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&scratch);
	KUNIT_ASSERT_EQ(test, rc, 0);
	scratch.rax = 0xdeadbeefdeadbeefULL;
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_REGS,
			      (unsigned long)&scratch);
	KUNIT_ASSERT_EQ(test, rc, 0);

	rc = kvm_v2_snapshot_restore_full(snap);
	KUNIT_ASSERT_EQ(test, rc, 0);

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&scratch);
	KUNIT_ASSERT_EQ(test, rc, 0);
	KUNIT_EXPECT_EQ(test, scratch.rax, snap->regs.rax);

	kvm_v2_snapshot_destroy(snap);
}

static struct kunit_case kvm_v2_snapshot_test_cases[] = {
	KUNIT_CASE(test_kvm_v2_snapshot_basic),
	{}
};

static struct kunit_suite kvm_v2_snapshot_test_suite = {
	.name        = "kvm_v2_snapshot",
	.suite_init  = kvm_v2_snapshot_suite_init,
	.test_cases  = kvm_v2_snapshot_test_cases,
};

kunit_test_suite(kvm_v2_snapshot_test_suite);

MODULE_DESCRIPTION("UML kvm-v2 snapshot KUnit tests (memo 26-snapshot Phase 2)");
MODULE_LICENSE("GPL");
