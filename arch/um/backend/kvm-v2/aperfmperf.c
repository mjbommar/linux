// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend APERF/MPERF MSR passthrough control.
 *
 * KVM exposes APERF/MPERF passthrough through the per-VM
 * KVM_CAP_X86_DISABLE_EXITS cap. Setting
 * KVM_X86_DISABLE_EXITS_APERFMPERF lets guest CPL0 rdmsr instructions
 * for IA32_APERF and IA32_MPERF pass through to hardware instead of
 * returning the default emulated zero.
 *
 * This file owns the boot-time toggle, the predicate used by
 * kvm_v2_vm_create(), the recorded KVM_ENABLE_CAP result, and the
 * debugfs status file. It does not execute rdmsr from UML kernel code:
 * UML itself runs as a host userspace process, while the cap affects code
 * executing inside the KVM guest context.
 *
 * The cap must be enabled before any vCPU is created. The predicate is
 * therefore seeded by Kconfig and the UML cmdline parser and is not
 * mutated after VM creation starts. Host APERF/MPERF support is left to
 * KVM's capability check; rejection leaves the default zero-counter
 * behavior in place and is reported through the status file.
 *
 * See Documentation/virt/uml/aperf-mperf.rst for user-facing usage and
 * Documentation/virt/uml/examples/aperf-mperf/README.md for an in-guest
 * example.
 */

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/cpufeatures.h>
#include <asm/cpufeature.h>

#include <init.h>		/* __uml_setup */

#include "kvm_v2_backend.h"

/*
 * Default state of the toggle. Single bool; predicate is read at
 * vm_create time after the boot-param parser settles and once more per
 * debugfs open. No locking required.
 */
static bool aperfmperf_enabled = true;

/*
 * Outcome record updated by kvm_v2_vm_create() right after the
 * KVM_ENABLE_CAP ioctl returns. Read by the debugfs probe so callers can
 * distinguish:
 *
 *   attempted = 0:  passthrough disabled (Kconfig=n compile-out or
 *                   boot param off; vm_create skipped the ioctl)
 *   attempted = 1, ioctl_rc = 0:
 *                   passthrough enabled, KVM accepted the cap
 *   attempted = 1, ioctl_rc < 0:
 *                   passthrough requested, KVM rejected the cap
 *                   (usually because the host CPU lacks
 *                   X86_FEATURE_APERFMPERF or KVM's
 *                   kvm_get_allowed_disable_exits masked it off)
 */
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

/*
 * Consumed by kvm_v2_install_per_vcpu_gadget_state() to
 * decide whether to set the per-vCPU APERF_CAP byte that the
 * h_aperfmperf gadget body checks before issuing rdmsr.
 *
 * Returns true only when vm_create both attempted the cap-enable
 * and KVM accepted it. This is the definitive "rdmsr from guest
 * CPL=0 will pass through to hardware" predicate; using just the
 * toggle would mis-arm the gadget on hosts that lack
 * X86_FEATURE_APERFMPERF (KVM rejects the ioctl in that case).
 */
bool kvm_v2_aperfmperf_cap_active(void)
{
	return ioctl_attempted && ioctl_rc == 0;
}

/*
 * Boot-param parser. Accepts on/off/1/0/y/n; bare key without value
 * defaults to on. Unknown values warn and leave the default
 * untouched; silently accepting garbage would hide cmdline typos.
 *
 * Registered as __uml_setup because init_backend() runs from linux_main()
 * before start_kernel() and parse_args(). Neither __setup nor early_param
 * hooks have fired by that point. uml_check_setup() has already iterated
 * __uml_setup entries, so the predicate is correct by the time
 * kvm_v2_vm_create() consults it.
 */
static int __init kvm_v2_aperfmperf_uml_setup(char *line, int *add)
{
	*add = 0;

	if (!line || !*line) {
		/* bare kvm_v2_aperfmperf with no value => on */
		aperfmperf_enabled = true;
		return 0;
	}

	if (!strcmp(line, "on") || !strcmp(line, "1") || !strcmp(line, "y"))
		aperfmperf_enabled = true;
	else if (!strcmp(line, "off") || !strcmp(line, "0") ||
		 !strcmp(line, "n"))
		aperfmperf_enabled = false;
	else
		pr_warn("um: kvm-v2: ignoring unknown kvm_v2_aperfmperf=%s (expected on|off)\n",
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

/*
 * Debugfs status probe. Reports the architectural plumbing; does not
 * execute rdmsr. Output format is line-oriented so userspace probes can
 * parse it without libc:
 *
 *     toggle=on|off                # final state after Kconfig + cmdline
 *     ioctl_attempted=0|1          # did vm_create issue KVM_ENABLE_CAP?
 *     ioctl_rc=<int>               # return code, 0 on success
 *     host_feature_aperfmperf=0|1  # boot_cpu_has(X86_FEATURE_APERFMPERF)
 *     status=enabled|disabled|rejected|host_no_feature
 *
 * The status line is the human-readable verdict:
 *
 *   enabled         - ioctl_attempted=1 ioctl_rc=0:
 *                     KVM accepted; APERF/MPERF rdmsr at guest CPL=0
 *                     will now pass through to hardware
 *   rejected        - ioctl_attempted=1 ioctl_rc<0 and host has feature:
 *                     KVM refused the cap.
 *   host_no_feature - ioctl_attempted=1 ioctl_rc<0 and host lacks
 *                     X86_FEATURE_APERFMPERF (kvm_get_allowed_disable_exits
 *                     masks the bit off in that case)
 *   disabled        - ioctl_attempted=0:
 *                     boot param said off, or Kconfig compiled it out
 */
static int kvm_v2_aperfmperf_show(struct seq_file *s, void *v)
{
	bool host_has = boot_cpu_has(X86_FEATURE_APERFMPERF);
	const char *status;

	if (!ioctl_attempted)
		status = "disabled";
	else if (ioctl_rc == 0)
		status = "enabled";
	else if (!host_has)
		status = "host_no_feature";
	else
		status = "rejected";

	seq_printf(s, "toggle=%s\n", aperfmperf_enabled ? "on" : "off");
	seq_printf(s, "ioctl_attempted=%d\n", ioctl_attempted ? 1 : 0);
	seq_printf(s, "ioctl_rc=%d\n", ioctl_rc);
	seq_printf(s, "host_feature_aperfmperf=%d\n", host_has ? 1 : 0);
	seq_printf(s, "status=%s\n", status);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(kvm_v2_aperfmperf);

static int __init kvm_v2_aperfmperf_debugfs_init(void)
{
	struct dentry *um_dir, *kvm_v2_dir;

	/*
	 * debugfs_create_dir does not split a slashed path; it makes
	 * the literal name a single entry. Build the hierarchy in two
	 * steps, reusing an existing "um" dir if some other subsystem
	 * has already created it.
	 */
	um_dir = debugfs_lookup("um", NULL);
	if (!um_dir) {
		um_dir = debugfs_create_dir("um", NULL);
		if (IS_ERR(um_dir)) {
			pr_warn("um: kvm-v2: debugfs_create_dir(um) failed (%ld); aperf_mperf probe unavailable\n",
				PTR_ERR(um_dir));
			return 0;
		}
	}

	kvm_v2_dir = debugfs_lookup("kvm_v2", um_dir);
	if (!kvm_v2_dir) {
		kvm_v2_dir = debugfs_create_dir("kvm_v2", um_dir);
		if (IS_ERR(kvm_v2_dir)) {
			pr_warn("um: kvm-v2: debugfs_create_dir(um/kvm_v2) failed (%ld); aperf_mperf probe unavailable\n",
				PTR_ERR(kvm_v2_dir));
			return 0;
		}
	}

	debugfs_create_file("aperf_mperf", 0444, kvm_v2_dir, NULL,
			    &kvm_v2_aperfmperf_fops);
	return 0;
}
late_initcall(kvm_v2_aperfmperf_debugfs_init);
