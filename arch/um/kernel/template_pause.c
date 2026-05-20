// SPDX-License-Identifier: GPL-2.0
/*
 * UML template-pause + fork hook (Memo 09 Phase 1a).
 *
 * The cooperative in-kernel seam that lets a booted UML quiesce at a
 * named "ready point," SIGSTOP the host process for the supervisor to
 * fork(2), and resume on SIGCONT with a per-child identity blob
 * applied.  See Documentation/virt/uml/redesign/06-sequencing/
 * post-2026-05-19-next-sprint/09-fork-server-snapshot-restore.md
 * for the full design.
 *
 * Distinct from arch/um/kernel/snapshot.c's AFL-style forkserver path:
 * that path's parent never returns and the parent itself drives per-
 * iteration fork via fds 198/199.  Here the kernel doesn't fork —
 * the umlctl supervisor does, externally, after seeing the master's
 * SIGSTOP.  Each forked child wakes from the same SIGSTOP point with
 * its own identity blob to apply (Phase 2 wires the actual MAC / IP /
 * tap-fd replumb; Phase 1a just reads + logs).
 *
 * Phase 1a scope (this commit):
 *   - __setup("um_template_pause", ...) arm flag
 *   - /proc/um/template_pause write trigger
 *   - um_template_pause_enter() core: log → SIGSTOP → log → read
 *     identity blob → validate magic/version → log
 *
 * Out of scope (Phase 2):
 *   - Applying the identity (netdev MAC swap, IP rebind, tap fd swap)
 *   - mconsole socket re-path
 *   - sysfs node reflecting current identity
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <asm/um-template-pause.h>
#ifdef CONFIG_UM_TEMPLATE_PAUSE_FORK
#include <asm/um-snapshot.h>		/* um_snapshot_worker_init() */
#include <skas.h>			/* um_skas_teardown_all_stubs(),
					 *  um_skas_respawn_all_stubs() */
#endif

#include <backend.h>				/* um_backend, kind enum */

#include <os.h>

/*
 * Armed via "um_template_pause" on the kernel cmdline.  Off by
 * default: a non-pool boot must never accidentally raise SIGSTOP on
 * itself just because a stray /proc write lands.  The proc write
 * handler checks this flag and returns -ENODEV when unarmed.
 */
static bool template_pause_armed_flag __read_mostly;

/*
 * Fork-on-resume mode.  Off by default (Phase 1a behaviour: single
 * SIGSTOP/SIGCONT cycle, master IS the taken instance).  Armed when
 * the kernel cmdline reads `um_template_pause=fork` (Phase 2a:
 * fork-on-resume loop — each SIGCONT forks the master, child wakes
 * as the taken pool member, master re-pauses for the next take).
 *
 * Requires CONFIG_UM_TEMPLATE_PAUSE_FORK at build time (selects the
 * CoW physmem path + um_snapshot_worker_init).  Without that config,
 * even `=fork` is downgraded to Phase 1a behaviour with a warning.
 */
static bool template_pause_fork_armed_flag __read_mostly;

/* Diagnostic only — counts how many SIGSTOP/SIGCONT cycles this
 * process (or any of its forked descendants that still share the
 * .data segment) has been through.  A fresh fork() inherits the
 * counter value of its parent; consumers reading
 * /sys/kernel/um/template_pause_count can use that to distinguish
 * "master that has paused once" from "child that has paused twice."
 */
static atomic_t template_pause_count = ATOMIC_INIT(0);

static int __init template_pause_setup(char *str)
{
	template_pause_armed_flag = true;
	if (str && !strcmp(str, "=fork")) {
		template_pause_fork_armed_flag = true;
		pr_warn("template_pause: armed via kernel cmdline (fork-on-resume) — EXPERIMENTAL; known to corrupt parent-side seccomp stub state. See 09-fork-server-STATUS.md.\n");
	} else {
		pr_info("template_pause: armed via kernel cmdline\n");
	}
	return 1;
}
__setup("um_template_pause", template_pause_setup);

bool um_template_pause_armed(void)
{
	return template_pause_armed_flag;
}
EXPORT_SYMBOL_GPL(um_template_pause_armed);

/*
 * Read + validate the identity blob.  Returns 0 on a present + valid
 * blob (fields are NUL-terminated and logged), >0 if @identity_fd is
 * -1 (no channel — informational), or -errno on read / validation
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
 * One SIGSTOP/SIGCONT cycle + identity read.  Shared between the
 * Phase 1a single-shot path and each iteration of the Phase 2a
 * fork-on-resume loop.
 */
static int one_pause_cycle(const char *named_point, int identity_fd,
			   struct um_template_identity *blob_out)
{
	int ret;

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
 * Refuse the fork-on-resume path under the KVM backend.  Same
 * reasoning as arch/um/kernel/snapshot.c's
 * um_snapshot_assert_ready(): fork() aliases /dev/kvm + per-vCPU
 * mmap state into the child, and two processes racing ioctls
 * against shared KVM state corrupts both halves.  KVM-aware fork
 * is a future phase (will issue KVM_CREATE_VCPU per child after
 * the fork point).
 */
static int assert_fork_safety(const char *named_point)
{
	if (um_backend && um_backend->kind == UM_BACKEND_KIND_KVM) {
		pr_err("template_pause: fork-on-resume refused under KVM backend at \"%s\" — kvm-aware fork is a future phase\n",
		       named_point);
		return -EOPNOTSUPP;
	}

	/*
	 * Refuse if any OTHER mm has a stub parked in FUTEX_IN_KERN —
	 * tearing it down (which §3.3's loop will do) would leave the
	 * task parked forever from the kernel's view, indistinguishable
	 * from a stub crash, and the existing mm_sigchld_irq path
	 * would fire fatal_sigsegv on it.  The caller's own mm is
	 * exempt (it IS mid-syscall — that's the proc_write that
	 * brought us here).  See PHASE2A-DESIGN.md §3.5.
	 */
	if (um_skas_other_mm_mid_syscall(current_mm_id())) {
		pr_err("template_pause: fork-on-resume refused at \"%s\" — another mm is mid-syscall (FUTEX_IN_KERN). Quiesce other guest tasks before pausing.\n",
		       named_point);
		return -EBUSY;
	}

	return 0;
}

/*
 * Phase 2a — fork-on-resume loop.  On each SIGCONT, the master
 * fork(2)s; the child reuses um_snapshot_worker_init() to drop
 * parent-inherited SIGIO / POSIX-timer / runqueue state and returns
 * up the stack so the bootstrap script continues as the taken
 * instance.  The parent writes the new child's pid to the identity
 * memfd at offset `sizeof(struct um_template_identity)` (so the
 * supervisor can read it back) and loops to the next SIGSTOP.
 *
 * Parent never re-enters UML kernel scheduling between forks — the
 * loop is pure host-syscall work (kill + fork + write + kill).
 * UML's in-kernel signal dispatch is gated for the loop body via
 * os_snapshot_block_iter_signals() to avoid the D41 SIGILL hazard
 * that bit the AFL forkserver's parent-side wait4 path.
 *
 * Returns from the CHILD with the taken identity in @first_blob (or
 * zero-initialised if the first take had no identity channel).
 * Never returns from the parent in the happy path; on fatal error
 * the loop unwinds and the master process should be killed by the
 * supervisor.
 */
static int fork_on_resume_loop(const char *named_point, int identity_fd,
			       struct um_template_identity *first_blob)
{
	struct um_template_identity blob;
	int ret, child_pid, n;

	ret = assert_fork_safety(named_point);
	if (ret)
		return ret;

	/* The first call gave us blob already in *first_blob.  Use it
	 * for the first fork, then loop with fresh reads for each
	 * subsequent take.
	 */
	blob = *first_blob;

	os_snapshot_block_iter_signals();

	for (;;) {
		/*
		 * (A) PRE-FORK: kill every stub child in mm_list.  Without
		 * this, fork(2) aliases the master's stub pids into the
		 * forked-child UML's mm_list (which inherits via CoW), and
		 * the two halves race to drive the same stubs — corrupting
		 * the 16-bit offset fields in stub_data and SEGVing deep
		 * inside stub code.  See
		 * Documentation/virt/uml/redesign/06-sequencing/post-
		 * 2026-05-19-next-sprint/09-fork-server-PHASE2A-DESIGN.md
		 * §2 for the full hazard model.
		 */
		n = um_skas_teardown_all_stubs();
		if (n < 0) {
			pr_err("template_pause: stub teardown failed: %d\n", n);
			os_snapshot_unblock_iter_signals();
			return n;
		}
		pr_info("template_pause: torn down %d stub(s) pre-fork\n", n);

		/* (B) FORK */
		child_pid = os_template_pause_fork();
		if (child_pid < 0) {
			pr_err("template_pause: fork failed: %d\n", child_pid);
			(void)um_skas_respawn_all_stubs();
			os_snapshot_unblock_iter_signals();
			return child_pid;
		}

		if (child_pid == 0) {
			/* (D-child) — In the forked child, IMMEDIATELY
			 * disable preemption so the kernel's voluntary-
			 * schedule paths don't UML_LONGJMP into a CoW'd
			 * task's stale jmp_buf (whose saved RIP is a
			 * guest-userspace address that has no valid
			 * mapping in the child process).
			 *
			 * Bisect (2026-05-20) confirmed:
			 *   1. Without sched_worker_detach_other_tasks
			 *      called HERE (in the child, after fork),
			 *      the first schedule() call picks a stale-
			 *      jmp_buf task and panics.
			 *   2. The detach MUST happen before any function
			 *      call that might schedule (e.g. start_user-
			 *      space's wait_stub_done_seccomp futex wait).
			 */
			preempt_disable();
			/* Detach all inherited tasks so schedule() can't
			 * longjmp into a stale jmp_buf.
			 */
			sched_worker_detach_other_tasks();
			n = um_skas_respawn_all_stubs();
			if (n < 0) {
				pr_err("template_pause: child stub respawn failed: %d\n",
				       n);
				preempt_enable();
				return n;
			}
			um_snapshot_worker_init();
			preempt_enable();
			*first_blob = blob;
			return 0;
		}

		/* (C-parent) — RESPAWN master's own stubs.  The master keeps
		 * its UML task list intact; we just need fresh stub children.
		 */
		n = um_skas_respawn_all_stubs();
		if (n < 0) {
			pr_err("template_pause: parent stub respawn failed: %d\n",
			       n);
			os_snapshot_unblock_iter_signals();
			return n;
		}

		/* Report the new child's host pid via memfd[260:264]. */
		if (identity_fd >= 0) {
			int werr = os_template_pause_write_child_pid(
				identity_fd,
				sizeof(struct um_template_identity),
				child_pid);
			if (werr)
				pr_warn("template_pause: write child pid %d failed: %d\n",
					child_pid, werr);
		}

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
	pr_warn("template_pause: =fork requested at \"%s\" but kernel was built without CONFIG_UM_TEMPLATE_PAUSE_FORK; behaving as single-shot Phase 1a\n",
		named_point);
	return 0;
}

#endif /* CONFIG_UM_TEMPLATE_PAUSE_FORK */

int um_template_pause_enter(const char *named_point)
{
	struct um_template_identity blob;
	int identity_fd, ret;

	if (!named_point)
		named_point = "(null)";

	if (!template_pause_armed_flag) {
		pr_info("template_pause: enter(\"%s\") refused — um_template_pause not on cmdline\n",
			named_point);
		return -ENODEV;
	}

	identity_fd = os_template_pause_identity_fd();
	if (identity_fd < 0) {
		pr_info("template_pause: enter(\"%s\") — no UM_TEMPLATE_IDENTITY_FD; identity-blob channel disabled (err=%d)\n",
			named_point, identity_fd);
		identity_fd = -1;
	} else {
		pr_info("template_pause: enter(\"%s\") — identity_fd=%d\n",
			named_point, identity_fd);
	}

	ret = one_pause_cycle(named_point, identity_fd, &blob);
	if (ret)
		return ret;

	if (template_pause_fork_armed_flag)
		return fork_on_resume_loop(named_point, identity_fd, &blob);

	return 0;
}
EXPORT_SYMBOL_GPL(um_template_pause_enter);

/*
 * /proc/um/template_pause — write-only trigger.  Writing any non-
 * empty string invokes um_template_pause_enter() with that string as
 * the named point.  The write() blocks across SIGSTOP/SIGCONT — the
 * caller's write(2) returns only after the supervisor SIGCONTs us.
 *
 * Phase 1b's supervisor doesn't strictly need this; the bootstrap
 * script in the master Umlfile's init phases is the typical caller.
 * Keeping a writable /proc entry separately makes the path test-
 * scriptable without a Umlfile.
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

	ret = um_template_pause_enter(name[0] ? name : "proc");
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
