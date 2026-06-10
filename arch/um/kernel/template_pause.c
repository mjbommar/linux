// SPDX-License-Identifier: GPL-2.0
/*
 * UML template-pause hook.
 *
 * A booted UML can stop at a named ready point, raise SIGSTOP in its
 * host process, and resume on SIGCONT after an external supervisor has
 * supplied an optional identity blob.
 *
 * In fork-on-resume mode the paused master forks a child for each take,
 * reports the child's host pid through the identity channel, and pauses
 * again for the next take.  This is separate from snapshot.c's AFL-style
 * forkserver path, where the parent owns the fork loop internally.
 */

#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <asm/um-template-pause.h>
#include <asm/um-snapshot.h>
#include "template_pause_identity.h"
#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK
#include <skas.h>
#include <skas/skas.h>
#include <os_io_ring.h>
#endif

#include <backend.h>

#include <os.h>

/*
 * Armed via "um_template_pause" on the kernel cmdline.  Off by
 * default: a non-pool boot must never accidentally raise SIGSTOP on
 * itself just because a stray /proc write lands.  The proc write
 * handler checks this flag and returns -ENODEV when unarmed.
 */
static bool template_pause_armed_flag __read_mostly;

/*
 * Fork-on-resume mode.  Off by default: a single SIGSTOP/SIGCONT cycle
 * resumes the same UML instance.  Armed by um_template_pause=fork;
 * each SIGCONT forks the master, the child wakes as the taken pool
 * member, and the master pauses again for the next take.
 *
 * Requires CONFIG_UM_TEMPLATE_PAUSE_FORK at build time (selects the
 * CoW physmem path + um_snapshot_worker_init).  Without that config,
 * =fork is downgraded to single-shot behaviour with a warning.
 */
static bool template_pause_fork_armed_flag __read_mostly;

/*
 * Early-pause mode.  Armed by um_template_pause=early on the
 * cmdline.  In this mode the UML process pauses itself via a late
 * initcall before run_init_process is reached: no guest userspace
 * has executed, no SKAS stub children exist, and the runqueue holds
 * only kernel-mode tasks.  This avoids inherited scheduler and SKAS
 * state hazards at subsequent ready points.
 *
 * Combinable with =fork: um_template_pause=early-fork arms both.
 */
static bool template_pause_early_armed_flag __read_mostly;

/*
 * Private-stack mode arm.  Default on in fork mode.  Can be disabled via
 * um_template_pause_private_stack=0 on the kernel cmdline to force
 * the raw __NR_fork + post-fork SIGKILL path.
 *
 * Mechanism: master uses __NR_clone rather than __NR_fork with a private
 * MAP_PRIVATE | MAP_ANONYMOUS stack for the child.  This avoids
 * MAP_SHARED physmem stack aliasing after fork.  The child's
 * first instruction post-clone runs on its own stack, so master's
 * concurrent writes to its stack don't corrupt the child's
 * view.  Child does inline __NR_exit_group(0) and terminates
 * cleanly without needing SIGKILL.
 */
static bool template_pause_private_stack_armed_flag __read_mostly = true;

/*
 * Pivot-test arm.  Off by default.  Armed by
 * um_template_pause_pivot_test=1 on the kernel cmdline.
 *
 * When set, master uses os_template_pause_fork_clone_to(@entry) and
 * the child runs child_entry_pivot_test on the private stack: a
 * kernel C function that writes a "PIVOT_OK\n" line via raw
 * __NR_write and exits cleanly.  Master skips the post-fork SIGKILL
 * because the child terminates itself.
 */
static bool template_pause_pivot_test_armed_flag __read_mostly;

/*
 * Pool-member arm.  Off by default.  Armed by
 * um_template_pause_pool_member=1 on the kernel cmdline.
 *
 * When set, master uses os_template_pause_fork_clone_to(@entry) and
 * the child runs child_entry_pool_member() on the private stack:
 * preempt_enable + host signal restore + SKAS stub respawn + drop
 * into userspace(), the canonical UML "resume the userspace task"
 * pattern.  The result: each post-fork child becomes a long-lived
 * UML kernel running the caller's userspace continuation.
 */
static bool template_pause_pool_member_armed_flag __read_mostly;

/*
 * Experimental pool-member physmem isolation.  Off by default.  When
 * enabled, the forked pool member switches to its own physmem backing
 * before it refreshes SKAS stubs and returns to userspace.
 */
static bool template_pause_pool_replicate_armed_flag __read_mostly;

/*
 * Per-host-process flag: set in child_entry_pool_member after the
 * fresh stub is up.  This child IS a pool member; subsequent
 * template_pause writes from its userspace (for example, a repeated
 * proc trigger) must not
 * recursively re-enter the fork-on-resume loop.  There is no
 * daemon to SIGCONT the child, so it would hang forever at
 * SIGSTOP.  Inherited via CoW (each child sets it independently
 * in its own page); master never sees it set.
 */
static bool template_pause_pool_member_active;

/*
 * Diagnostic only: counts how many SIGSTOP/SIGCONT cycles this
 * process (or any of its forked descendants that still share the
 * .data segment) has been through.  A fresh fork() inherits the
 * counter value of its parent; consumers reading
 * /sys/kernel/um/template_pause_count can use that to distinguish
 * "master that has paused once" from "child that has paused twice."
 */
static atomic_t template_pause_count = ATOMIC_INIT(0);

/*
 * Syscall return value restored in pool-member children.  The /proc
 * write path stores its byte count before entering the fork loop; each
 * child inherits the value via CoW and returns it to userspace.
 */
static long template_pause_resume_ret;

static int __init template_pause_setup(char *str)
{
	template_pause_armed_flag = true;
	if (str && (!strcmp(str, "=early") || !strcmp(str, "=early-fork"))) {
		template_pause_early_armed_flag = true;
		if (!strcmp(str, "=early-fork"))
			template_pause_fork_armed_flag = true;
		pr_info("template_pause: armed via kernel cmdline (early-pause%s)\n",
			template_pause_fork_armed_flag ? " + fork-on-resume" : "");
	} else if (str && !strcmp(str, "=fork")) {
		template_pause_fork_armed_flag = true;
		pr_warn("template_pause: fork-on-resume armed; keep parent seccomp quiescent\n");
	} else {
		pr_info("template_pause: armed via kernel cmdline\n");
	}
	return 1;
}
__setup("um_template_pause", template_pause_setup);

static int __init template_pause_private_stack_setup(char *str)
{
	if (str && str[0] == '=' && str[1] == '0')
		template_pause_private_stack_armed_flag = false;
	else if (str && str[0] == '=' && str[1] == '1')
		template_pause_private_stack_armed_flag = true;
	else if (str && str[0] == '\0')
		template_pause_private_stack_armed_flag = true;
	pr_info("template_pause: private-stack mode = %s\n",
		template_pause_private_stack_armed_flag ? "enabled" : "disabled (fork+SIGKILL)");
	return 1;
}
__setup("um_template_pause_private_stack", template_pause_private_stack_setup);

static int __init template_pause_pivot_test_setup(char *str)
{
	if (str && str[0] == '=' && str[1] == '1')
		template_pause_pivot_test_armed_flag = true;
	else if (str && str[0] == '\0')
		template_pause_pivot_test_armed_flag = true;
	if (template_pause_pivot_test_armed_flag)
		pr_warn("template_pause: pivot-test mode armed; M-fork child will run child_entry_pivot_test on a private stack\n");
	return 1;
}
__setup("um_template_pause_pivot_test", template_pause_pivot_test_setup);

static int __init template_pause_pool_member_setup(char *str)
{
	if (str && str[0] == '=' && str[1] == '1')
		template_pause_pool_member_armed_flag = true;
	else if (str && str[0] == '\0')
		template_pause_pool_member_armed_flag = true;
	if (template_pause_pool_member_armed_flag)
		pr_warn("template_pause: pool-member mode armed; M-fork child will enter userspace() as a long-lived pool member\n");
	return 1;
}
__setup("um_template_pause_pool_member", template_pause_pool_member_setup);

static int __init template_pause_pool_replicate_setup(char *str)
{
	if (str && str[0] == '=' && str[1] == '1')
		template_pause_pool_replicate_armed_flag = true;
	else if (str && str[0] == '\0')
		template_pause_pool_replicate_armed_flag = true;
	if (template_pause_pool_replicate_armed_flag)
		pr_warn("template_pause: experimental pool-member physmem replication armed\n");
	return 1;
}
__setup("um_template_pause_pool_replicate",
	template_pause_pool_replicate_setup);

/*
 * Child entry for pivot-test mode.  Runs on a MAP_PRIVATE stack
 * allocated by os_template_pause_fork_clone_to(), so it does not
 * share the master's kernel stack.
 *
 * Implementation: raw inline-asm only (no glibc, no kernel locks,
 * no printk). Writes a fixed status line to host fd 1 then exit_group(0).
 *
 * If "PIVOT_OK" appears in boot output, kernel C ran in the child
 * on the private stack.
 */
static void __noreturn
child_entry_pivot_test(void)
{
	static const char marker[] = "PIVOT_OK\n";

	register long rax asm("rax") = 1;       /* __NR_write */
	register long rdi asm("rdi") = 1;       /* host fd 1 */
	register long rsi asm("rsi") = (long)marker;
	register long rdx asm("rdx") = sizeof(marker) - 1;

	asm volatile ("syscall"
		      : "+r" (rax)
		      : "r" (rdi), "r" (rsi), "r" (rdx)
		      : "rcx", "r11", "memory");

	{
		register long erax asm("rax") = 231;    /* __NR_exit_group */
		register long erdi asm("rdi") = 0;

		asm volatile ("syscall"
			      :
			      : "r" (erax), "r" (erdi)
			      : "rcx", "r11", "memory");
	}
	__builtin_unreachable();
}

/*
 * Real pool-member child entry.  Runs on a MAP_PRIVATE 8 KiB stack
 * post-clone (allocated by os_template_pause_fork_clone_to()).
 *
 * UML resolves current via cpu_tasks[uml_curr_cpu()] (a per-CPU
 * table, see arch/um/include/asm/current.h), not thread_info-on-
 * stack.  So current still points at the task struct that called
 * write to /proc/um/template_pause, even from the
 * private stack.
 *
 * Sequence:
 *
 *   1. preempt_enable() - master held preempt_disable across fork.
 *   2. os_template_pause_signals_restore_host() - master blocked
 *      host signals pre-fork; child must unblock so timer ticks,
 *      SIGIO, SIGCHLD reach the seccomp dispatch loop.
 *   3. PT_REGS_SET_SYSCALL_RETURN(current regs, resume_ret) -
 *      the /proc write that triggered template_pause is "returning"
 *      with the byte count stored before fork.
 *   4. um_skas_respawn_all_stubs() - master tore the stubs down
 *      before fork in fork_on_resume_loop().  Child needs
 *      a fresh stub for its own userspace.
 *   5. userspace(&current->thread.regs.regs) - drop into the
 *      seccomp dispatch loop forever, pumping the caller's userspace.
 *      Never returns.
 */
static void template_pause_write_pool_enter_marker(void)
{
	static const char enter_msg[] = "POOL_ENTER\n";

	register long rax asm("rax") = 1;
	register long rdi asm("rdi") = 1;
	register long rsi asm("rsi") = (long)enter_msg;
	register long rdx asm("rdx") = sizeof(enter_msg) - 1;

	asm volatile ("syscall"
		      : "+r" (rax)
		      : "r" (rdi), "r" (rsi), "r" (rdx)
		      : "rcx", "r11", "memory");
}

static void template_pause_write_pool_replicate_marker(bool ok)
{
	static const char ok_msg[] = "POOL_REPLICATE_OK\n";
	static const char fail_msg[] = "POOL_REPLICATE_FAIL\n";
	const char *msg = ok ? ok_msg : fail_msg;
	size_t len = ok ? sizeof(ok_msg) - 1 : sizeof(fail_msg) - 1;

	register long rax asm("rax") = 1;
	register long rdi asm("rdi") = 1;
	register long rsi asm("rsi") = (long)msg;
	register long rdx asm("rdx") = len;

	asm volatile ("syscall"
		      : "+r" (rax)
		      : "r" (rdi), "r" (rsi), "r" (rdx)
		      : "rcx", "r11", "memory");
}

static void template_pause_rebuild_child_timer(void)
{
	/*
	 * Per timer(7), POSIX interval timers are not preserved across
	 * fork(2): the child must rebuild its own SIGALRM source.
	 * os_timer_worker_forget() zeroes the inherited timer_t handles
	 * (they would deliver to master's gettid() otherwise); the
	 * rebuild creates a fresh CLOCK_MONOTONIC timer targeting this
	 * thread's tid.  Without this, nanosleep() in the child never
	 * wakes up.
	 */
	os_timer_worker_forget();
	(void)os_timer_worker_rebuild();
	/*
	 * Arm a near-immediate one-shot so the kernel's tick path
	 * gets called and re-establishes its next_event tracking.
	 * Without this, the inherited clock_event_device state may
	 * not call set_next_event for the next hrtimer expiration.
	 */
	(void)os_timer_one_shot(0, 1000000ULL); /* 1ms */
}

static void template_pause_complete_child_proc_write(void)
{
	/*
	 * Complete the interrupted /proc write in the child.  AX was
	 * still -ENOSYS because the master never finished the syscall
	 * return path before forking.
	 */
	PT_REGS_SET_SYSCALL_RETURN(&current->thread.regs,
				   READ_ONCE(template_pause_resume_ret));
}

static void template_pause_reset_child_task_state(void)
{
	/*
	 * Restore the calling task's signal state to a clean RUNNING
	 * baseline.  At the time of the master's SIGSTOP, the task is
	 * mid-syscall (write to /proc/um/template_pause).
	 */
	current->flags &= ~(PF_EXITING | PF_POSTCOREDUMP | PF_SIGNALED);
	WRITE_ONCE(current->__state, TASK_RUNNING);
	current->exit_state = 0;
	current->exit_code = 0;
	atomic_set(&current->signal->live, 1);
	current->signal->group_exit_code = 0;
	current->signal->flags = 0;
	if (current->mm) {
		atomic_set(&current->mm->mm_users, 2);
		atomic_set(&current->mm->mm_count, 2);
	}
}

static void template_pause_refresh_child_stubs(void)
{
	/*
	 * Current pool members get fresh SKAS stub state below.  Full
	 * per-member physmem replacement is intentionally not performed
	 * here because remapping the process-wide physmem backing must
	 * preserve host signal delivery and kernel/stub VMA coherence.
	 * start_userspace_fresh() replaces id->stack with a memfd-backed
	 * stub_data page and clones a stub in this process's VM, avoiding
	 * parent/child aliasing.
	 */
	int dret = um_skas_disown_inherited();
	struct mm_id *id = current_mm_id();

	if (dret > 0 && id) {
		(void)start_userspace_fresh(id);
		if (current->mm)
			(void)um_skas_force_resync_mm(current->mm);
	}
}

static void template_pause_replicate_child_physmem(void)
{
	int ret;

	if (!template_pause_pool_replicate_armed_flag)
		return;

	ret = um_pool_replicate_physmem();
	if (ret) {
		template_pause_write_pool_replicate_marker(false);
		pr_err("template_pause: pool-member physmem replication failed: %d\n",
		       ret);
		os_template_pause_child_exit(100);
	}

	template_pause_write_pool_replicate_marker(true);
	pr_info("template_pause: pool-member physmem replicated\n");
}

static void __noreturn
child_entry_pool_member(void)
{
	template_pause_write_pool_enter_marker();

	preempt_enable();
	(void)os_template_pause_signals_restore_host();

	template_pause_rebuild_child_timer();
	template_pause_complete_child_proc_write();
	template_pause_reset_child_task_state();
	template_pause_replicate_child_physmem();
	template_pause_refresh_child_stubs();

	/*
	 * Mark this host process as a pool member so subsequent template_pause
	 * writes from its userspace are no-ops.
	 * Prevents recursive SIGSTOP-without-daemon hangs (still
	 * useful for the single-iteration path).
	 */
	template_pause_pool_member_active = true;

	userspace(&current->thread.regs.regs);
	__builtin_unreachable();
}

bool um_template_pause_armed(void)
{
	return template_pause_armed_flag;
}
EXPORT_SYMBOL_GPL(um_template_pause_armed);

/*
 * Read + validate the identity blob.  Returns 0 on a present + valid
 * blob (fields are NUL-terminated and logged), >0 if @identity_fd is
 * -1 (no channel, informational), or -errno on read / validation
 * failure.  @blob is zero-initialised on entry.
 */
static int read_identity_blob(int identity_fd,
			      const char *named_point,
			      struct um_template_identity *blob)
{
	ssize_t n;

	memset(blob, 0, sizeof(*blob));
	if (identity_fd < 0)
		return 1;

	n = os_template_pause_read_identity(identity_fd, blob, sizeof(*blob));
	if (n < 0) {
		pr_err("template_pause: identity read failed: %zd\n", n);
		return (int)n;
	}
	if ((size_t)n < sizeof(blob->magic) + sizeof(blob->version)) {
		pr_warn("template_pause: identity blob too short (%zd bytes); skipping\n",
			n);
		return -EINVAL;
	}
	if (blob->magic != UM_TEMPLATE_IDENTITY_MAGIC) {
		pr_err("template_pause: identity blob magic 0x%08x != expected 0x%08x\n",
		       blob->magic, UM_TEMPLATE_IDENTITY_MAGIC);
		return -EILSEQ;
	}
	if (blob->version != UM_TEMPLATE_IDENTITY_VERSION) {
		pr_err("template_pause: identity blob version %u not supported (expected %u)\n",
		       blob->version, UM_TEMPLATE_IDENTITY_VERSION);
		return -EPROTONOSUPPORT;
	}

	blob->instance_name[sizeof(blob->instance_name) - 1] = '\0';
	blob->tap_name[sizeof(blob->tap_name) - 1] = '\0';
	blob->ipv4_cidr[sizeof(blob->ipv4_cidr) - 1] = '\0';
	blob->ipv4_gateway[sizeof(blob->ipv4_gateway) - 1] = '\0';
	pr_info("template_pause: identity at \"%s\" instance=\"%s\" mac=%02x:%02x:%02x:%02x:%02x:%02x tap=\"%s\" ipv4=\"%s\" gw=\"%s\"\n",
		named_point,
		blob->instance_name,
		blob->mac_addr[0], blob->mac_addr[1], blob->mac_addr[2],
		blob->mac_addr[3], blob->mac_addr[4], blob->mac_addr[5],
		blob->tap_name, blob->ipv4_cidr, blob->ipv4_gateway);
	return 0;
}

/*
 * One SIGSTOP/SIGCONT cycle plus identity read.  Shared by the
 * single-shot path and each iteration of the fork-on-resume loop.
 */
static int one_pause_cycle(const char *named_point, int identity_fd,
			   struct um_template_identity *blob_out)
{
	int ret;

	/*
	 * Block host signals before SIGSTOP, not after SIGCONT.
	 *
	 * If signals are blocked only post-SIGCONT (the previous order),
	 * then queued signals (SIGALRM, SIGCHLD, SIGIO) accumulated
	 * during the stop window fire on master's kernel stack between
	 * SIGCONT and signal_block.  UML's hard_handler runs on the
	 * interrupted task's kernel stack, and the handler frame
	 * corrupts the saved return state at this function's caller's
	 * epilogue.
	 *
	 * Blocking before SIGSTOP closes that window: signals queue at
	 * the host kernel level but are not delivered until master
	 * explicitly unblocks (which it never does in the fork loop).
	 * SIGSTOP/SIGCONT are unmaskable so the stop/resume cycle still
	 * works.
	 *
	 * Idempotent on repeated calls (same rt_sigprocmask mask).
	 */
	(void)os_template_pause_signals_block_host();

	pr_info("template_pause: raising SIGSTOP at \"%s\" (pid=%d)\n",
		named_point, os_getpid());
	ret = os_template_pause_stop_self();
	if (ret) {
		pr_err("template_pause: SIGSTOP/SIGCONT roundtrip failed: %d\n",
		       ret);
		return ret;
	}
	atomic_inc(&template_pause_count);
	pr_info("template_pause: resumed via SIGCONT at \"%s\" (pid=%d, count=%d)\n",
		named_point, os_getpid(),
		atomic_read(&template_pause_count));
	(void)read_identity_blob(identity_fd, named_point, blob_out);
	return 0;
}

#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK

/*
 * Refuse fork-on-resume when the current UML state cannot be cloned
 * safely.  KVM is rejected because fork() aliases /dev/kvm and per-vCPU
 * mmap state into the child; each child would need its own KVM objects
 * created after the fork point.
 */
static int assert_fork_safety(const char *named_point)
{
	int violations = 0;

	/*
	 * These are the same invariants snapshot.c checks before a fork:
	 * the pre-fork hazard surface includes IRQ context, pending host
	 * signals, SMP state, and inherited signal/runqueue state.
	 *
	 * Use WARN_ONCE so each violation prints once with a stack
	 * trace, then return -EBUSY to refuse the fork.
	 */
	if (WARN_ONCE(in_hardirq() || in_softirq(),
		      "%s(\"%s\"): fork-on-resume entered from IRQ/softirq context\n",
		      __func__, named_point))
		violations++;

	if (WARN_ONCE(signal_pending(current),
		      "%s(\"%s\"): fork-on-resume entered with pending signals\n",
		      __func__, named_point))
		violations++;

	if (WARN_ONCE(num_online_cpus() > 1,
		      "%s(\"%s\"): fork-on-resume unsupported under SMP; park secondary vCPUs or build with NR_CPUS=1\n",
		      __func__, named_point))
		violations++;

	if (WARN_ONCE(um_get_signals() != 1,
		      "%s(\"%s\"): UML signals_enabled is %d at fork-on-resume entry; must be 1\n",
		      __func__, named_point, um_get_signals()))
		violations++;

	/*
	 * KVM-backend refusal.  fork() under KVM
	 * aliases /dev/kvm + per-vCPU mmap state into the child.
	 */
	if (um_backend && um_backend->kind == UM_BACKEND_KIND_KVM) {
		pr_err("template_pause: fork-on-resume unsupported under KVM backend at \"%s\"\n",
		       named_point);
		return -EOPNOTSUPP;
	}

	/*
	 * Refuse if any other mm has a stub parked in FUTEX_IN_KERN.
	 * Tearing it down would leave the task parked forever from the
	 * kernel's view, indistinguishable from a stub crash, and the
	 * existing mm_sigchld_irq path would fire fatal_sigsegv on it. The
	 * caller's own mm is exempt because it entered this path through
	 * proc_write.
	 */
	if (um_skas_other_mm_mid_syscall(current_mm_id())) {
		pr_err("template_pause: fork-on-resume refused at \"%s\"; another mm is mid-syscall\n",
		       named_point);
		return -EBUSY;
	}

	return violations ? -EBUSY : 0;
}

static void template_pause_apply_identity(const char *named_point,
					  const struct um_template_identity *blob)
{
	int aerr;

	/*
	 * Apply the just-read identity blob to the master's in-guest netdev.
	 * The forked child inherits the applied identity via CoW. Failures are
	 * logged and ignored: a bad blob must not abort the fork loop.
	 */
	if (blob->magic != UM_TEMPLATE_IDENTITY_MAGIC)
		return;

	aerr = um_template_identity_apply(blob);
	if (aerr)
		pr_warn("template_pause: identity apply at \"%s\" returned %d (continuing)\n",
			named_point, aerr);
}

static int template_pause_prepare_stubs_for_fork(void)
{
	int n;

	/*
	 * Pool-member mode keeps stubs alive across fork so the post-fork child
	 * inherits a working per-mm SKAS context.
	 */
	if (template_pause_pool_member_armed_flag) {
		pr_info("template_pause: skipping pre-fork stub teardown (pool_member mode keeps SKAS state)\n");
		return 0;
	}

	n = um_skas_teardown_all_stubs();
	if (n < 0) {
		pr_err("template_pause: stub teardown failed: %d\n", n);
		return n;
	}
	pr_info("template_pause: torn down %d stub(s) pre-fork\n", n);
	return 0;
}

static void template_pause_prepare_fork_state(void)
{
	int sret;

	/*
	 * Block all host-level signals via raw __NR_rt_sigprocmask before the
	 * fork.
	 */
	sret = os_template_pause_signals_block_host();
	pr_info("template_pause: host signal block ret=%d\n", sret);

	/*
	 * Detach all non-current tasks from the runqueue before fork. After
	 * fork, both halves have only current in their CFS runqueue, so
	 * schedule() cannot UML_LONGJMP into a CoW'd task with an invalid
	 * jmp_buf.
	 */
	preempt_disable();
	sched_worker_detach_other_tasks();
}

static int template_pause_fork_child(void)
{
	if (template_pause_pool_member_armed_flag)
		return os_template_pause_fork_clone_to(child_entry_pool_member);
	if (template_pause_pivot_test_armed_flag)
		return os_template_pause_fork_clone_to(child_entry_pivot_test);
	if (template_pause_private_stack_armed_flag)
		return os_template_pause_fork_clone();
	return os_template_pause_fork();
}

static int template_pause_handle_fork_failure(int child_pid)
{
	pr_err("template_pause: fork failed: %d\n", child_pid);
	preempt_enable();
	(void)os_template_pause_signals_restore_host();
	(void)um_skas_respawn_all_stubs();
	os_snapshot_unblock_iter_signals();
	return child_pid;
}

static void template_pause_kill_legacy_child(int child_pid)
{
	/*
	 * In fork+SIGKILL mode, kill the child before it returns to user mode
	 * with inherited master-only signal and stub state. Private-stack,
	 * pivot-test, and pool-member children do not use this path.
	 */
	if (template_pause_pool_member_armed_flag ||
	    template_pause_pivot_test_armed_flag ||
	    template_pause_private_stack_armed_flag)
		return;

	{
		register long rax_k asm("rax") = 62;	/* __NR_kill */
		register long rdi_k asm("rdi") = (long)child_pid;
		register long rsi_k asm("rsi") = 9;	/* SIGKILL */

		asm volatile("syscall\n\t"
			: "+r" (rax_k)
			: "r" (rdi_k), "r" (rsi_k)
			: "rcx", "r11", "memory");
	}
}

static void template_pause_report_child_pid(int identity_fd, int child_pid)
{
	size_t id_len = sizeof(struct um_template_identity);
	int werr;

	if (identity_fd < 0)
		return;

	werr = os_template_pause_write_child_pid(identity_fd, id_len,
						 child_pid);
	if (werr)
		pr_warn("template_pause: write child pid %d failed: %d\n",
			child_pid, werr);
}

/*
 * Fork-on-resume loop.  On each SIGCONT, the master forks a child and
 * pauses again for the next take.  The parent writes the new child's pid
 * to the identity memfd at offset sizeof(struct um_template_identity)
 * so the supervisor can read it back.
 *
 * Parent never re-enters UML kernel scheduling between forks.  The
 * loop is pure host-syscall work (kill + fork + write + kill).
 * UML's in-kernel signal dispatch is gated for the loop body via
 * os_snapshot_block_iter_signals().
 *
 * Returns from the CHILD with the taken identity in @first_blob (or
 * zero-initialised if the first take had no identity channel).
 * Never returns from the parent in the happy path; on error the
 * loop unwinds and the master process should be killed by the
 * supervisor.
 */
static int fork_on_resume_loop(const char *named_point, int identity_fd,
			       struct um_template_identity *first_blob)
{
	struct um_template_identity blob;
	int ret, child_pid;

	ret = assert_fork_safety(named_point);
	if (ret)
		return ret;

	/* The first pause already stored a blob in *first_blob.  Use it
	 * for the first fork, then loop with fresh reads for each
	 * subsequent take.
	 */
	blob = *first_blob;

	os_snapshot_block_iter_signals();

	for (;;) {
		template_pause_apply_identity(named_point, &blob);

		ret = template_pause_prepare_stubs_for_fork();
		if (ret) {
			os_snapshot_unblock_iter_signals();
			return ret;
		}

		template_pause_prepare_fork_state();
		child_pid = template_pause_fork_child();
		if (child_pid < 0)
			return template_pause_handle_fork_failure(child_pid);

		if (child_pid == 0) {
			/*
			 * Fork+SIGKILL child path.  Keep the child inert until
			 * the parent kills it; returning through normal UML
			 * cleanup here would traverse inherited runqueue and
			 * stub state that belongs to the master.
			 */
			for (;;)
				;
			return 0;
		}

		template_pause_kill_legacy_child(child_pid);
		preempt_enable();

		/*
		 * Master keeps host signals blocked across iterations.
		 * SIGSTOP/SIGCONT are excluded from the block so the wake-up
		 * still works. Other signals stay queued at host level until
		 * the master's lifecycle ends.
		 */

		template_pause_report_child_pid(identity_fd, child_pid);

		/* (E) Next take's pause. */
		ret = one_pause_cycle(named_point, identity_fd, &blob);
		if (ret) {
			os_snapshot_unblock_iter_signals();
			return ret;
		}
	}
}

#else /* !CONFIG_UM_TEMPLATE_PAUSE_FORK */

static inline int fork_on_resume_loop(const char *named_point, int identity_fd,
				      struct um_template_identity *first_blob)
{
	pr_warn("template_pause: =fork at \"%s\" needs CONFIG_UM_TEMPLATE_PAUSE_FORK; using single-shot\n",
		named_point);
	return 0;
}

#endif /* CONFIG_UM_TEMPLATE_PAUSE_FORK */

static int template_pause_enter(const char *named_point, long resume_ret)
{
	struct um_template_identity blob;
	int identity_fd, ret;

	if (!named_point)
		named_point = "(null)";

	if (!template_pause_armed_flag) {
		pr_info("template_pause: enter(\"%s\") refused; um_template_pause not on cmdline\n",
			named_point);
		return -ENODEV;
	}

	if (template_pause_pool_member_active) {
		pr_info("template_pause: enter(\"%s\") refused; already a pool member\n",
			named_point);
		return 0;
	}

	WRITE_ONCE(template_pause_resume_ret, resume_ret);

	identity_fd = os_template_pause_identity_fd();
	if (identity_fd < 0) {
		pr_info("template_pause: enter(\"%s\"); no UM_TEMPLATE_IDENTITY_FD; identity-blob channel disabled (err=%d)\n",
			named_point, identity_fd);
		identity_fd = -1;
	} else {
		pr_info("template_pause: enter(\"%s\"); identity_fd=%d\n",
			named_point, identity_fd);
	}

	ret = one_pause_cycle(named_point, identity_fd, &blob);
	if (ret)
		return ret;

	if (template_pause_fork_armed_flag)
		return fork_on_resume_loop(named_point, identity_fd, &blob);

	/*
	 * Single-shot path: the master is the taken instance.  Apply
	 * identity directly so the master boots as the configured pool
	 * member.  Failures are logged and ignored so a bad blob does
	 * not refuse the pause.
	 */
	if (blob.magic == UM_TEMPLATE_IDENTITY_MAGIC) {
		int aerr = um_template_identity_apply(&blob);

		if (aerr)
			pr_warn("template_pause: identity apply at \"%s\" returned %d (continuing)\n",
				named_point, aerr);
	}

	return 0;
}

int um_template_pause_enter(const char *named_point)
{
	return template_pause_enter(named_point, 0);
}
EXPORT_SYMBOL_GPL(um_template_pause_enter);

/*
 * /proc/um/template_pause - write-only trigger.  Writing any non-
 * empty string invokes um_template_pause_enter() with that string as
 * the named point.  The write() blocks across SIGSTOP/SIGCONT; the
 * caller's write(2) returns only after the supervisor sends SIGCONT.
 *
 * Keeping a writable /proc entry makes the path scriptable without a
 * supervisor-specific boot description.
 */
static ssize_t template_pause_proc_write(struct file *f,
					 const char __user *buf,
					 size_t count, loff_t *ppos)
{
	char name[32];
	size_t n;
	int ret;

	n = count < sizeof(name) - 1 ? count : sizeof(name) - 1;
	if (copy_from_user(name, buf, n))
		return -EFAULT;
	name[n] = '\0';
	if (n > 0 && name[n - 1] == '\n')
		name[n - 1] = '\0';

	ret = template_pause_enter(name[0] ? name : "proc", count);
	if (ret)
		return ret;
	return count;
}

static const struct proc_ops template_pause_proc_ops = {
	.proc_write = template_pause_proc_write,
};

static struct proc_dir_entry *um_proc_dir;

static int __init template_pause_proc_init(void)
{
	struct proc_dir_entry *ent;

	if (!template_pause_armed_flag) {
		/* Don't litter /proc when not in use. */
		return 0;
	}

	um_proc_dir = proc_mkdir("um", NULL);
	if (!um_proc_dir) {
		pr_err("template_pause: /proc/um mkdir failed\n");
		return -ENOMEM;
	}

	ent = proc_create("template_pause", 0200, um_proc_dir,
			  &template_pause_proc_ops);
	if (!ent) {
		pr_err("template_pause: /proc/um/template_pause create failed\n");
		proc_remove(um_proc_dir);
		um_proc_dir = NULL;
		return -ENOMEM;
	}

	pr_info("template_pause: /proc/um/template_pause ready\n");
	return 0;
}
late_initcall(template_pause_proc_init);

/*
 * Early-pause initcall: if armed via um_template_pause=early or
 * =early-fork, pause the kernel from this late_initcall, after
 * proc_init and before run_init_process spawns userspace or any SKAS
 * stub child is created.
 *
 * Returning 0 lets the boot proceed normally (init runs as usual) for
 * kernels where the early flag is not armed.
 */
static int __init template_pause_early_init(void)
{
	if (!template_pause_armed_flag || !template_pause_early_armed_flag)
		return 0;

	pr_info("template_pause: entering early-pause initcall\n");
	(void)um_template_pause_enter("early-initcall");
	pr_info("template_pause: early-pause initcall returned\n");
	return 0;
}
late_initcall_sync(template_pause_early_init);
