// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend APERF/MPERF MSR passthrough control.
 *
 * The boot option controls whether VM creation asks KVM to pass guest
 * IA32_APERF/IA32_MPERF reads through to hardware. The cap must be
 * enabled before any vCPU exists, so this file keeps the pre-VM toggle,
 * records the KVM_ENABLE_CAP result, and reports the final state through
 * debugfs. It does not execute rdmsr from UML host code.
 */

#include <linux/dcache.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#include <asm/cpufeatures.h>
#include <asm/cpufeature.h>

#include <init.h>		/* __uml_setup */

#include "kvm_v2_backend.h"

enum kvm_v2_aperfmperf_status {
	KVM_V2_APERFMPERF_DISABLED,
	KVM_V2_APERFMPERF_ENABLED,
	KVM_V2_APERFMPERF_HOST_NO_FEATURE,
	KVM_V2_APERFMPERF_REJECTED,
};

/* Set by Kconfig default and kvm_v2_aperfmperf= before VM creation. */
static bool aperfmperf_enabled = true;

/* Outcome of the VM-create KVM_ENABLE_CAP attempt. */
static bool ioctl_attempted;
static int  ioctl_rc;

bool kvm_v2_aperfmperf_enabled(void)
{
	return aperfmperf_enabled;
}

void kvm_v2_aperfmperf_record_ioctl(int rc)
{
	ioctl_attempted = true;
	ioctl_rc = rc;
}

bool kvm_v2_aperfmperf_cap_active(void)
{
	return ioctl_attempted && ioctl_rc == 0;
}

static int __init kvm_v2_aperfmperf_uml_setup(char *line, int *add)
{
	bool enabled;

	*add = 0;

	if (!line || !*line) {
		aperfmperf_enabled = true;
		return 0;
	}

	if (!kstrtobool(line, &enabled)) {
		aperfmperf_enabled = enabled;
		return 0;
	}

	pr_warn("um: kvm-v2: ignoring unknown kvm_v2_aperfmperf=%s (expected boolean)\n",
		line);

	return 0;
}

__uml_setup("kvm_v2_aperfmperf=", kvm_v2_aperfmperf_uml_setup,
	    "kvm_v2_aperfmperf=<on|off>\n"
	    "    Override the Kconfig default for the KVM v2 backend's\n"
	    "    APERF/MPERF MSR passthrough cap (KVM_CAP_X86_DISABLE_EXITS /\n"
	    "    KVM_X86_DISABLE_EXITS_APERFMPERF).  'on' tells vm_create to\n"
	    "    issue KVM_ENABLE_CAP so rdmsr 0xE7/0xE8 at guest CPL=0 reads\n"
	    "    host counters; 'off' skips the ioctl and KVM keeps emulating\n"
	    "    the MSRs as zero.  See Documentation/virt/uml/aperf-mperf.rst.\n"
	    "\n");

static enum kvm_v2_aperfmperf_status
kvm_v2_aperfmperf_status(bool host_has)
{
	if (!ioctl_attempted)
		return KVM_V2_APERFMPERF_DISABLED;
	if (ioctl_rc == 0)
		return KVM_V2_APERFMPERF_ENABLED;
	if (!host_has)
		return KVM_V2_APERFMPERF_HOST_NO_FEATURE;
	return KVM_V2_APERFMPERF_REJECTED;
}

static const char *
kvm_v2_aperfmperf_status_name(enum kvm_v2_aperfmperf_status status)
{
	switch (status) {
	case KVM_V2_APERFMPERF_DISABLED:
		return "disabled";
	case KVM_V2_APERFMPERF_ENABLED:
		return "enabled";
	case KVM_V2_APERFMPERF_HOST_NO_FEATURE:
		return "host_no_feature";
	case KVM_V2_APERFMPERF_REJECTED:
		return "rejected";
	}
	return "unknown";
}

static int kvm_v2_aperfmperf_show(struct seq_file *s, void *v)
{
	bool host_has = boot_cpu_has(X86_FEATURE_APERFMPERF);
	enum kvm_v2_aperfmperf_status status;

	status = kvm_v2_aperfmperf_status(host_has);

	seq_printf(s, "toggle=%s\n", aperfmperf_enabled ? "on" : "off");
	seq_printf(s, "ioctl_attempted=%d\n", ioctl_attempted ? 1 : 0);
	seq_printf(s, "ioctl_rc=%d\n", ioctl_rc);
	seq_printf(s, "host_feature_aperfmperf=%d\n", host_has ? 1 : 0);
	seq_printf(s, "status=%s\n", kvm_v2_aperfmperf_status_name(status));

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(kvm_v2_aperfmperf);

static struct dentry *kvm_v2_debugfs_get_dir(const char *name,
					     struct dentry *parent,
					     bool *needs_dput)
{
	struct dentry *dir;

	*needs_dput = false;

	dir = debugfs_lookup(name, parent);
	if (IS_ERR(dir))
		return NULL;
	if (dir) {
		*needs_dput = true;
		return dir;
	}

	dir = debugfs_create_dir(name, parent);
	if (IS_ERR(dir)) {
		pr_warn("um: kvm-v2: debugfs_create_dir(%s) failed (%ld); aperf_mperf probe unavailable\n",
			name, PTR_ERR(dir));
		return NULL;
	}

	return dir;
}

static int __init kvm_v2_aperfmperf_debugfs_init(void)
{
	struct dentry *um_dir, *kvm_v2_dir;
	bool put_um_dir, put_kvm_v2_dir;

	um_dir = kvm_v2_debugfs_get_dir("um", NULL, &put_um_dir);
	if (!um_dir)
		return 0;

	kvm_v2_dir = kvm_v2_debugfs_get_dir("kvm_v2", um_dir,
					    &put_kvm_v2_dir);
	if (!kvm_v2_dir)
		goto out_um;

	debugfs_create_file("aperf_mperf", 0444, kvm_v2_dir, NULL,
			    &kvm_v2_aperfmperf_fops);
	if (put_kvm_v2_dir)
		dput(kvm_v2_dir);
out_um:
	if (put_um_dir)
		dput(um_dir);
	return 0;
}
late_initcall(kvm_v2_aperfmperf_debugfs_init);
