// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — stub.
 *
 * Real implementation arrives in phases per
 * Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
 * 26-v2-implementation-plan.md after the prerequisite ARCH=um core
 * refactors land (memo 25 Part 2). Until then this TU compiles only
 * a no-op init and prints a banner so an EXPERT user who selected
 * CONFIG_UM_BACKEND_KVM_V2=y knows what they are signing up for.
 *
 * The v2 backend ops table is NOT registered with init_backend()
 * yet; the dispatch layer cannot resolve to v2 today. Boot still
 * picks seccomp or ptrace via DYNAMIC; v1 is archived
 * (arch/um/backend/kvm-v1-archive/, depends on BROKEN).
 */

#include <linux/init.h>
#include <linux/printk.h>

static int __init kvm_v2_stub_banner(void)
{
	pr_info("um: kvm-v2: stub init (no ops registered yet — see memo 26)\n");
	return 0;
}
arch_initcall(kvm_v2_stub_banner);
