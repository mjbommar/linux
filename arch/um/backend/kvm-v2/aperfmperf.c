// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — APERF/MPERF MSR passthrough toggle + probe.
 *
 * What this file is for:
 *
 *   The KVM uapi exposes a per-VM cap, KVM_CAP_X86_DISABLE_EXITS, with
 *   a KVM_X86_DISABLE_EXITS_APERFMPERF bit (1 << 4) that tells KVM to
 *   skip the rdmsr intercept on IA32_APERF (0xE7) and IA32_MPERF
 *   (0xE8) so guest reads pass through to hardware.  QEMU's
 *   -overcommit cpu-pm=on plumbs the HLT / MWAIT / PAUSE / CSTATE
 *   bits but not APERFMPERF, and libvirt does not surface a property
 *   for it either — so guests under QEMU+libvirt see zero on those
 *   MSRs even with CPUID advertising the feature.
 *
 *   The v2 backend is its own KVM userspace VMM (it does not go
 *   through QEMU), so it can issue the cap-enable directly.  This
 *   file:
 *
 *     1. Owns the runtime predicate kvm_v2_aperfmperf_enabled() that
 *        kvm_v2_vm_create() consults before issuing KVM_ENABLE_CAP.
 *     2. Parses the `kvm_v2_aperfmperf=on|off` boot param so an
 *        operator can flip the default without rebuilding.
 *     3. Exposes a debugfs read file
 *        /sys/kernel/debug/um/kvm_v2/aperf_mperf that issues
 *        native_read_msr() on 0xE7 / 0xE8 from the running UML
 *        kernel (which executes at CPL=0 inside the KVM guest).
 *        With the cap enabled, the rdmsr returns the host counters;
 *        without it, both fields read zero — exactly mirroring the
 *        QEMU bug Anderson hit so the selftest can prove the
 *        kernel-side path works.
 *
 * Constraints captured here so this file stays self-contained:
 *
 *   - The cap is per-VM and must be enabled before any vCPU is
 *     created.  kvm_v2_vm_create() runs before kvm_v2_vcpu_create();
 *     the predicate must therefore be queryable at that point and
 *     never change after.  We use a single static bool seeded by
 *     Kconfig default + cmdline override; no late writers exist.
 *
 *   - Host CPU must have X86_FEATURE_APERFMPERF for KVM to accept
 *     the bit.  We do NOT pre-probe here — KVM rejects the ioctl
 *     with -EINVAL and the caller logs + continues, which is the
 *     correct behavior (same observable state as
 *     CONFIG_..._APERFMPERF_PASSTHROUGH=n).
 *
 * See Documentation/virt/uml/aperf-mperf.rst for the operator-facing
 * usage doc and the QEMU/libvirt bridging context.
 */

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#include <asm/msr.h>

#include "kvm_v2_backend.h"

/*
 * Default state of the toggle.  Single bool — the predicate is read
 * exactly once from kvm_v2_vm_create() (and possibly by the debugfs
 * file from userspace introspection), well after the boot-param
 * parser has settled.  No locking required.
 */
static bool aperfmperf_enabled = true;

bool kvm_v2_aperfmperf_enabled(void)
{
	return aperfmperf_enabled;
}

/*
 * Boot-param parser.  Accepts `on` / `off` / `1` / `0` / `y` / `n`.
 * Unknown values warn and leave the default untouched — silent
 * acceptance of garbage would hide typos in production cmdlines.
 */
static int __init kvm_v2_aperfmperf_setup(char *str)
{
	if (!str || !*str) {
		/* bare `kvm_v2_aperfmperf` with no value = on */
		aperfmperf_enabled = true;
		return 1;
	}

	if (!strcmp(str, "on") || !strcmp(str, "1") || !strcmp(str, "y"))
		aperfmperf_enabled = true;
	else if (!strcmp(str, "off") || !strcmp(str, "0") || !strcmp(str, "n"))
		aperfmperf_enabled = false;
	else
		pr_warn("um: kvm-v2: ignoring unknown kvm_v2_aperfmperf=%s (expected on|off)\n",
			str);

	pr_info("um: kvm-v2: APERF/MPERF passthrough %s by cmdline\n",
		aperfmperf_enabled ? "ENABLED" : "DISABLED");
	return 1;
}
__setup("kvm_v2_aperfmperf=", kvm_v2_aperfmperf_setup);

/*
 * Debugfs probe.  Reads IA32_APERF (0xE7) and IA32_MPERF (0xE8) from
 * the UML kernel's CPU context, which IS the KVM guest's CPL=0
 * context.  Output format is line-oriented so the kselftest can grep
 * it without parsing tooling:
 *
 *     aperf=<u64>
 *     mperf=<u64>
 *     ratio_pct=<percentage of mperf>
 *     toggle=on|off
 *
 * The ratio is computed from the same snapshot read; with passthrough
 * disabled both values read zero and the ratio is reported as "n/a".
 *
 * IMPORTANT: the read happens inside the UML kernel.  On a real KVM
 * guest under the v2 backend that puts us at CPL=0 of the
 * VMX-non-root guest, which is exactly where the KVM trap would
 * apply.  On a seccomp backend (no KVM context for the guest at all),
 * the read goes to the actual host CPU because the UML kernel runs
 * directly on the host — useful as a smoke-test reference but does
 * NOT exercise the passthrough path.  The selftest gates on
 * `backend=force=kvm-v2` to avoid that confusion.
 */
#define KVM_V2_MSR_IA32_APERF	0x000000e7
#define KVM_V2_MSR_IA32_MPERF	0x000000e8

static int kvm_v2_aperfmperf_show(struct seq_file *s, void *v)
{
	u64 aperf = 0, mperf = 0;
	int rc_a, rc_m;

	/*
	 * native_read_msr_safe traps SMI/MCE-class faults.  On a host
	 * that lacks the feature AND KVM is passing through the rdmsr
	 * trap, the read can #GP; the _safe variant turns that into a
	 * non-zero return without nuking the seq_file dispatch.
	 */
	rc_a = native_read_msr_safe(KVM_V2_MSR_IA32_APERF, &aperf);
	rc_m = native_read_msr_safe(KVM_V2_MSR_IA32_MPERF, &mperf);

	seq_printf(s, "toggle=%s\n",
		   aperfmperf_enabled ? "on" : "off");
	seq_printf(s, "aperf_rc=%d\n", rc_a);
	seq_printf(s, "mperf_rc=%d\n", rc_m);
	seq_printf(s, "aperf=%llu\n", aperf);
	seq_printf(s, "mperf=%llu\n", mperf);
	if (mperf > 0)
		seq_printf(s, "ratio_pct=%llu\n",
			   (aperf * 100ULL) / mperf);
	else
		seq_puts(s, "ratio_pct=n/a\n");

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(kvm_v2_aperfmperf);

static int __init kvm_v2_aperfmperf_debugfs_init(void)
{
	struct dentry *dir;

	dir = debugfs_lookup("um/kvm_v2", NULL);
	if (!dir) {
		dir = debugfs_create_dir("um/kvm_v2", NULL);
		if (IS_ERR(dir)) {
			pr_warn("um: kvm-v2: debugfs_create_dir(um/kvm_v2) failed (%ld) — aperf_mperf probe unavailable\n",
				PTR_ERR(dir));
			return 0;	/* non-fatal */
		}
	}

	debugfs_create_file("aperf_mperf", 0444, dir, NULL,
			    &kvm_v2_aperfmperf_fops);
	return 0;
}
late_initcall(kvm_v2_aperfmperf_debugfs_init);
