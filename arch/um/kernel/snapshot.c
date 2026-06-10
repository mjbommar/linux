// SPDX-License-Identifier: GPL-2.0
/*
 * UML snapshot / forkserver interface.
 *
 * This translation unit provides the in-kernel coordination point that
 * lets a UML guest reach a named "ready point," quiesce to a barrier,
 * and fork() itself for each fuzz iteration. The AFL-compatible
 * 12-byte-per-iteration wire protocol on fds 198/199 lives here too;
 * it binds directly onto syzkaller's Instance.RunSnapshot API so
 * the fuzz profile does not need its own per-iteration negotiation.
 *
 * The ready point asserts that the current UML state can be forked,
 * enables the snapshot static key only while the forkserver is active,
 * and runs the AFL-compatible protocol until the fuzzer disconnects.
 */

#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#include <asm/um-mmaps.h>
#include <asm/um-snapshot.h>

#include <shared/backend.h>

#include <os.h>

/* AFL forkserver protocol: caller opens host fds 198 (fuzzer to server)
 * and 199 (server to fuzzer) before exec'ing UML.
 */
#define UM_FORKSERVER_CTL_FD	198
#define UM_FORKSERVER_STATUS_FD	199

/* 4-byte handshake the parent sends on status fd right after quiesce.
 * AFL++ accepts any value whose top 24 bits match 'A','F','L' and
 * uses the bottom 8 bits for protocol-version flags. We send the
 * classic zero value.
 */
static const u8 um_forkserver_hello[4] = { 'A', 'F', 'L', 0 };

/*
 * Off by default in every profile. The fuzz and fuzz-deep defconfigs
 * flip it on at boot once the ready-point handshake completes;
 * workers flip it back off locally in um_snapshot_worker_init() so
 * hot paths stay zero-cost.
 */
DEFINE_STATIC_KEY_FALSE(um_snapshot_enabled);
EXPORT_SYMBOL_GPL(um_snapshot_enabled);

/* Registry of kernel mmap regions. Populated by
 * boot-time callers of um_register_mmap_region(); append-only at boot
 * and read without the lock afterwards. The lock is declared here for
 * a possible post-boot registration path, such as hot-plug hostfs or
 * runtime-attached time-travel shm.
 */
LIST_HEAD(um_mmap_regions);
EXPORT_SYMBOL_GPL(um_mmap_regions);
DEFINE_SPINLOCK(um_mmap_regions_lock);
EXPORT_SYMBOL_GPL(um_mmap_regions_lock);

/**
 * um_register_mmap_region() - record a kernel-side host mmap region.
 * @r: caller-allocated region descriptor. Must outlive the kernel
 *	(typically static-storage duration).
 *
 * Thread-safe against concurrent registration; callers that register
 * at boot before SMP comes up may call this unlocked.
 */
void um_register_mmap_region(struct um_mmap_region *r)
{
	unsigned long flags;

	if (WARN_ON_ONCE(!r || !r->name || !r->len))
		return;

	spin_lock_irqsave(&um_mmap_regions_lock, flags);
	list_add_tail(&r->list, &um_mmap_regions);
	spin_unlock_irqrestore(&um_mmap_regions_lock, flags);
}
EXPORT_SYMBOL_GPL(um_register_mmap_region);

/*
 * Strict ready-point assertions.
 *
 * Fires WARN_ONCE for every invariant the ready-point contract
 * requires. Returns 0 if all held, -EBUSY if any violation was
 * observed. Callers should propagate the error up and refuse to fork:
 * snapshotting from an inconsistent state breaks the forkserver.
 *
 * This checks the low-cost invariants needed before forking:
 *   - caller is in task context (not in IRQ or softirq)
 *   - caller has no pending non-SIGCHLD signals
 *   - UML is UP (multi-vCPU quiesce is not supported here)
 *   - UML's own signals_enabled flag is 1: the ready-point
 *     contract requires a caller who has not already gated UML
 *     signals, so the os_snapshot_block_iter_signals() call
 *     inside the forkserver loop actually takes effect. A
 *     caller who enters the ready point already-gated would
 *     otherwise silently make the block-iter a no-op and the
 *     signal-delivery hazard would return.
 *
 * More expensive checks, such as dirty inodes, pending RCU callbacks,
 * and non-caller kthreads in TASK_RUNNING, are intentionally not part
 * of this hot ready-point path.
 */
static int um_snapshot_assert_ready(const char *named_point)
{
	int violations = 0;

	if (WARN_ONCE(in_hardirq() || in_softirq(),
		      "%s(\"%s\"): ready point entered from IRQ/softirq context\n",
		      __func__, named_point))
		violations++;

	if (WARN_ONCE(signal_pending(current),
		      "%s(\"%s\"): ready point entered with pending signals\n",
		      __func__, named_point))
		violations++;

	if (WARN_ONCE(num_online_cpus() > 1,
		      "%s(\"%s\"): SMP ready point unsupported; build with NR_CPUS=1.\n",
		      __func__, named_point))
		violations++;

	if (WARN_ONCE(um_get_signals() != 1,
		      "%s(\"%s\"): UML signals_enabled is %d at ready-point entry; must be 1\n",
		      __func__, named_point, um_get_signals()))
		violations++;

	/*
	 * Refuse to enter the AFL forkserver path under the KVM backend.
	 * This path uses raw __NR_fork (os_snapshot_fork_worker), and
	 * fork() under KVM aliases the parent's /dev/kvm + per-vCPU mmap
	 * state into the child. Two processes then race ioctls against the
	 * same vCPU, the kvm_run mmap is shared (concurrent KVM_RUN
	 * corrupts state), and KVM_USER_MEMORY_REGION host VAs that were
	 * CoW-mapped to the parent's pages diverge in the child once any
	 * write fires.
	 *
	 * The proper KVM-aware snapshot path saves vCPU state via
	 * KVM_GET_REGS / SREGS / MSRS / FPU at checkpoint, snapshots
	 * the memslot host VAs, and restores via KVM_SET_* on either
	 * a fresh vCPU in the same VM or a new VM entirely. This helper
	 * is only the fork-based snapshot path; refuse loudly so a
	 * misconfigured fuzz build doesn't silently corrupt KVM state.
	 *
	 * Detection: um_backend->kind. The dynamic-backend selector may
	 * resolve to KVM at boot, so a runtime check is more reliable than
	 * a build-time dependency.
	 */
	if (WARN_ONCE(um_backend &&
		      um_backend->kind == UM_BACKEND_KIND_KVM,
		      "%s(\"%s\"): KVM backend cannot use fork snapshot\n",
		      __func__, named_point))
		violations++;

	return violations ? -EBUSY : 0;
}

static bool um_snapshot_forkserver_fds_ready(const char *named_point)
{
	/* If fds 198/199 are not open, do not enter the AFL path. This
	 * makes um_snapshot_ready() safe to invoke from debugfs even when
	 * no fuzzer is listening; the caller gets a clean printk and a
	 * non-zero return, and nothing in kernel state is disturbed.
	 *
	 * Returning here before flipping um_snapshot_enabled is load-
	 * bearing: a benign debugfs poke (someone writing a name to
	 * /sys/kernel/debug/um/snapshot_ready in a non-fuzz profile or
	 * before a fuzzer has attached) must not permanently latch the
	 * hot-path check on. If the key were enabled in the caller and
	 * this returned here, every subsequent kernel hot path gated by
	 * um_snapshot_enabled would pay the taken-branch cost forever,
	 * turning a debugfs poke into a performance regression.
	 */
	if (os_snapshot_fd_is_open(UM_FORKSERVER_CTL_FD) &&
	    os_snapshot_fd_is_open(UM_FORKSERVER_STATUS_FD))
		return true;

	pr_info("snapshot: ready point \"%s\" hit but fds %d/%d not open; forkserver skipped\n",
		named_point, UM_FORKSERVER_CTL_FD, UM_FORKSERVER_STATUS_FD);
	return false;
}

static int um_snapshot_forkserver_handshake(void)
{
	ssize_t n;

	n = os_snapshot_write_all(UM_FORKSERVER_STATUS_FD,
				  um_forkserver_hello,
				  sizeof(um_forkserver_hello));
	if (n < 0) {
		pr_err("snapshot: handshake write failed: %zd\n", n);
		return (int)n;
	}

	return 0;
}

static int um_snapshot_forkserver_parent_exit(int ret)
{
	os_snapshot_unblock_iter_signals();
	static_branch_disable(&um_snapshot_enabled);
	return ret;
}

static void um_snapshot_reap_previous_workers(void)
{
	/*
	 * Drain any zombies from previous iterations before the fuzzer's
	 * next command. Non-blocking so it never enters the wait-crash
	 * path; between iterations the fuzzer's think time is plenty for
	 * prior workers to have exited via os_snapshot_worker_exit(0), so
	 * in practice this reaps them all on the first call. If a worker
	 * has not exited yet, the zombie sticks around one more iteration,
	 * which is tolerable at any real fuzz cadence.
	 */
	(void)os_snapshot_reap_zombies();
}

static int um_snapshot_read_forkserver_cmd(unsigned long iter)
{
	u32 cmd;
	ssize_t n;

	n = os_snapshot_read_all(UM_FORKSERVER_CTL_FD, &cmd, sizeof(cmd));
	if (n >= 0)
		return 0;

	/* EPIPE / EOF: fuzzer disconnected, normal exit path. */
	if (n == -EPIPE)
		pr_info("snapshot: fuzzer disconnected after %lu iteration(s)\n",
			iter);
	else
		pr_err("snapshot: command read failed at iter %lu: %zd\n",
		       iter, n);

	return (int)n;
}

static int um_snapshot_fork_worker(unsigned long iter)
{
	int pid;

	pid = os_snapshot_fork_worker();
	if (pid < 0)
		pr_err("snapshot: fork failed at iter %lu: %d\n", iter, pid);

	return pid;
}

static int um_snapshot_report_worker_pid(unsigned long iter, int pid)
{
	ssize_t n;

	n = os_snapshot_write_all(UM_FORKSERVER_STATUS_FD,
				  &pid, sizeof(pid));
	if (n >= 0)
		return 0;

	if (n == -EPIPE)
		pr_info("snapshot: fuzzer disconnected mid-iter %lu\n", iter);
	else
		pr_err("snapshot: pid write failed at iter %lu: %zd\n",
		       iter, n);

	return (int)n;
}

static int um_snapshot_report_worker_status(unsigned long iter)
{
	int status = 0;
	ssize_t n;

	n = os_snapshot_write_all(UM_FORKSERVER_STATUS_FD,
				  &status, sizeof(status));
	if (n >= 0)
		return 0;

	if (n == -EPIPE)
		pr_info("snapshot: fuzzer disconnected while writing status at iter %lu\n",
			iter);
	else
		pr_err("snapshot: status write failed at iter %lu: %zd\n",
		       iter, n);

	return (int)n;
}

static int um_snapshot_report_worker(unsigned long iter, int pid)
{
	int ret;

	/* Parent: AFL protocol per iteration -
	 *   1. report the worker pid (4 bytes)
	 *   2. report the exit status (4 bytes, hard 0)
	 *
	 * Do not wait for the worker between pid and status writes: the
	 * parent can take host signals while it is outside normal UML kernel
	 * execution, and the signal path may re-enter the scheduler with
	 * parent-captured longjmp targets. Reap exited workers at the top of
	 * the next loop instead. Status is reported as hard 0; consumers that
	 * need worker crash status must use a side channel such as the
	 * coverage map.
	 */
	ret = um_snapshot_report_worker_pid(iter, pid);
	if (ret)
		return ret;

	return um_snapshot_report_worker_status(iter);
}

/*
 * AFL forkserver loop: handshake once, then iterate
 * {read cmd, fork, report pid} forever until the fuzzer
 * disconnects (fd 198 closes / fd 199 breaks).
 *
 * The parent never returns from this function in the happy path;
 * real fuzz use sits here for the UML process's entire lifetime.
 * On fuzzer disconnect, the function returns so the caller can tear
 * down. Iteration count is logged so userspace can see forkserver
 * progress.
 *
 * Protocol on fds 198/199 matches AFL++ afl-forkserver.c:
 *
 *   one time:
 *     parent -> 199: 4 bytes "AFL\0"                   (handshake)
 *
 *   per iteration:
 *     198    -> parent: 4 bytes testcase descriptor    (ignored)
 *     parent forks
 *     parent -> 199: 4 bytes worker host pid
 *     worker exit_group(0)
 *
 * The multi-iteration loop lets a fuzzer drive arbitrarily many
 * iterations and exposes leaks across iterations, inherited fd state,
 * and parent drift.
 */
static int um_snapshot_forkserver_loop(const char *named_point)
{
	unsigned long iter = 0;
	int pid, ret;

	if (!um_snapshot_forkserver_fds_ready(named_point))
		return -ENODEV;

	ret = um_snapshot_forkserver_handshake();
	if (ret)
		return ret;

	/*
	 * Forkserver is committed: fds plumbed, handshake sent. Flip
	 * the static key on so any hot-path call site gated by
	 * um_snapshot_enabled sees the enabled branch for the life of
	 * the loop. Every exit path below disables the key again so
	 * the parent never lives past the loop with the key latched
	 * on. Workers flip the key off in their own address space
	 * right after fork (see pid == 0 branch below).
	 */
	static_branch_enable(&um_snapshot_enabled);

	pr_info("snapshot: forkserver up at \"%s\"; entering loop\n",
		named_point);

	/* Block the host signals UML normally dispatches to its in-
	 * kernel IRQ handlers for the duration of the loop. The loop
	 * is pure host-syscall work (read / fork / waitpid / write)
	 * and any UML signal dispatch re-entering from this context
	 * has repeatedly corrupted parent-side control flow. With
	 * signals masked, waitpid / write / read run uninterrupted and
	 * UML resumes normal signal handling after the loop exits.
	 */
	os_snapshot_block_iter_signals();

	for (;;) {
		um_snapshot_reap_previous_workers();

		ret = um_snapshot_read_forkserver_cmd(iter);
		if (ret)
			return um_snapshot_forkserver_parent_exit(ret);

		pid = um_snapshot_fork_worker(iter);
		if (pid < 0)
			return um_snapshot_forkserver_parent_exit(pid);

		if (pid == 0) {
			/*
			 * Worker path. Keep UML signals gated until worker
			 * host-side state has been rebuilt; otherwise timer or
			 * SIGIO delivery can re-enter the scheduler with
			 * parent-captured longjmp targets.
			 */
			um_snapshot_worker_init();
			return 0;
		}

		ret = um_snapshot_report_worker(iter, pid);
		if (ret)
			return um_snapshot_forkserver_parent_exit(ret);

		iter++;
	}
}

/**
 * um_snapshot_ready() - reach the named "ready point" and quiesce.
 * @named_point: printable identifier of the call site.
 *
 * Runs the AFL forkserver loop until the fuzzer disconnects, if fds
 * 198/199 are open; otherwise emits an info message and returns. In
 * the happy path the parent remains in the forkserver loop for the UML
 * process's lifetime, and only workers return.
 */
void um_snapshot_ready(const char *named_point)
{
	int ret;

	if (!named_point)
		named_point = "(null)";

	ret = um_snapshot_assert_ready(named_point);
	if (ret) {
		pr_err("snapshot: ready point \"%s\" refused: %d violation(s)\n",
		       named_point, -ret);
		return;
	}

	/*
	 * um_snapshot_enabled is flipped on INSIDE the forkserver loop
	 * only after the fd-open check passes and the handshake lands,
	 * and flipped off again on every loop exit path. That keeps a
	 * benign debugfs poke (ready-point hit with no fuzzer plumbed)
	 * from latching the hot-path cost on.
	 */
	ret = um_snapshot_forkserver_loop(named_point);

	/*
	 * Two classes of callers reach this point:
	 *   - Parent, on disconnect / error (fuzzer closed its end,
	 *     fork failed, write failed).
	 *   - Worker, returning up through the forkserver loop to
	 *     continue guest execution.
	 *
	 * Deliberately no pr_info here. The worker's kernel state
	 * immediately post-fork has seen fork-inheritance quirks
	 * that trip vsnprintf via its per-CPU / TLS lookups
	 * before worker-side state is consistent enough for printk.
	 * Suppress ret to silence unused-variable warnings.
	 */
	(void)ret;
}
EXPORT_SYMBOL_GPL(um_snapshot_ready);

/**
 * um_snapshot_worker_init() - drop parent-inherited host-side state
 * in a fork-server worker.
 *
 * Called exactly once in the forked child right after the
 * os_snapshot_fork_worker() return discriminates parent vs worker.
 * Drops parent-owned references to host threads, POSIX timers,
 * and SIGIO infrastructure, then rebuilds the child-side pieces that
 * can be safely recreated before guest execution resumes.
 *
 * What fork() inherits but child cannot safely use:
 *   - write_sigio_td (pthread_t of parent's SIGIO helper thread):
 *     invalid in the child because no such thread exists. Abandon,
 *     do not join.
 *   - epollfd (file descriptor parent's SIGIO thread was waiting on):
 *     inherited but has no reader in the child. Close.
 *   - POSIX timers (per-CPU): timer_create()'d with SIGEV_THREAD_ID
 *     pointing at parent CPU threads' gettid()s; those tids do not
 *     exist in the child. Disable so no signal delivery is
 *     attempted.
 *
 * Explicitly out of scope here:
 *   - Recreating a child-side signalfd.
 *   - Seccomp stub children: no stubs exist at ready-point because
 *     no guest userspace task has run yet. First user-space
 *     syscall in a worker will lazily spawn one via start_userspace().
 *   - Mconsole socket: compiled out in the fuzz profile per
 *     03-profiles/fuzz.md; nothing to forget.
 *   - UBD / winch / virtio: not present in the fuzz profile init;
 *     profiles that enable them with snapshot need their own forget
 *     helpers here.
 *
 */
void um_snapshot_worker_init(void)
{
	int err;

	/*
	 * Step 0: flip um_snapshot_enabled off in the child's own
	 * address space. The key was forked-in ON from the parent
	 * (enabled inside um_snapshot_forkserver_loop before the
	 * fork_worker call). The worker resumes guest execution
	 * and must not pay the snapshot hot-path cost; every gate
	 * guarded by static_branch_unlikely(&um_snapshot_enabled)
	 * should behave the same way as on a non-snapshot
	 * kernel. Matches the documented contract in
	 * arch/um/include/asm/um-snapshot.h.
	 */
	static_branch_disable(&um_snapshot_enabled);

	/*
	 * Forget parent-inherited state. Drops references that point at
	 * threads / tids / pthread
	 * handles unique to the parent. Must happen before any
	 * rebuild so the rebuild doesn't inherit parent-owned state.
	 */
	os_sigio_worker_forget();
	os_timer_worker_forget();

	/*
	 * Detach fork-inherited tasks from
	 * the worker's CFS runqueue. Before this, schedule() can pick
	 * kthreads like ksoftirqd whose saved jmp_buf targets parent-
	 * side host-thread state that doesn't exist in the worker.
	 * The helper is defined in kernel/sched/core.c and guarded by
	 * CONFIG_UM_SNAPSHOT_FORKSERVER.
	 */
	sched_worker_detach_other_tasks();

	/*
	 * Rebuild fresh host-side infrastructure in the worker's own
	 * address space. Register handlers, fds, and timers before
	 * reopening UML signal delivery.
	 */
	err = os_sigio_worker_rebuild();
	if (err)
		pr_err("snapshot: worker sigio rebuild failed: %d\n", err);

	err = os_timer_worker_rebuild();
	if (err)
		pr_err("snapshot: worker timer rebuild failed: %d\n", err);

	/*
	 * At the current ready point, before the first guest userspace
	 * task, mm_list is empty by construction. Later ready points that
	 * allow guest tasks must clear mm_list under mm_list_lock here.
	 *
	 * Deliberately no printk-on-success here: the worker's
	 * kernel state immediately post-fork has seen fork-
	 * inheritance quirks that trip vsnprintf via its per-CPU /
	 * TLS lookups. pr_err above is tolerated because it only
	 * fires on failure.
	 */
}
EXPORT_SYMBOL_GPL(um_snapshot_worker_init);

/*
 * Debugfs trigger. Writing a printable name to
 * /sys/kernel/debug/um/snapshot_ready invokes um_snapshot_ready()
 * with that name.
 */

#ifdef CONFIG_DEBUG_FS

static ssize_t um_snapshot_debugfs_write(struct file *f, const char __user *buf,
					 size_t count, loff_t *ppos)
{
	char name[32];
	size_t n = count;

	if (n >= sizeof(name))
		n = sizeof(name) - 1;
	if (copy_from_user(name, buf, n))
		return -EFAULT;
	name[n] = '\0';

	/* Strip a trailing newline so echo foo > node DTRT. */
	if (n > 0 && name[n - 1] == '\n')
		name[n - 1] = '\0';

	um_snapshot_ready(name[0] ? name : "debugfs");
	return count;
}

static const struct file_operations um_snapshot_debugfs_fops = {
	.write		= um_snapshot_debugfs_write,
};

static int __init um_snapshot_debugfs_init(void)
{
	struct dentry *d;

	/* Share the "um" dentry with arch/um/kernel/um_debugfs.c so the
	 * snapshot_ready file appears alongside its backend / stats /
	 * hooks entries. late_initcall_sync() runs strictly after
	 * um_debugfs.c's late_initcall() has created the "um" dir;
	 * debugfs_lookup() then picks it up. If DEBUG_FS is on but
	 * um_debugfs.c did not create it, create it here.
	 */
	d = debugfs_lookup("um", NULL);
	if (!d) {
		d = debugfs_create_dir("um", NULL);
		if (IS_ERR(d))
			return PTR_ERR(d);
	}

	debugfs_create_file("snapshot_ready", 0200, d, NULL,
			    &um_snapshot_debugfs_fops);
	return 0;
}
late_initcall_sync(um_snapshot_debugfs_init);

#endif /* CONFIG_DEBUG_FS */

/*
 * /sys/kernel/um/state_version - read-only unsigned integer telling
 * userspace which version of the UML snapshot/forkserver wire contract
 * this kernel speaks. Incremented on protocol-breaking changes; 1 is
 * the initial AFL-compatible 12-bytes-per-iteration protocol.
 *
 * Read by selftests and external fuzzers so they can refuse to bind to a
 * kernel whose snapshot/forkserver wire protocol they do not understand.
 * Note: "state_version" is the wire-protocol version, not a kernel build
 * version; it stays 1 across unrelated kernel bumps as long as the
 * 12-byte protocol is unchanged.
 */
#define UM_SNAPSHOT_STATE_VERSION 1u

static struct kobject *um_snapshot_kobj;

static ssize_t state_version_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", UM_SNAPSHOT_STATE_VERSION);
}

static struct kobj_attribute um_snapshot_state_version_attr =
	__ATTR_RO(state_version);

static int __init um_snapshot_sysfs_init(void)
{
	int err;

	um_snapshot_kobj = kobject_create_and_add("um", kernel_kobj);
	if (!um_snapshot_kobj)
		return -ENOMEM;

	err = sysfs_create_file(um_snapshot_kobj,
				&um_snapshot_state_version_attr.attr);
	if (err) {
		kobject_put(um_snapshot_kobj);
		um_snapshot_kobj = NULL;
		return err;
	}

	return 0;
}
late_initcall(um_snapshot_sysfs_init);
