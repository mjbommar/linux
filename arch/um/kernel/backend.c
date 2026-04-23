// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend arbiter.
 *
 * Single source of truth for which backend is active. Picks one of
 * the compiled-in backends (arch/um/backend/<kind>/) and exposes it
 * as the global `um_backend` pointer used by the dispatch macro in
 * arch/um/include/shared/backend.h.
 *
 * Resolution order:
 *   1. *_ONLY Kconfig pins the choice at compile time.
 *   2. CONFIG_UM_BACKEND_DYNAMIC: read using_seccomp (set by the
 *      seccomp probe in os_early_checks), choose accordingly.
 *      Honors `seccomp=on` (already enforced by os_early_checks
 *      panicking if the probe fails) and falls back to ptrace if
 *      using_seccomp == 0.
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
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/bug.h>
#include <asm/backend.h>

const struct um_backend_ops *um_backend;
EXPORT_SYMBOL_GPL(um_backend);

#ifdef CONFIG_UM_BACKEND_PTRACE
extern const struct um_backend_ops um_backend_ptrace_ops;
#endif
#ifdef CONFIG_UM_BACKEND_SECCOMP
extern const struct um_backend_ops um_backend_seccomp_ops;
#endif
#ifdef CONFIG_UM_BACKEND_KVM
extern const struct um_backend_ops um_backend_kvm_ops;
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
	if (!ops->run_userspace || !ops->mm_map || !ops->mm_unmap ||
	    !ops->context_switch || !ops->read_clock_ns)
		panic("um: backend %s has NULL HOT op (run_userspace=%p mm_map=%p mm_unmap=%p context_switch=%p read_clock_ns=%p)",
		      ops->name,
		      ops->run_userspace, ops->mm_map, ops->mm_unmap,
		      ops->context_switch, ops->read_clock_ns);
}

/*
 * Pick a backend in DYNAMIC mode. Resolution order:
 *   1. `backend=force=<kind>` boot param: requested kind or panic.
 *   2. `backend=<kind>` (non-force): preferred kind if compiled in,
 *      else fall through to using_seccomp.
 *   3. `backend=auto` (default) / no boot param: defer to using_seccomp.
 */
#ifdef CONFIG_UM_BACKEND_DYNAMIC
static const struct um_backend_ops * __init pick_dynamic_backend(void)
{
	enum um_backend_kind want = (enum um_backend_kind)backend_arg_requested;
	bool force = backend_arg_force != 0;

	if (want == UM_BACKEND_KIND_PTRACE) {
		if (IS_ENABLED(CONFIG_UM_BACKEND_PTRACE)) {
			using_seccomp = 0;
			return &um_backend_ptrace_ops;
		}
		if (force)
			panic("um: backend=force=ptrace but ptrace not compiled in");
	} else if (want == UM_BACKEND_KIND_SECCOMP) {
		if (IS_ENABLED(CONFIG_UM_BACKEND_SECCOMP) && using_seccomp) {
			return &um_backend_seccomp_ops;
		}
		if (force)
			panic("um: backend=force=seccomp but seccomp not compiled in or probe failed");
	}
#ifdef CONFIG_UM_BACKEND_KVM
	else if (want == UM_BACKEND_KIND_KVM) {
		/*
		 * Non-force `backend=kvm` runs probe() here — if the
		 * host is missing /dev/kvm or the kvm API version
		 * drifted, we defer to the using_seccomp / ptrace
		 * default with a warning rather than letting
		 * init_backend() panic. That matches the design
		 * memo's "detect at runtime, fall back to seccomp
		 * backend with a warning" mitigation for the nested-
		 * KVM / CI-host case. force=kvm skips this and lets
		 * init_backend() panic per the arbiter contract.
		 */
		if (force) {
			using_seccomp = 0;
			return &um_backend_kvm_ops;
		}
		if (um_backend_kvm_ops.probe() == 0) {
			using_seccomp = 0;
			return &um_backend_kvm_ops;
		}
		pr_warn("um: backend=kvm requested but probe failed; falling back\n");
	}
#endif

	/* auto / fall-through */
	if (using_seccomp)
		return &um_backend_seccomp_ops;
	return &um_backend_ptrace_ops;
}
#endif

enum um_backend_kind __init init_backend(const struct um_backend_args *args)
{
	(void)args;	/* parser writes backend_arg_* directly via __uml_setup */

#if defined(CONFIG_UM_BACKEND_SECCOMP_ONLY)
	if (backend_arg_requested == UM_BACKEND_KIND_PTRACE && backend_arg_force)
		panic("um: backend=force=ptrace but kernel built SECCOMP_ONLY");
	um_backend = &um_backend_seccomp_ops;
	using_seccomp = 1;
#elif defined(CONFIG_UM_BACKEND_PTRACE_ONLY)
	if (backend_arg_requested == UM_BACKEND_KIND_SECCOMP && backend_arg_force)
		panic("um: backend=force=seccomp but kernel built PTRACE_ONLY");
	um_backend = &um_backend_ptrace_ops;
	using_seccomp = 0;
#elif defined(CONFIG_UM_BACKEND_KVM_ONLY)
	if ((backend_arg_requested == UM_BACKEND_KIND_PTRACE ||
	     backend_arg_requested == UM_BACKEND_KIND_SECCOMP) &&
	    backend_arg_force)
		panic("um: backend=force=%s but kernel built KVM_ONLY",
		      backend_arg_requested == UM_BACKEND_KIND_PTRACE ?
		      "ptrace" : "seccomp");
	um_backend = &um_backend_kvm_ops;
	using_seccomp = 0;
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
