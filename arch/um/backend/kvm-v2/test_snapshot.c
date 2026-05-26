// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — snapshot KUnit suite (memo 26-snapshot
 * §Phase 2 + §Phase 3).
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
 *   - test_kvm_v2_snapshot_full (Phase 3): capture_full, mutate the
 *     vCPU's RAX AND scribble a known marker into a freshly-allocated
 *     scratch page that sits inside the giant physmem memslot,
 *     restore_full_vcpu, assert both the RAX and the scratch-page
 *     contents are restored. Confirms the memslot capture/restore
 *     pair round-trips bytes through the kvmalloc'd backing buffer.
 *     The scratch page is __get_free_page-allocated from buddy so
 *     it's in physmem by construction, hitting the same memslot the
 *     IDT/GDT/IST/TSS/gadget-state pages live in.
 *
 * Why a separate TU from test_byteshape.c: byte-shape tests are
 * pure data — no vCPU state, no /dev/kvm. The snapshot tests need
 * a live VM + vCPU pool, so they pay the suite_init fixture cost.
 * Splitting the suites keeps the byte-shape gate cheap (it runs
 * even on builds that don't init kvm-v2 successfully).
 */
#include <kunit/test.h>
#include <linux/elf.h>
#include <linux/elfcore.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kvm.h>
#include <linux/mm.h>
#include <linux/mm_types.h>		/* EMPTY_VMA_FLAGS */
#include <linux/printk.h>
#include <linux/sched.h>		/* current */
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/user.h>			/* struct user_regs_struct */

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

	/*
	 * SMP-T79 (state-audit/28): use the explicit-vCPU capture
	 * variant so we exercise the same code path KUnit and
	 * production callers will share.  Pre-T79 this test used
	 * the implicit capture_regs_only() and relied on
	 * pick_vcpu's smp_processor_id() fallback — flaky under
	 * ncpus > 1 because the kthread could land on a non-0 CPU
	 * and pick a vCPU whose KVM state did not reflect
	 * suite_init's prime of vcpus[0].
	 */
	rc = kvm_v2_snapshot_capture_regs_only_for_vcpu(snap, vcpu);
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

	/*
	 * SMP-T79 (same rationale as the capture above): use the
	 * explicit-vCPU restore variant.  restore_full(snap) would
	 * defer to pick_vcpu and return -ENODEV under KUnit
	 * (kthread has no last_task match in the pool).
	 */
	rc = kvm_v2_snapshot_restore_full_vcpu(snap, vcpu);
	KUNIT_ASSERT_EQ(test, rc, 0);

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&scratch);
	KUNIT_ASSERT_EQ(test, rc, 0);
	KUNIT_EXPECT_EQ(test, scratch.rax, snap->regs.rax);

	kvm_v2_snapshot_destroy(snap);
}

/*
 * Phase 3 marker pattern: a recognisable 8-byte value the test
 * writes into a scratch page after capture so the restore can prove
 * it round-tripped through snap->memslots[i].data. Distinct from the
 * regs-only test's RAX scribble value (0xdeadbeefdeadbeefULL) so a
 * cross-test failure mode (e.g. wrong field zeroed) is easy to
 * distinguish in pr_warn output.
 */
#define KVM_V2_SNAPSHOT_TEST_MARKER	0xa5a5a5a5cafebabeULL
#define KVM_V2_SNAPSHOT_TEST_GARBAGE	0x0badf00d0badf00dULL

/*
 * Phase 3 test slot guest_phys_addr (memo 26-snapshot §Phase 3
 * notes): a GPA well above any realistic physmem_size (KVM accepts
 * up to host phys-bits, typically 48-52 bits) so the test slot
 * doesn't overlap the giant gpa=0..physmem_size physmem slot
 * installed by kvm_v2_physmem_memslot_install. 4 TiB is high enough
 * that no plausible UML config's mem= ever reaches it.
 *
 * Why a dedicated test slot rather than scribbling into the giant
 * physmem slot's bytes directly: the giant slot's size equals UML's
 * mem= setting, and capture_full's per-slot kvmalloc has to fit
 * another mem=N bytes alongside the running kernel. On a typical
 * UML config (mem=512M) that's infeasible (we have N bytes total
 * for kernel + buddy + capture buffer + everything). The Phase 3
 * memslot capture gracefully degrades on such slots (sets
 * snap->memslots[i].data = NULL with a pr_warn) but the round-trip
 * assertion can't be made against an un-captured slot. The
 * dedicated test slot, sized to a single page, always fits.
 *
 * COW / dirty-bitmap optimization for the giant slot is Phase 4
 * work (per the design memo §Phase 3 close-out / §Phase 4 open).
 */
#define KVM_V2_SNAPSHOT_TEST_SLOT_GPA	0x40000000000ULL	/* 4 TiB */

/**
 * test_kvm_v2_snapshot_full - full capture + memslot round-trip.
 * @test: KUnit test handle.
 *
 * Phase 3 acceptance gate (memo 26-snapshot §Phase 3):
 *
 *   1. Allocate a scratch page from buddy.
 *   2. Register a dedicated single-page memslot at a high GPA
 *      backed by the scratch page (see TEST_SLOT_GPA comment for
 *      why a separate slot rather than scribbling into physmem).
 *   3. Write a known marker into the scratch page.
 *   4. capture_full against vcpus[0].
 *   5. Overwrite the scratch page with garbage AND scribble the
 *      vCPU's RAX with garbage.
 *   6. restore_full_vcpu against vcpus[0].
 *   7. Assert the scratch page contains the marker (proves the
 *      memslot copy round-tripped) AND the vCPU RAX matches the
 *      captured value (proves the regs path still works).
 *   8. Tear down the test slot.
 *
 * Why a scratch page rather than scribbling into the IDT / GDT
 * pages directly: those pages are live state the rest of the
 * kernel may walk concurrently (irq vectoring, etc.). A fresh
 * __get_free_page allocation is dedicated to this test and is
 * safe to mutate without coordinating with other subsystems.
 */
static void test_kvm_v2_snapshot_full(struct kunit *test)
{
	struct kvm_v2_snapshot *snap;
	struct kvm_v2_vcpu *vcpu = kvm_v2_test_vcpu;
	struct kvm_v2_vm *vm;
	struct kvm_userspace_memory_region kr;
	struct kvm_regs scratch;
	unsigned long scratch_page;
	u64 *scratch_marker;
	int test_slot_id = -1;
	int memslot_data_count;
	int i;
	int rc;

	KUNIT_ASSERT_NOT_NULL_MSG(test, vcpu,
				  "suite_init fixture did not populate kvm_v2_test_vcpu");

	vm = kvm_v2_vm_get();
	KUNIT_ASSERT_NOT_NULL_MSG(test, vm,
				  "kvm_v2_vm_get returned NULL (VM not initialised?)");

	/*
	 * Allocate the scratch page from buddy. __GFP_ZERO so the
	 * post-restore comparison doesn't trip on uninit poison.
	 */
	scratch_page = __get_free_page(GFP_KERNEL | __GFP_ZERO);
	KUNIT_ASSERT_NE_MSG(test, scratch_page, 0UL,
			    "scratch page __get_free_page failed");
	scratch_marker = (u64 *)scratch_page;

	/*
	 * Register the per-test memslot. memslot_add tracks the
	 * in-memory entry; the KVM_SET_USER_MEMORY_REGION ioctl makes
	 * KVM aware of it. The order here mirrors
	 * kvm_v2_physmem_memslot_install (context.c) — first allocate
	 * the slot id + list entry, then push to KVM, undo on failure.
	 */
	test_slot_id = kvm_v2_memslot_add(vm,
					  KVM_V2_SNAPSHOT_TEST_SLOT_GPA,
					  scratch_page,
					  PAGE_SIZE,
					  0 /* flags */);
	if (test_slot_id < 0) {
		free_page(scratch_page);
		KUNIT_FAIL(test, "kvm_v2_memslot_add rc=%d", test_slot_id);
		return;
	}
	kr = (struct kvm_userspace_memory_region){
		.slot		 = (u32)test_slot_id,
		.flags		 = 0,
		.guest_phys_addr = KVM_V2_SNAPSHOT_TEST_SLOT_GPA,
		.memory_size	 = PAGE_SIZE,
		.userspace_addr	 = scratch_page,
	};
	rc = os_ioctl_generic(vm->vm_fd, KVM_SET_USER_MEMORY_REGION,
			      (unsigned long)&kr);
	if (rc < 0) {
		kvm_v2_memslot_del(vm, (u32)test_slot_id);
		free_page(scratch_page);
		KUNIT_FAIL(test, "KVM_SET_USER_MEMORY_REGION rc=%d", rc);
		return;
	}

	*scratch_marker = KVM_V2_SNAPSHOT_TEST_MARKER;

	snap = kvm_v2_snapshot_alloc();
	if (!snap) {
		kr.memory_size = 0;
		os_ioctl_generic(vm->vm_fd, KVM_SET_USER_MEMORY_REGION,
				 (unsigned long)&kr);
		kvm_v2_memslot_del(vm, (u32)test_slot_id);
		free_page(scratch_page);
		KUNIT_FAIL(test, "kvm_v2_snapshot_alloc returned NULL");
		return;
	}

	rc = kvm_v2_snapshot_capture_full(snap, vcpu);
	KUNIT_ASSERT_EQ_MSG(test, rc, 0,
			    "capture_full rc=%d", rc);
	KUNIT_ASSERT_GT_MSG(test, snap->memslot_count, 0,
			    "capture_full saw zero memslots (test slot registration didn't take?)");

	/*
	 * Count slots whose .data buffer actually got allocated.
	 * Under memory pressure the giant physmem slot may have
	 * gracefully degraded to metadata-only (data=NULL); the
	 * test slot's PAGE_SIZE copy always fits, so at least one
	 * data buffer must be live.
	 */
	memslot_data_count = 0;
	for (i = 0; i < snap->memslot_count; i++)
		if (snap->memslots[i].data)
			memslot_data_count++;
	KUNIT_ASSERT_GT_MSG(test, memslot_data_count, 0,
			    "capture_full produced zero data-bearing slots");

	/*
	 * Mutate both the in-guest scratch page and the vCPU's RAX so
	 * the restore has two distinct things to undo.
	 */
	*scratch_marker = KVM_V2_SNAPSHOT_TEST_GARBAGE;

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&scratch);
	KUNIT_ASSERT_EQ(test, rc, 0);
	scratch.rax = 0xdeadbeefdeadbeefULL;
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_REGS,
			      (unsigned long)&scratch);
	KUNIT_ASSERT_EQ(test, rc, 0);

	rc = kvm_v2_snapshot_restore_full_vcpu(snap, vcpu);
	KUNIT_ASSERT_EQ_MSG(test, rc, 0,
			    "restore_full_vcpu rc=%d", rc);

	/* Memslot round-trip — the marker must have come back. */
	KUNIT_EXPECT_EQ_MSG(test, *scratch_marker,
			    (u64)KVM_V2_SNAPSHOT_TEST_MARKER,
			    "scratch page not restored: got %#llx expected %#llx",
			    (u64)*scratch_marker,
			    (u64)KVM_V2_SNAPSHOT_TEST_MARKER);

	/* vCPU regs round-trip — same property as the basic test. */
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&scratch);
	KUNIT_ASSERT_EQ(test, rc, 0);
	KUNIT_EXPECT_EQ(test, scratch.rax, snap->regs.rax);

	kvm_v2_snapshot_destroy(snap);

	/*
	 * Tear down the test memslot: KVM_SET_USER_MEMORY_REGION with
	 * memory_size=0 is KVM's "delete this slot" syntax; then drop
	 * the kvm_v2_memslot list entry and free the scratch page.
	 */
	kr.memory_size = 0;
	rc = os_ioctl_generic(vm->vm_fd, KVM_SET_USER_MEMORY_REGION,
			      (unsigned long)&kr);
	if (rc < 0)
		pr_warn("um: kvm-v2 snapshot kunit: test slot teardown rc=%d (continuing)\n",
			rc);
	kvm_v2_memslot_del(vm, (u32)test_slot_id);
	free_page(scratch_page);
}

/**
 * test_kvm_v2_snapshot_task - Phase 4 cross-task semantics round-trip.
 * @test: KUnit test handle.
 *
 * Phase 4 acceptance gate (memo 26-snapshot §Phase 4):
 *
 *   1. Stage a recognisable pattern into current->thread.arch.kvm_v2.
 *      iotrap_fpu and current->thread.arch.kvm_v2.iotrap_events, and
 *      set their _valid flags.
 *   2. capture_task against vcpus[0].
 *   3. Stomp current's iotrap_* fields with a contrasting pattern
 *      (and clear the _valid flags) so the restore has something to
 *      undo.
 *   4. restore_task against vcpus[0].
 *   5. Assert the original pattern is back AND the _valid flags are
 *      restored to true. Confirms the task_iotrap_* fields round-trip
 *      independently of the vCPU's KVM_GET_XSAVE view.
 *   6. Snapshot from a fresh _alloc and try restore_task: must reject
 *      with -EINVAL because task_state_captured is false. Confirms
 *      the gate prevents accidentally zeroing current's state from a
 *      snapshot that never captured it.
 *
 * Why this test matters: under the per-host-CPU vCPU pool, the
 * snapshot's @xsave reflects whichever task last ran on the vCPU,
 * NOT the calling task's saved iotrap_fpu. Without Phase 4,
 * record/replay (#169) would lose the calling task's per-task FPU
 * state across a capture/restore cycle (since the calling task may
 * have been switched out and its FPU saved to iotrap_fpu rather
 * than living in the vCPU).
 */
static void test_kvm_v2_snapshot_task(struct kunit *test)
{
#ifdef CONFIG_UM_BACKEND_KVM_V2
	struct kvm_v2_snapshot *snap;
	struct kvm_v2_vcpu *vcpu = kvm_v2_test_vcpu;
	/*
	 * kvm_xsave is 4 KB; combined with the snapshot allocation
	 * already in scope the kernel stack would blow Wframe-larger-
	 * than= (memo 26-snapshot §Phase 3 frame-size lesson). Allocate
	 * the entry-state save buffer on the heap so we stay under 1024 B.
	 */
	struct kvm_xsave *saved_fpu;
	struct kvm_vcpu_events saved_events;
	bool saved_fpu_valid;
	bool saved_events_valid;
	const u8 marker_fpu_byte    = 0xa5;
	const u8 garbage_fpu_byte   = 0x5a;
	int rc;

	KUNIT_ASSERT_NOT_NULL_MSG(test, vcpu,
				  "suite_init fixture did not populate kvm_v2_test_vcpu");

	saved_fpu = kzalloc(sizeof(*saved_fpu), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, saved_fpu);

	/* Save current's existing iotrap state so we can restore it
	 * unmodified at test exit (KUnit test should be transparent).
	 */
	*saved_fpu         = current->thread.arch.kvm_v2.iotrap_fpu;
	saved_events       = current->thread.arch.kvm_v2.iotrap_events;
	saved_fpu_valid    = current->thread.arch.kvm_v2.iotrap_fpu_valid;
	saved_events_valid = current->thread.arch.kvm_v2.iotrap_events_valid;

	/* Stage the marker pattern. */
	memset(&current->thread.arch.kvm_v2.iotrap_fpu, marker_fpu_byte,
	       sizeof(current->thread.arch.kvm_v2.iotrap_fpu));
	current->thread.arch.kvm_v2.iotrap_fpu_valid = true;
	memset(&current->thread.arch.kvm_v2.iotrap_events, marker_fpu_byte,
	       sizeof(current->thread.arch.kvm_v2.iotrap_events));
	current->thread.arch.kvm_v2.iotrap_events_valid = true;

	snap = kvm_v2_snapshot_alloc();
	KUNIT_ASSERT_NOT_NULL(test, snap);

	rc = kvm_v2_snapshot_capture_task(snap, vcpu);
	KUNIT_ASSERT_EQ_MSG(test, rc, 0, "capture_task rc=%d", rc);
	KUNIT_EXPECT_TRUE(test, snap->task_state_captured);
	KUNIT_EXPECT_TRUE(test, snap->task_iotrap_fpu_valid);
	KUNIT_EXPECT_TRUE(test, snap->task_iotrap_events_valid);
	KUNIT_EXPECT_EQ(test, snap->task_source_pid, current->pid);
	KUNIT_EXPECT_EQ(test,
			((u8 *)&snap->task_iotrap_fpu)[0],
			marker_fpu_byte);

	/* Stomp current's fields with a contrasting pattern. */
	memset(&current->thread.arch.kvm_v2.iotrap_fpu, garbage_fpu_byte,
	       sizeof(current->thread.arch.kvm_v2.iotrap_fpu));
	current->thread.arch.kvm_v2.iotrap_fpu_valid = false;
	memset(&current->thread.arch.kvm_v2.iotrap_events, garbage_fpu_byte,
	       sizeof(current->thread.arch.kvm_v2.iotrap_events));
	current->thread.arch.kvm_v2.iotrap_events_valid = false;

	rc = kvm_v2_snapshot_restore_task(snap, vcpu);
	KUNIT_ASSERT_EQ_MSG(test, rc, 0, "restore_task rc=%d", rc);

	/* The marker must have come back. */
	KUNIT_EXPECT_TRUE(test,
		current->thread.arch.kvm_v2.iotrap_fpu_valid);
	KUNIT_EXPECT_TRUE(test,
		current->thread.arch.kvm_v2.iotrap_events_valid);
	KUNIT_EXPECT_EQ(test,
		((u8 *)&current->thread.arch.kvm_v2.iotrap_fpu)[0],
		marker_fpu_byte);
	KUNIT_EXPECT_EQ(test,
		((u8 *)&current->thread.arch.kvm_v2.iotrap_events)[0],
		marker_fpu_byte);

	kvm_v2_snapshot_destroy(snap);

	/*
	 * Gate test: a fresh snapshot has task_state_captured=false;
	 * restore_task must reject it rather than zero current's state.
	 */
	snap = kvm_v2_snapshot_alloc();
	KUNIT_ASSERT_NOT_NULL(test, snap);
	KUNIT_EXPECT_FALSE(test, snap->task_state_captured);
	rc = kvm_v2_snapshot_restore_task(snap, vcpu);
	KUNIT_EXPECT_EQ_MSG(test, rc, -EINVAL,
		"restore_task on un-captured snapshot returned %d (want -EINVAL)",
		rc);
	kvm_v2_snapshot_destroy(snap);

	/* Restore the test-entry state (be a good KUnit citizen). */
	current->thread.arch.kvm_v2.iotrap_fpu          = *saved_fpu;
	current->thread.arch.kvm_v2.iotrap_events       = saved_events;
	current->thread.arch.kvm_v2.iotrap_fpu_valid    = saved_fpu_valid;
	current->thread.arch.kvm_v2.iotrap_events_valid = saved_events_valid;
	kfree(saved_fpu);
#else
	kunit_skip(test, "CONFIG_UM_BACKEND_KVM_V2 not enabled");
#endif
}

/*
 * Helper: count memslots whose data buffer was allocated. Used by
 * test_kvm_v2_snapshot_elf_basic to match its assertion against the
 * same count the ELF writer used. Declared here rather than as a
 * static in snapshot_elf.c so the test TU can read the count without
 * adding an exported symbol whose only consumer is a test.
 */
static int kvm_v2_snapshot_elf_count_load_test_helper(const struct kvm_v2_snapshot *snap)
{
	int i, n = 0;

	if (!snap || !snap->memslots)
		return 0;
	for (i = 0; i < snap->memslot_count; i++)
		if (snap->memslots[i].data && snap->memslots[i].data_size > 0)
			n++;
	return n;
}

/**
 * test_kvm_v2_snapshot_elf_basic - ELF64-core export round-trip (#181).
 * @test: KUnit test handle.
 *
 * #181 acceptance gate. Captures a regs-only snapshot, writes a known
 * marker into the vCPU's RAX so the captured snap->regs.rax has a
 * predictable value, exports the snapshot to an in-kernel tmpfs file
 * (shmem_kernel_file_setup — no userspace fd dance required), reads
 * the file back into memory, and parses the ELF header to verify:
 *
 *   - ELF magic + ELFCLASS64 + ELFDATA2LSB + e_type == ET_CORE.
 *   - At least one PT_NOTE phdr is present.
 *   - PT_LOAD count matches the count of data-bearing memslots
 *     (zero for a regs-only snapshot per Phase 1's contract).
 *   - The NT_PRSTATUS note exists and its pr_reg.rax field equals
 *     the captured snap->regs.rax (i.e. the marker we wrote).
 *
 * Why shmem rather than vfs_truncate(memfd_create()): memfd_create is
 * a userspace-only syscall (no in-kernel symbol export);
 * shmem_kernel_file_setup is the documented "give me a tmpfs file
 * handle from kernel context" API used by drivers/gpu/drm/i915 and
 * fs/ipc/shm.c. The returned struct file * can be passed straight to
 * kvm_v2_snapshot_elf_export_to_file.
 */
static void test_kvm_v2_snapshot_elf_basic(struct kunit *test)
{
	struct kvm_v2_snapshot *snap;
	struct kvm_v2_vcpu *vcpu = kvm_v2_test_vcpu;
	struct kvm_regs scratch;
	struct file *f;
	void *buf;
	size_t buf_size;
	loff_t pos;
	struct elf64_hdr *ehdr;
	struct elf64_phdr *phdrs;
	int pt_note_count = 0;
	int pt_load_count = 0;
	bool saw_nt_prstatus = false;
	u64 prstatus_rax = 0;
	int i;
	int rc;

	KUNIT_ASSERT_NOT_NULL_MSG(test, vcpu,
				  "suite_init fixture did not populate kvm_v2_test_vcpu");

	/*
	 * Stamp a recognisable RAX value so the captured snapshot has a
	 * known marker we can verify after the ELF round-trip.
	 */
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&scratch);
	KUNIT_ASSERT_EQ(test, rc, 0);
	scratch.rax = 0x18112026ULL;	/* #181 / 2026 — a memorable marker. */
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_REGS,
			      (unsigned long)&scratch);
	KUNIT_ASSERT_EQ(test, rc, 0);

	snap = kvm_v2_snapshot_alloc();
	KUNIT_ASSERT_NOT_NULL(test, snap);

	rc = kvm_v2_snapshot_capture_regs_only_for_vcpu(snap, vcpu);
	KUNIT_ASSERT_EQ(test, rc, 0);
	KUNIT_ASSERT_EQ_MSG(test, snap->regs.rax, 0x18112026ULL,
			    "captured RAX (%#llx) does not match the marker (%#llx)",
			    snap->regs.rax, 0x18112026ULL);

	/*
	 * Open a kernel-only tmpfs file (anonymous shmem). 0 size means
	 * "grow as we write."
	 */
	f = shmem_kernel_file_setup("kvm-v2-snap.elf", 0, EMPTY_VMA_FLAGS);
	if (IS_ERR(f)) {
		kvm_v2_snapshot_destroy(snap);
		KUNIT_FAIL(test, "shmem_kernel_file_setup rc=%ld",
			   PTR_ERR(f));
		return;
	}

	rc = kvm_v2_snapshot_elf_export_to_file(snap, f);
	KUNIT_ASSERT_EQ_MSG(test, rc, 0, "export_to_file rc=%d", rc);

	/*
	 * Read the file back into a heap buffer. snap->regs+sregs+
	 * notes+headers fits well under 16 KB for a regs-only export.
	 */
	buf_size = 16384;
	buf = kvzalloc(buf_size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	pos = 0;
	rc = kernel_read(f, buf, buf_size, &pos);
	KUNIT_ASSERT_GT_MSG(test, rc, (int)sizeof(struct elf64_hdr),
			    "kernel_read rc=%d (need at least Ehdr)", rc);

	ehdr = (struct elf64_hdr *)buf;
	KUNIT_ASSERT_EQ(test, ehdr->e_ident[EI_MAG0], ELFMAG0);
	KUNIT_ASSERT_EQ(test, ehdr->e_ident[EI_MAG1], ELFMAG1);
	KUNIT_ASSERT_EQ(test, ehdr->e_ident[EI_MAG2], ELFMAG2);
	KUNIT_ASSERT_EQ(test, ehdr->e_ident[EI_MAG3], ELFMAG3);
	KUNIT_EXPECT_EQ(test, ehdr->e_ident[EI_CLASS], ELFCLASS64);
	KUNIT_EXPECT_EQ(test, ehdr->e_ident[EI_DATA], ELFDATA2LSB);
	KUNIT_EXPECT_EQ(test, ehdr->e_type, ET_CORE);
	KUNIT_EXPECT_EQ(test, ehdr->e_machine, EM_X86_64);

	/*
	 * Walk the phdr table — PT_NOTE / PT_LOAD count must match the
	 * snapshot's memslot inventory (regs-only → 0 PT_LOAD, 1 PT_NOTE).
	 */
	phdrs = (struct elf64_phdr *)((u8 *)buf + ehdr->e_phoff);
	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type == PT_NOTE)
			pt_note_count++;
		else if (phdrs[i].p_type == PT_LOAD)
			pt_load_count++;
	}
	KUNIT_EXPECT_GT(test, pt_note_count, 0);
	KUNIT_EXPECT_EQ_MSG(test, pt_load_count,
			    kvm_v2_snapshot_elf_count_load_test_helper(snap),
			    "PT_LOAD count does not match data-bearing memslot count");

	/*
	 * Scan the notes for NT_PRSTATUS and pull RAX out. The Nhdr
	 * stream lives at phdrs[note].p_offset for whatever length
	 * .p_filesz says.
	 */
	for (i = 0; i < ehdr->e_phnum; i++) {
		u8 *note;
		u32 note_end;
		u32 cur;

		if (phdrs[i].p_type != PT_NOTE)
			continue;
		if (phdrs[i].p_offset + phdrs[i].p_filesz > buf_size)
			continue;	/* Truncated; can't safely walk. */
		note = (u8 *)buf + phdrs[i].p_offset;
		note_end = phdrs[i].p_filesz;
		cur = 0;
		while (cur + sizeof(struct elf64_note) <= note_end) {
			struct elf64_note *nh =
				(struct elf64_note *)(note + cur);
			u32 namesz = nh->n_namesz;
			u32 descsz = nh->n_descsz;
			u32 paystart;

			cur += sizeof(*nh);
			paystart = cur + ((namesz + 3) & ~3u);
			if (paystart + descsz > note_end)
				break;
			if (nh->n_type == NT_PRSTATUS &&
			    descsz >= sizeof(struct elf_prstatus)) {
				struct elf_prstatus *ps =
					(struct elf_prstatus *)(note + paystart);
				/*
				 * pr_reg layout (x86_64 user_regs_struct):
				 * ax is the 11th unsigned long (offset 80).
				 * See arch/x86/include/asm/user_64.h.
				 */
				struct user_regs_struct *ur =
					(struct user_regs_struct *)&ps->pr_reg;

				saw_nt_prstatus = true;
				prstatus_rax = ur->ax;
				break;
			}
			cur = paystart + ((descsz + 3) & ~3u);
		}
		if (saw_nt_prstatus)
			break;
	}

	KUNIT_EXPECT_TRUE_MSG(test, saw_nt_prstatus,
			      "NT_PRSTATUS not found in PT_NOTE");
	KUNIT_EXPECT_EQ_MSG(test, prstatus_rax, 0x18112026ULL,
			    "NT_PRSTATUS pr_reg.ax (%#llx) does not match snap->regs.rax (%#llx)",
			    prstatus_rax, snap->regs.rax);

	kvfree(buf);
	fput(f);
	kvm_v2_snapshot_destroy(snap);
}

static struct kunit_case kvm_v2_snapshot_test_cases[] = {
	KUNIT_CASE(test_kvm_v2_snapshot_basic),
	KUNIT_CASE(test_kvm_v2_snapshot_full),
	KUNIT_CASE(test_kvm_v2_snapshot_task),
	KUNIT_CASE(test_kvm_v2_snapshot_elf_basic),
	{}
};

static struct kunit_suite kvm_v2_snapshot_test_suite = {
	.name        = "kvm_v2_snapshot",
	.suite_init  = kvm_v2_snapshot_suite_init,
	.test_cases  = kvm_v2_snapshot_test_cases,
};

kunit_test_suite(kvm_v2_snapshot_test_suite);

MODULE_DESCRIPTION("UML kvm-v2 snapshot KUnit tests (memo 26-snapshot Phases 2-3)");
MODULE_LICENSE("GPL");
