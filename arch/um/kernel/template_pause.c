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
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <asm/um-template-pause.h>

#include <os.h>

/*
 * Armed via "um_template_pause" on the kernel cmdline.  Off by
 * default: a non-pool boot must never accidentally raise SIGSTOP on
 * itself just because a stray /proc write lands.  The proc write
 * handler checks this flag and returns -ENODEV when unarmed.
 */
static bool template_pause_armed_flag __read_mostly;

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
	pr_info("template_pause: armed via kernel cmdline\n");
	return 1;
}
__setup("um_template_pause", template_pause_setup);

bool um_template_pause_armed(void)
{
	return template_pause_armed_flag;
}
EXPORT_SYMBOL_GPL(um_template_pause_armed);

int um_template_pause_enter(const char *named_point)
{
	struct um_template_identity blob;
	int identity_fd, ret;
	ssize_t n;

	if (!named_point)
		named_point = "(null)";

	if (!template_pause_armed_flag) {
		pr_info("template_pause: enter(\"%s\") refused — um_template_pause not on cmdline\n",
			named_point);
		return -ENODEV;
	}

	identity_fd = os_template_pause_identity_fd();
	if (identity_fd < 0) {
		/* No identity channel — pool master before fork or a
		 * standalone pause for testing.  Pause still proceeds;
		 * the supervisor may not even intend to write a blob.
		 */
		pr_info("template_pause: enter(\"%s\") — no UM_TEMPLATE_IDENTITY_FD; identity-blob channel disabled (err=%d)\n",
			named_point, identity_fd);
		identity_fd = -1;
	} else {
		pr_info("template_pause: enter(\"%s\") — identity_fd=%d\n",
			named_point, identity_fd);
	}

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

	if (identity_fd < 0)
		return 0;

	memset(&blob, 0, sizeof(blob));
	n = os_template_pause_read_identity(identity_fd, &blob, sizeof(blob));
	if (n < 0) {
		pr_err("template_pause: identity read failed: %zd (continuing without identity)\n",
		       n);
		return 0;
	}
	if ((size_t)n < sizeof(blob.magic) + sizeof(blob.version)) {
		pr_warn("template_pause: identity blob too short (%zd bytes); skipping validation\n",
			n);
		return 0;
	}
	if (blob.magic != UM_TEMPLATE_IDENTITY_MAGIC) {
		pr_err("template_pause: identity blob magic 0x%08x != expected 0x%08x\n",
		       blob.magic, UM_TEMPLATE_IDENTITY_MAGIC);
		return -EILSEQ;
	}
	if (blob.version != UM_TEMPLATE_IDENTITY_VERSION) {
		pr_err("template_pause: identity blob version %u not supported (expected %u)\n",
		       blob.version, UM_TEMPLATE_IDENTITY_VERSION);
		return -EPROTONOSUPPORT;
	}

	/* Phase 1a: log + return.  Phase 2 applies the identity. */
	blob.instance_name[sizeof(blob.instance_name) - 1] = '\0';
	blob.tap_name[sizeof(blob.tap_name) - 1] = '\0';
	blob.ipv4_cidr[sizeof(blob.ipv4_cidr) - 1] = '\0';
	blob.ipv4_gateway[sizeof(blob.ipv4_gateway) - 1] = '\0';
	pr_info("template_pause: identity instance=\"%s\" mac=%02x:%02x:%02x:%02x:%02x:%02x tap=\"%s\" ipv4=\"%s\" gw=\"%s\"\n",
		blob.instance_name,
		blob.mac_addr[0], blob.mac_addr[1], blob.mac_addr[2],
		blob.mac_addr[3], blob.mac_addr[4], blob.mac_addr[5],
		blob.tap_name, blob.ipv4_cidr, blob.ipv4_gateway);

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
