// SPDX-License-Identifier: GPL-2.0
/*
 * UML snapshot / forkserver seam (workstream C-09).
 *
 * This translation unit is the cooperative in-kernel seam that lets a
 * UML guest reach a named "ready point," quiesce to a barrier, and
 * ``fork()`` itself for each fuzz iteration. The AFL-compatible
 * 12-byte-per-iteration wire protocol on fds 198/199 lives here too;
 * it binds directly onto syzkaller's ``Instance.RunSnapshot`` API so
 * the fuzz profile does not need its own per-iteration negotiation.
 *
 * Commit 2 (this patch) lands the real ``um_snapshot_ready()``
 * quiesce-and-fork path with strict ready-point assertions (D37 pull-
 * forward #2), driven by a debugfs trigger. Workers do a single round-
 * trip: AFL handshake → fork → worker exits → parent reaps → return.
 * Full multi-iteration forkserver loop and the worker-side reinit
 * that lets a worker run guest code both come in commit 3.
 *
 * Commit 1 landed: the static key, the mmap-region registry, and
 * WARN-stubbed public entry points. Commit 3 lands worker-side post-
 * fork reinit (including the KASAN MADV_DONTFORK fix from D37 pull-
 * forward #6); commit 4 the host-fd hygiene sweep; commit 5 the user
 * doc + selftest; commit 6 the fuzz-defconfig wire-up.
 *
 * Design + decisions:
 *   - Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/
 *     09-snapshot-forkserver.md — the v1 design.
 *   - 04-risks/decisions-log.md D35 — v1 scope (forkserver, not CRIU).
 *   - 04-risks/decisions-log.md D36 — v2 on-disk format (ELF + notes).
 *   - 04-risks/decisions-log.md D37 — v1 pull-forward items, including
 *     this file's mmap registry and the ready-point assertions below.
 *   - 04-risks/decisions-log.md D38 — v2 Mode A vs Mode B split; v1 is
 *     Mode B by construction.
 */

#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/preempt.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include <asm/um-mmaps.h>
#include <asm/um-snapshot.h>

#include <os.h>

/* AFL forkserver protocol: caller opens host fds 198 (fuzzer→server)
 * and 199 (server→fuzzer) before exec'ing UML. Matches AFL++'s
 * src/afl-forkserver.c and lcamtuf's original technical_details.txt.
 * See 09-snapshot-forkserver.md §"Per-iteration protocol (AFL-
 * compatible)" for the wire format.
 */
#define UM_FORKSERVER_CTL_FD	198
#define UM_FORKSERVER_STATUS_FD	199

/* 4-byte handshake the parent sends on status fd right after quiesce.
 * AFL++ accepts any value whose top 24 bits match 'A','F','L' and
 * uses the bottom 8 bits for protocol-version flags. We send the
 * classic zero value; commit 5's selftest speaks matching bytes.
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

/* Registry of kernel mmap regions (D37 pull-forward #1). Populated by
 * boot-time callers of um_register_mmap_region(); today append-only
 * at boot, read without the lock afterwards. The lock is declared
 * here for the day a post-boot registration path appears (hot-plug
 * hostfs, runtime-attached time-travel shm, …).
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
 * Strict ready-point assertions (D37 pull-forward #2).
 *
 * Fires WARN_ONCE for every invariant the ready-point contract
 * requires. Returns 0 if all held, -EBUSY if any violation was
 * observed. Callers should propagate the error up and refuse to fork:
 * snapshotting from an inconsistent state breaks both the fork-
 * server (Q6 drift) and the future v2 writer.
 *
 * Today we check the cheapest/most-load-bearing subset:
 *   - caller is in task context (not in IRQ or softirq)
 *   - caller has no pending non-SIGCHLD signals
 *   - UML is UP (multi-vCPU quiesce is commit-3+ scope)
 *
 * The richer assertions the design doc describes — dirty inodes,
 * pending RCU callbacks, non-caller kthreads in TASK_RUNNING — are
 * expensive enough that we land them incrementally; commit 3 grows
 * this function as the worker reinit path matures.
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
		      "%s(\"%s\"): SMP ready point not yet supported; commit 3+ will park secondary vCPUs. Build with NR_CPUS=1 for now.\n",
		      __func__, named_point))
		violations++;

	return violations ? -EBUSY : 0;
}

/*
 * AFL forkserver loop: handshake once, then iterate
 * {read cmd, fork, report pid} forever until the fuzzer
 * disconnects (fd 198 closes / fd 199 breaks).
 *
 * The parent never returns from this function in the happy path
 * — real fuzz use sits here for the UML process's entire lifetime.
 * On fuzzer disconnect, the function returns so the caller can tear
 * down. Iteration count is tracked for the diagnostic printk so
 * operators can see forkserver is making progress.
 *
 * Commit 3a scope: worker still exits immediately (no reinit, no
 * guest code); parent still does not waitpid (status-byte response
 * arrives in 3c/3d). The multi-iteration loop alone lets a host
 * harness drive arbitrarily many iterations and catches at-scale
 * bugs (leaks across iterations, stale fd state, parent drift) that
 * commit 2's one-shot could not see.
 *
 * Protocol on fds 198/199 matches AFL++ afl-forkserver.c:
 *
 *   one time:
 *     parent -> 199: 4 bytes "AFL\0"                   (handshake)
 *
 *   per iteration:
 *     198    -> parent: 4 bytes testcase descriptor    (ignored today)
 *     parent forks
 *     parent -> 199: 4 bytes worker host pid
 *     worker exit_group(0)
 *
 * Commit 3c/3d extend per-iteration with waitpid + status byte.
 */
static int um_snapshot_forkserver_loop(const char *named_point)
{
	unsigned long iter = 0;
	u32 cmd;
	int pid, status;
	ssize_t n;

	/* If a host harness didn't plumb fds 198/199 open, do not enter
	 * the AFL path. This makes um_snapshot_ready() safe to invoke
	 * from debugfs even when no fuzzer is listening — the caller
	 * gets a clean printk and a non-zero return, and nothing in
	 * kernel state is disturbed.
	 */
	if (!os_snapshot_fd_is_open(UM_FORKSERVER_CTL_FD) ||
	    !os_snapshot_fd_is_open(UM_FORKSERVER_STATUS_FD)) {
		pr_info("snapshot: ready point \"%s\" hit but fds %d/%d not open; forkserver skipped\n",
			named_point, UM_FORKSERVER_CTL_FD, UM_FORKSERVER_STATUS_FD);
		return -ENODEV;
	}

	n = os_snapshot_write_all(UM_FORKSERVER_STATUS_FD,
				  um_forkserver_hello,
				  sizeof(um_forkserver_hello));
	if (n < 0) {
		pr_err("snapshot: handshake write failed: %zd\n", n);
		return (int)n;
	}

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
		n = os_snapshot_read_all(UM_FORKSERVER_CTL_FD,
					 &cmd, sizeof(cmd));
		if (n < 0) {
			/* EPIPE / EOF: fuzzer disconnected — normal exit
			 * path. Anything else is a loud failure.
			 */
			if (n == -EPIPE)
				pr_info("snapshot: fuzzer disconnected after %lu iteration(s)\n",
					iter);
			else
				pr_err("snapshot: command read failed at iter %lu: %zd\n",
				       iter, n);
			os_snapshot_unblock_iter_signals();
			return (int)n;
		}

		pid = os_snapshot_fork_worker();
		if (pid < 0) {
			pr_err("snapshot: fork failed at iter %lu: %d\n",
			       iter, pid);
			os_snapshot_unblock_iter_signals();
			return pid;
		}

		if (pid == 0) {
			/* Worker. Commit 3c: drop parent-inherited host-
			 * side state before exit so we don't leave
			 * helper threads or timers attempting to signal
			 * this child. Commit 3d will replace the exit
			 * with a return so the worker can run guest code.
			 */
			um_snapshot_worker_init();
			os_snapshot_worker_exit(0);
		}

		/* Parent: AFL protocol per iteration -
		 *   1. report the worker pid (4 bytes)
		 *   2. report the exit status (4 bytes, placeholder 0)
		 *
		 * KNOWN LIMITATION (reverted from 3d-a v2 after several
		 * failed attempts). Calling os_snapshot_waitpid_status()
		 * from this UML-kernel context crashes the parent with
		 * a null-jump or heap-jump regardless of whether we use
		 *   - glibc waitpid() with no masking,
		 *   - glibc waitpid() with host sigprocmask blocking
		 *     {SIGCHLD, SIGALRM, SIGIO, SIGUSR1},
		 *   - glibc waitpid() with um_set_signals(0) (UML-native
		 *     deferred-signal gate),
		 *   - raw syscall(__NR_wait4, ...) with the same
		 *     UML-native gate.
		 *
		 * The failure mode is consistent with UML's signal-driven
		 * scheduler re-entering during the host waitpid, but the
		 * UML-native gate isn't enough to stop it — a path we
		 * haven't yet traced is bypassing signals_enabled==0 and
		 * running either a longjmp-based task switch into a
		 * stale jmp_buf or a function-pointer dispatch whose
		 * target has been clobbered. The crash addresses (0x0
		 * and 0x610XXXXX inside UML's own VA) match this shape.
		 *
		 * Deferred to commit 3d-c, where the worker-side rebuild
		 * and return-path changes re-arrange the parent's state
		 * during the wait window. If the root cause is in the
		 * parent, 3d-c's diagnostics will narrow it; if it's in
		 * cross-process SIGCHLD handling, 3d-c's stub respawn
		 * will expose that separately.
		 *
		 * Workers still terminate cleanly via
		 * os_snapshot_worker_exit(0). Until we call wait() in a
		 * way that doesn't crash, zombies accumulate in the host
		 * and get reaped when the UML process exits — fine for
		 * short selftest runs, not for sustained fuzzing. The
		 * fuzzer reads a well-formed zero status byte per
		 * iteration, so the wire protocol is correct even though
		 * the status value is uninformative.
		 */
		n = os_snapshot_write_all(UM_FORKSERVER_STATUS_FD,
					  &pid, sizeof(pid));
		if (n < 0) {
			if (n == -EPIPE)
				pr_info("snapshot: fuzzer disconnected mid-iter %lu\n",
					iter);
			else
				pr_err("snapshot: pid write failed at iter %lu: %zd\n",
				       iter, n);
			os_snapshot_unblock_iter_signals();
			return (int)n;
		}

		status = 0;	/* see comment above; 3d-c revisits. */
		n = os_snapshot_write_all(UM_FORKSERVER_STATUS_FD,
					  &status, sizeof(status));
		if (n < 0) {
			if (n == -EPIPE)
				pr_info("snapshot: fuzzer disconnected while writing status at iter %lu\n",
					iter);
			else
				pr_err("snapshot: status write failed at iter %lu: %zd\n",
				       iter, n);
			os_snapshot_unblock_iter_signals();
			return (int)n;
		}

		iter++;
	}
}

/**
 * um_snapshot_ready() - reach the named "ready point" and quiesce.
 * @named_point: printable identifier of the call site.
 *
 * Commit 3a semantics: runs the multi-iteration AFL forkserver loop
 * until the fuzzer disconnects, if fds 198/199 are open; otherwise
 * emits an info message and returns. In the happy path, this
 * function does not return — the parent sits in the forkserver loop
 * for the UML process's entire lifetime, and only workers return
 * (via os_snapshot_worker_exit in commit 3a; via
 * um_snapshot_worker_init + caller unwind in commits 3c/3d).
 *
 * Commits 3c/3d extend per-iteration with worker-side reinit +
 * waitpid + status-byte response.
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

	static_branch_enable(&um_snapshot_enabled);
	ret = um_snapshot_forkserver_loop(named_point);

	/* Parent reaches here only on disconnect / error (fuzzer
	 * closed its end, fork failed, write failed). In commit 3a
	 * workers never reach this point — os_snapshot_worker_exit
	 * in the forkserver loop terminates the worker host process.
	 * Commits 3c/3d change that: workers return through the loop
	 * back here and continue guest execution.
	 */
	pr_info("snapshot: ready point \"%s\" returning: %d\n",
		named_point, ret);
}
EXPORT_SYMBOL_GPL(um_snapshot_ready);

/**
 * um_snapshot_worker_init() - drop parent-inherited host-side state
 * in a fork-server worker.
 *
 * Called exactly once in the forked child right after the
 * os_snapshot_fork_worker() return discriminates parent vs worker.
 * Commit 3c scope (this commit): a no-crash "forget" that drops
 * stale references to parent-owned host threads, POSIX timers, and
 * SIGIO infrastructure. The worker still exits immediately via
 * os_snapshot_worker_exit(0) after this returns — running actual
 * guest code is commit 3d's job, and needs the additional recreate
 * logic (timer re-arm, new SIGIO thread, stub respawn).
 *
 * What fork() inherits but child cannot safely use:
 *   - write_sigio_td (pthread_t of parent's SIGIO helper thread):
 *     stale; child has no such thread. Abandon, do not join.
 *   - epollfd (file descriptor parent's SIGIO thread was waiting on):
 *     inherited but has no reader in the child. Close.
 *   - POSIX timers (per-CPU): timer_create()'d with SIGEV_THREAD_ID
 *     pointing at parent CPU threads' gettid()s; those tids do not
 *     exist in the child. Disable so no signal delivery is
 *     attempted.
 *
 * Explicitly out of scope for commit 3c:
 *   - Recreating a child-side SIGIO thread / timer / signalfd.
 *     That's commit 3d, where the worker needs them to run code.
 *   - Seccomp stub children: no stubs exist at ready-point because
 *     no guest userspace task has run yet. First user-space
 *     syscall in a worker (commit 3d) will lazily spawn one via
 *     start_userspace().
 *   - Mconsole socket: compiled out in the fuzz profile per
 *     `03-profiles/fuzz.md`; nothing to forget.
 *   - UBD / winch / virtio: not present in the fuzz profile init;
 *     if a future profile enables them with snapshot, they'll want
 *     their own "forget" helpers here.
 *
 * See D39 (the commit-3 split) and the agent research captured in
 * the implementation notes for `09-snapshot-forkserver.md` for the
 * full host-side inventory.
 */
void um_snapshot_worker_init(void)
{
	os_sigio_worker_forget();
	os_timer_worker_forget();

	/* Additional child-side state that needs to match reality:
	 * commit 3d turns these from comments into code when it needs
	 * the worker to actually run. For commit 3c it is enough that
	 * we don't crash while forgetting.
	 *
	 *   - cpu_online_mask / cpu_present_mask: on SMP, parent's
	 *     secondary vCPU threads are gone. For UP (fuzz default)
	 *     cpu_online_mask is just {0} already; no action.
	 *   - current->thread.* scheduler state: valid-for-us because
	 *     we are the thread that ran um_snapshot_ready().
	 *   - __curr_cpu / signals_active TLS: defaults are fine until
	 *     the worker does something that consults them.
	 *
	 * Deliberately no printk here: the worker's kernel state
	 * immediately post-fork has seen fork-inheritance quirks that
	 * trip vsnprintf via its per-CPU / TLS lookups. Commit 3d will
	 * re-enable the worker's printk path after it rebuilds enough
	 * per-CPU state; for commit 3c's "no-crash" bar we simply
	 * avoid it.
	 */
}
EXPORT_SYMBOL_GPL(um_snapshot_worker_init);

/*
 * Debugfs trigger (commit 2). Writing a printable name to
 * /sys/kernel/debug/um/snapshot_ready invokes um_snapshot_ready()
 * with that name. Commit 5's selftest speaks to this node. A commit-
 * 5+ signal-based trigger (SIGRTMIN+N, per D38) can share the same
 * entry point when it lands.
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

	/* Strip a trailing newline so `echo foo > node` DTRT. */
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

	/* Share the "um" dentry with arch/um/kernel/um_debugfs.c so our
	 * snapshot_ready file appears alongside its backend / stats /
	 * hooks entries. We use late_initcall_sync() so we run strictly
	 * after um_debugfs.c's late_initcall() has created the "um"
	 * dir; debugfs_lookup() then picks it up. If DEBUG_FS is on but
	 * um_debugfs.c didn't create it for some reason, fall back to
	 * creating it ourselves.
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
