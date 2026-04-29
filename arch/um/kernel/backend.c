// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend arbiter.
 *
 * Single source of truth for which backend is active. Picks the
 * compiled-in backend (arch/um/backend/<kind>/) and exposes it as
 * the global `um_backend` pointer used by the dispatch macro in
 * arch/um/include/shared/backend.h.
 *
 * Resolution:
 *   1. SECCOMP_ONLY Kconfig pins seccomp at compile time.
 *   2. DYNAMIC compiles seccomp; init_backend() picks it (v2 KVM
 *      will join this branch when memo 26 Phase A.1 wires it in).
 *
 * After init_backend() returns, `using_seccomp` always matches
 * `um_backend->kind == UM_BACKEND_KIND_SECCOMP`. Both representations
 * of "which backend is active" are kept in sync from this point on.
 *
 * The validation at the top: every backend's HOT ops must be
 * non-NULL. Cold ops can be NULL pending in-tree consumers (e.g.
 * read/write_guest_regs awaiting KGDB / C-11) but a NULL cold op
 * still crashes if dispatched in dynamic mode — the dispatch macro
 * does not synthesize -ENOSYS. See backend-contract.rst.
 *
 * The ptrace backend was removed in memo 25 refactor 11 (B); the
 * archived implementation lives at the kvm-v1-archive-20260428 tag's
 * arch/um/backend/ptrace/ for anyone who wants to revive it.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/bug.h>
#include <asm/backend.h>

const struct um_backend_ops *um_backend;
EXPORT_SYMBOL_GPL(um_backend);

#ifdef CONFIG_UM_BACKEND_SECCOMP
extern const struct um_backend_ops um_backend_seccomp_ops;
#endif
#ifdef CONFIG_UM_BACKEND_KVM_V2
extern const struct um_backend_ops um_backend_kvm_v2_ops;
#endif

/* Set by arch/um/os-Linux/start_up.c::os_early_checks() based on
 * the host probe. init_backend reads it for DYNAMIC selection.
 */
extern int using_seccomp;

/* Set by arch/um/os-Linux/start_up.c::uml_backend_config (the
 * `backend=` boot param parser). 0 means "no preference, use
 * Kconfig default + probe".
 */
extern int backend_arg_requested;
extern int backend_arg_force;

static void validate_hot_ops(const struct um_backend_ops *ops)
{
	if (!ops->vcpu_run || !ops->mm_region_added || !ops->mm_region_removed ||
	    !ops->context_switch || !ops->read_clock_ns)
		panic("um: backend %s has NULL HOT op (vcpu_run=%p mm_region_added=%p mm_region_removed=%p context_switch=%p read_clock_ns=%p)",
		      ops->name,
		      ops->vcpu_run, ops->mm_region_added, ops->mm_region_removed,
		      ops->context_switch, ops->read_clock_ns);
}

/*
 * Pick a backend in DYNAMIC mode. Resolution:
 *   `backend=force=<kind>` panics if kind is unbuilt.
 *   `backend=<kind>` (non-force) is a preference; falls through to
 *      seccomp if the requested kind isn't built.
 *   `backend=auto` (default) selects seccomp.
 *
 * Today only seccomp is built. ptrace was removed in memo 25
 * refactor 11; v2 KVM joins this resolver in memo 26 Phase A.1.
 */
#ifdef CONFIG_UM_BACKEND_DYNAMIC
static const struct um_backend_ops * __init pick_dynamic_backend(void)
{
	enum um_backend_kind want = (enum um_backend_kind)backend_arg_requested;
	bool force = backend_arg_force != 0;

	if (want == UM_BACKEND_KIND_PTRACE) {
		if (force)
			panic("um: backend=force=ptrace but ptrace backend was removed in memo 25 refactor 11; pin to v6.16 or earlier UML if you need it");
		pr_warn("um: backend=ptrace requested but ptrace backend was removed (memo 25 refactor 11); falling back to seccomp\n");
	} else if (want == UM_BACKEND_KIND_SECCOMP) {
		if (IS_ENABLED(CONFIG_UM_BACKEND_SECCOMP) && using_seccomp)
			return &um_backend_seccomp_ops;
		if (force)
			panic("um: backend=force=seccomp but seccomp not compiled in or probe failed");
	} else if (want == UM_BACKEND_KIND_KVM) {
#ifdef CONFIG_UM_BACKEND_KVM_V2
		/*
		 * v2 is the only KVM backend that can register today (v1
		 * archived). Probe runs in init_backend() after this returns
		 * and panics on failure regardless of force, so we only
		 * return v2 when forced — a non-force "preference" should
		 * still fall through to seccomp on a /dev/kvm-less host.
		 * The Phase A.1 ops table delegates HOT/cold ops to seccomp,
		 * so a successful force=kvm selection is functionally
		 * equivalent to seccomp until A.2+ migrates ops over.
		 */
		if (force)
			return &um_backend_kvm_v2_ops;
		pr_info("um: backend=kvm preference noted; v2 only auto-selects under force=kvm (Phase A.1); falling back to seccomp\n");
#else
		if (force)
			panic("um: backend=force=kvm requested but UM_BACKEND_KVM_V2 not built (v1 archived; enable EXPERT + UM_BACKEND_KVM_V2)");
		pr_warn("um: backend=kvm requested but UM_BACKEND_KVM_V2 not built; falling back\n");
#endif
	}

	/* auto / fall-through: seccomp is the only resident backend today */
	if (using_seccomp)
		return &um_backend_seccomp_ops;
	panic("um: dynamic backend resolution failed — seccomp probe did not succeed and no other backend is built");
}
#endif

enum um_backend_kind __init init_backend(const struct um_backend_args *args)
{
	(void)args;	/* parser writes backend_arg_* directly via __uml_setup */

#if defined(CONFIG_UM_BACKEND_SECCOMP_ONLY)
	if (backend_arg_requested == UM_BACKEND_KIND_PTRACE && backend_arg_force)
		panic("um: backend=force=ptrace but ptrace backend was removed in memo 25 refactor 11");
	um_backend = &um_backend_seccomp_ops;
	using_seccomp = 1;
#elif defined(CONFIG_UM_BACKEND_DYNAMIC)
	um_backend = pick_dynamic_backend();
#else
	panic("um: no backend selected by Kconfig");
#endif

	validate_hot_ops(um_backend);

	/*
	 * Dispatch the contract's cold lifecycle ops. probe() here is
	 * the "this backend can still run on this host" recheck — the
	 * arbiter already trusted os_early_checks()'s probe result via
	 * using_seccomp, but the ops-table probe is what
	 * backend-contract.rst advertises, so invoking it keeps the
	 * contract authoritative. init() sets up per-backend state;
	 * currently a no-op for both in-tree backends pending the
	 * os-Linux cleanup noted in each backend's lifecycle.c, but a
	 * future KVM backend's real kvm_open / vcpu-thread spawn will
	 * land here. Both ops panic on failure per the contract.
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
