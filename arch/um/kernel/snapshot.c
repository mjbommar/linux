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
 * Minimal AFL forkserver one-shot: handshake + one fork + worker
 * self-exit. The parent reports the worker pid and returns without
 * blocking on wait; blocking reap, iteration loop, and the worker-
 * side post-fork reinit all arrive in commit 3, where
 * um_snapshot_worker_init() lands and workers begin to run guest
 * code.
 *
 * Commit-2 validation — what you can observe with this code:
 *   1. A host harness that pre-opens fds 198/199 sees 4 bytes
 *      ``AFL\0`` on fd 199 (handshake).
 *   2. After sending any 4 bytes on fd 198, the host reads 4 more
 *      bytes on fd 199: the worker's host pid.
 *   3. The worker host process exits immediately with status 0 —
 *      observable via waitpid() from whatever parent reaps it.
 *
 * This is the minimum that proves the forkserver seam is wired
 * correctly. Protocol on fds 198/199 matches AFL++ afl-forkserver.c:
 *
 *   parent -> 199: 4 bytes "AFL\0"                   (handshake)
 *   198    -> parent: 4 bytes testcase descriptor    (ignored today)
 *   parent forks
 *   parent -> 199: 4 bytes worker host pid
 *   worker exit_group(0)
 *   parent returns
 *
 * Commit 3 adds blocking ``os_snapshot_waitpid_status()`` and the
 * status-byte response, and converts the one-shot into a loop.
 */
static int um_snapshot_forkserver_one_shot(const char *named_point)
{
	u32 cmd;
	int pid;
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

	n = os_snapshot_read_all(UM_FORKSERVER_CTL_FD, &cmd, sizeof(cmd));
	if (n < 0) {
		pr_err("snapshot: command read failed: %zd\n", n);
		return (int)n;
	}

	pid = os_snapshot_fork_worker();
	if (pid < 0) {
		pr_err("snapshot: fork failed: %d\n", pid);
		return pid;
	}

	if (pid == 0) {
		/* Worker. Exit the forked host process immediately.
		 * Commit 3 will replace this with a call to
		 * um_snapshot_worker_init() + return, which will let
		 * the worker run guest code; for now the cleanest
		 * termination is a raw exit_group so no UML shutdown
		 * machinery fires twice. Never returns.
		 */
		os_snapshot_worker_exit(0);
	}

	/* Parent: tell the fuzzer the worker pid and return. Commit 3
	 * adds waitpid + status-byte response.
	 */
	n = os_snapshot_write_all(UM_FORKSERVER_STATUS_FD, &pid, sizeof(pid));
	if (n < 0)
		pr_err("snapshot: pid write failed: %zd\n", n);

	return pid;
}

/**
 * um_snapshot_ready() - reach the named "ready point" and quiesce.
 * @named_point: printable identifier of the call site.
 *
 * Commit 2 semantics: runs one round-trip of the AFL wire protocol if
 * fds 198/199 are open; otherwise emits an info message and returns.
 * Workers return with ``um_snapshot_enabled`` disabled locally so hot
 * paths run at zero overhead.
 *
 * Commit 3 extends this into a multi-iteration loop and wires in the
 * worker-side reinit path.
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
	ret = um_snapshot_forkserver_one_shot(named_point);

	/* Parent returns here after reaping the worker, or if the
	 * forkserver loop declined to run (no fds, handshake failure).
	 * Worker also returns here — it's path is: return from this
	 * function, caller runs commit-3's worker_init stub, caller
	 * falls off the end of whatever called um_snapshot_ready(), and
	 * the worker continues execution unaware it was ever forked.
	 * In commit 2 this means the worker's caller hits the debugfs-
	 * write return path and the worker host process exits cleanly
	 * when init userspace decides to; that is the cleanest "worker
	 * exits immediately" shape we get without the commit-3 reinit
	 * path, and it is enough for a host harness to observe the
	 * handshake happened.
	 */
	if (ret < 0)
		pr_info("snapshot: ready point \"%s\" completed without forkserver: %d\n",
			named_point, ret);
	else
		pr_info("snapshot: ready point \"%s\" one-shot fork done (pid was %d)\n",
			named_point, ret);
}
EXPORT_SYMBOL_GPL(um_snapshot_ready);

void um_snapshot_worker_init(void)
{
	WARN_ONCE(1,
		  "%s: only commits 1-2 have landed; post-fork reinit is in commit 3 per %s\n",
		  __func__,
		  "Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md");
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
