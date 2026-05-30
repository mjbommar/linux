// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — APERF/MPERF MSR passthrough toggle + status probe.
 *
 * What this file is for:
 *
 *   The KVM uapi exposes a per-VM cap, KVM_CAP_X86_DISABLE_EXITS, with
 *   a KVM_X86_DISABLE_EXITS_APERFMPERF bit (1 << 4) that tells KVM to
 *   skip the rdmsr intercept on IA32_APERF (0xE7) and IA32_MPERF
 *   (0xE8) so guest reads pass through to hardware.  QEMU's
 *   -overcommit cpu-pm=on plumbs HLT / MWAIT / PAUSE / CSTATE but not
 *   APERFMPERF; libvirt does not surface a property for it.  Guests
 *   under QEMU+libvirt therefore see zero on those MSRs even when
 *   CPUID advertises the feature.
 *
 *   The v2 backend is its own KVM userspace VMM (it does not go
 *   through QEMU), so it can issue the cap-enable directly.  This
 *   file owns:
 *
 *     1. The runtime predicate kvm_v2_aperfmperf_enabled() that
 *        kvm_v2_vm_create() consults before issuing KVM_ENABLE_CAP.
 *     2. The kvm_v2_aperfmperf={on,off,1,0,y,n} boot-param parser.
 *     3. The kvm_v2_aperfmperf_ioctl_rc / _attempted record so the
 *        probe can report the exact outcome of the ioctl.
 *     4. A debugfs read file
 *        /sys/kernel/debug/um/kvm_v2/aperf_mperf that reports the
 *        STATUS of the architectural plumbing (toggle on/off, the
 *        ioctl return code, host CPU feature availability) so a
 *        userspace test can verify the cap was actually plumbed.
 *
 * IMPORTANT — what this file does NOT do:
 *
 *   It does NOT execute rdmsr from within the UML kernel.  UML's
 *   kernel code runs as a host userspace process at host CPL=3;
 *   rdmsr requires CPL=0 and would #GP every time.  The disable-
 *   exits bit only affects rdmsr executed at GUEST CPL=0 inside the
 *   KVM guest (i.e., real guest kernel code running in VMX non-root
 *   mode).  For UML, the only code that runs at guest CPL=0 is the
 *   LSTAR gadget and the IDT/exception stubs.  Adding rdmsr to the
 *   gadget is possible (a future custom NR could expose APERF/MPERF
 *   to guest userspace) but is out of scope for this commit.
 *
 *   The architectural value is real: UML's kvm-v2 is now a faithful
 *   KVM userspace VMM with respect to APERFMPERF disable-exits.
 *   That fills the cap-plumbing gap in the upstream QEMU+libvirt
 *   userspace VMM and provides a reproducer for the cap-plumbing
 *   logic.  Actually consuming the counters from guest code is a
 *   separate problem (see the README under
 *   Documentation/virt/uml/examples/aperf-mperf/ for the bridging
 *   notes).
 *
 * Constraints captured here so this file stays self-contained:
 *
 *   - The cap is per-VM and must be enabled before any vCPU is
 *     created.  kvm_v2_vm_create() runs before kvm_v2_vcpu_create();
 *     the predicate must therefore be queryable at that point and
 *     never change after.  We use a single static bool seeded by
 *     Kconfig default + cmdline override; no late writers.
 *
 *   - Host CPU must have X86_FEATURE_APERFMPERF for KVM to accept
 *     the bit.  We do NOT pre-probe here — KVM rejects the ioctl
 *     with -EINVAL and the caller logs + continues, which is the
 *     correct behavior (same observable state as
 *     CONFIG_..._APERFMPERF_PASSTHROUGH=n).  The status probe
 *     surfaces the ioctl rc so the test can distinguish "feature
 *     was enabled by ops but host rejected" from "ops never asked".
 *
 * See Documentation/virt/uml/aperf-mperf.rst for the operator-facing
 * usage doc and Documentation/virt/uml/examples/aperf-mperf/README.md
 * for the runnable example and captured output.
 */

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#include <asm/cpufeatures.h>
#include <asm/cpufeature.h>

#include <init.h>		/* __uml_setup */

#include "kvm_v2_backend.h"

/*
 * Default state of the toggle.  Single bool — predicate is read at
 * vm_create time (well after the boot-param parser settles) and
 * once more per debugfs open.  No locking required.
 */
static bool aperfmperf_enabled = true;

/*
 * Outcome record updated by kvm_v2_vm_create() right after the
 * KVM_ENABLE_CAP ioctl returns.  Read by the debugfs probe so the
 * test can distinguish:
 *
 *   attempted = 0:  passthrough disabled (Kconfig=n compile-out or
 *                   boot param off — vm_create skipped the ioctl)
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
 * Boot-param parser.  Accepts on/off/1/0/y/n; bare key without value
 * defaults to on.  Unknown values warn and leave the default
 * untouched — silently accepting garbage would hide cmdline typos.
 *
 * Registered as __uml_setup (NOT __setup or early_param) because
 * init_backend() runs from linux_main() in arch/um/kernel/um_arch.c
 * BEFORE start_kernel() and parse_args().  Neither __setup nor
 * early_param hooks have fired by that point.  __uml_setup is the
 * UML-specific cmdline parser registered with init.h's section
 * machinery (extern struct uml_param __uml_setup_start);
 * uml_check_setup() iterates it from check_environ() during
 * linux_main, so the predicate is correct by the time
 * kvm_v2_vm_create() consults it.  Same path uml_backend_config()
 * and uml_seccomp_config() use; see start_up.c.
 */
static int __init kvm_v2_aperfmperf_uml_setup(char *line, int *add)
{
	*add = 0;

	if (!line || !*line) {
		/* bare `kvm_v2_aperfmperf` with no value => on */
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
"    KVM_X86_DISABLE_EXITS_APERFMPERF).  `on' tells vm_create to\n"
"    issue KVM_ENABLE_CAP so rdmsr 0xE7/0xE8 at guest CPL=0 reads\n"
"    host counters; `off' skips the ioctl and KVM keeps emulating\n"
"    the MSRs as zero.  See Documentation/virt/uml/aperf-mperf.rst.\n"
"\n"
);

/*
 * Debugfs status probe.  Reports the architectural plumbing — does
 * NOT execute rdmsr (see file-scope comment for why that would be
 * wrong).  Output format is line-oriented so the freestanding demo
 * (Documentation/virt/uml/examples/aperf-mperf/) can parse it
 * without libc:
 *
 *     toggle=on|off                # final state after Kconfig + cmdline
 *     ioctl_attempted=0|1          # did vm_create issue KVM_ENABLE_CAP?
 *     ioctl_rc=<int>               # return code, 0 on success
 *     host_feature_aperfmperf=0|1  # boot_cpu_has(X86_FEATURE_APERFMPERF)
 *     status=enabled|disabled|rejected|host_no_feature
 *
 * The `status` line is the operator-readable verdict:
 *
 *   enabled         — ioctl_attempted=1 ioctl_rc=0:
 *                     KVM accepted; APERF/MPERF rdmsr at guest CPL=0
 *                     will now pass through to hardware
 *   rejected        — ioctl_attempted=1 ioctl_rc<0 AND host has feature:
 *                     KVM refused the cap (rare; usually a SMT-RSB
 *                     mitigation overlap — see x86.c:6817)
 *   host_no_feature — ioctl_attempted=1 ioctl_rc<0 AND host lacks
 *                     X86_FEATURE_APERFMPERF (kvm_get_allowed_disable_exits
 *                     masks the bit off in that case)
 *   disabled        — ioctl_attempted=0:
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
	 * debugfs_create_dir does NOT split a slashed path — it makes
	 * the literal name a single entry.  Build the hierarchy in two
	 * steps, reusing an existing "um" dir if some other subsystem
	 * has already created it.
	 */
	um_dir = debugfs_lookup("um", NULL);
	if (!um_dir) {
		um_dir = debugfs_create_dir("um", NULL);
		if (IS_ERR(um_dir)) {
			pr_warn("um: kvm-v2: debugfs_create_dir(um) failed (%ld) — aperf_mperf probe unavailable\n",
				PTR_ERR(um_dir));
			return 0;	/* non-fatal */
		}
	}

	kvm_v2_dir = debugfs_lookup("kvm_v2", um_dir);
	if (!kvm_v2_dir) {
		kvm_v2_dir = debugfs_create_dir("kvm_v2", um_dir);
		if (IS_ERR(kvm_v2_dir)) {
			pr_warn("um: kvm-v2: debugfs_create_dir(um/kvm_v2) failed (%ld) — aperf_mperf probe unavailable\n",
				PTR_ERR(kvm_v2_dir));
			return 0;	/* non-fatal */
		}
	}

	debugfs_create_file("aperf_mperf", 0444, kvm_v2_dir, NULL,
			    &kvm_v2_aperfmperf_fops);
	return 0;
}
late_initcall(kvm_v2_aperfmperf_debugfs_init);
