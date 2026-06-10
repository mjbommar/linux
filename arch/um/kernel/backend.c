// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend arbiter.
 *
 * Single source of truth for which backend is active. Picks the
 * compiled-in backend (arch/um/backend/<kind>/) and exposes it as
 * the global um_backend pointer used by the dispatch macro in
 * arch/um/include/shared/backend.h.
 *
 * Resolution:
 *   1. SECCOMP_ONLY Kconfig pins seccomp at compile time.
 *   2. DYNAMIC compiles seccomp and optional KVM v2; init_backend()
 *      picks the requested backend when it is available.
 *
 * After init_backend() returns, using_seccomp always matches
 * um_backend->kind == UM_BACKEND_KIND_SECCOMP. Both representations
 * of "which backend is active" are kept in sync from this point on.
 *
 * The validation at the top: every backend's required dispatch ops
 * must be non-NULL. Optional ops can be NULL, but a NULL optional op
 * still crashes if dispatched in dynamic mode; the dispatch macro does
 * not synthesize -ENOSYS. See backend-contract.rst.
 *
 * The ptrace backend is no longer built by this tree.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/bug.h>
#include <asm/backend.h>

const struct um_backend_ops *um_backend;
EXPORT_SYMBOL_GPL(um_backend);

static void validate_required_ops(const struct um_backend_ops *ops)
{
	if (!ops->vcpu_run || !ops->mm_region_added || !ops->mm_region_removed ||
	    !ops->context_switch || !ops->read_clock_ns)
		panic("um: backend %s has NULL required op", ops->name);
}

/*
 * Pick a backend in DYNAMIC mode. Resolution:
 *   backend=force=<kind> panics if kind is unbuilt.
 *   backend=<kind> (non-force) is a preference; resolves to
 *      seccomp if the requested kind isn't built.
 *   backend=auto (default) selects seccomp.
 *
 * Seccomp is the default resident backend. KVM v2 can be selected
 * explicitly when it is built.
 */
#ifdef CONFIG_UM_BACKEND_DYNAMIC
static const struct um_backend_ops * __init pick_dynamic_backend(void)
{
	enum um_backend_kind want = (enum um_backend_kind)backend_arg_requested;
	bool force = backend_arg_force != 0;

	if (want == UM_BACKEND_KIND_PTRACE) {
		if (force)
			panic("um: backend=force=ptrace requested but ptrace backend is not built");
		pr_warn("um: backend=ptrace requested but ptrace backend is not built; falling back to seccomp\n");
	} else if (want == UM_BACKEND_KIND_SECCOMP) {
		if (IS_ENABLED(CONFIG_UM_BACKEND_SECCOMP) && using_seccomp)
			return &um_backend_seccomp_ops;
		if (force)
			panic("um: backend=force=seccomp but seccomp not compiled in or probe failed");
	} else if (want == UM_BACKEND_KIND_KVM) {
#ifdef CONFIG_UM_BACKEND_KVM_V2
		/*
		 * Probe runs in init_backend() after this returns and panics
		 * on failure regardless of force, so we only return KVM v2
		 * when forced. A non-force preference should still fall
		 * through to seccomp on a host without /dev/kvm.
		 */
		if (force)
			return &um_backend_kvm_v2_ops;
		pr_info("um: backend=kvm preference noted; falling back to seccomp unless force=kvm is used\n");
#else
		if (force)
			panic("um: backend=force=kvm requested but UM_BACKEND_KVM_V2 is not built");
		pr_warn("um: backend=kvm requested but UM_BACKEND_KVM_V2 not built; falling back\n");
#endif
	}

	/* Default dynamic path: seccomp is the resident fallback backend. */
	if (using_seccomp)
		return &um_backend_seccomp_ops;
	panic("um: dynamic backend resolution failed; seccomp probe did not succeed and no other backend is built");
}
#endif

enum um_backend_kind __init init_backend(const struct um_backend_args *args)
{
	(void)args;	/* parser writes backend_arg_* directly via __uml_setup */

#if defined(CONFIG_UM_BACKEND_SECCOMP_ONLY)
	if (backend_arg_requested == UM_BACKEND_KIND_PTRACE && backend_arg_force)
		panic("um: backend=force=ptrace requested but ptrace backend is not built");
	um_backend = &um_backend_seccomp_ops;
	using_seccomp = 1;
#elif defined(CONFIG_UM_BACKEND_DYNAMIC)
	um_backend = pick_dynamic_backend();
#else
	panic("um: no backend selected by Kconfig");
#endif

	validate_required_ops(um_backend);

	/*
	 * Dispatch the contract's cold lifecycle ops. probe() here is
	 * the "this backend can still run on this host" recheck; the
	 * arbiter already trusted os_early_checks()'s probe result via
	 * using_seccomp, but the ops-table probe is what
	 * backend-contract.rst advertises, so invoking it keeps the
	 * contract authoritative. init() sets up per-backend state.
	 * Both ops panic on failure per the contract.
	 */
	if (um_backend->probe) {
		int rc = um_backend->probe();

		if (rc)
			panic("um: backend %s probe failed: %d",
			      um_backend->name, rc);
	}
	if (um_backend->init) {
		int rc = um_backend->init(args);

		if (rc)
			panic("um: backend %s init failed: %d",
			      um_backend->name, rc);
	}

	pr_info("um: backend = %s (contract v%u)\n",
		um_backend->name, um_backend->contract_version);
	return um_backend->kind;
}
