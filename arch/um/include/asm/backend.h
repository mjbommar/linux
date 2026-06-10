/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML backend kernel-only API surface.
 *
 * Most of the contract (struct definitions, the dispatch macro, the
 * um_backend extern declaration, and per-backend op prototypes) lives
 * in <backend.h> so host-built UML files can include it too. This header
 * adds the kernel-only declarations: the init_backend() entry point,
 * ops-table singletons selected by the arbiter, and early boot handoff
 * state consumed by init_backend().
 *
 * Per-op semantics: Documentation/virt/uml/backend-contract.rst.
 */
#ifndef __ASM_UM_BACKEND_H
#define __ASM_UM_BACKEND_H

#include <linux/init.h>
#include <linux/types.h>
#include <linux/compiler_types.h>
#include <backend.h>

/*
 * Called once at boot from setup_arch(). Picks one of the
 * compiled-in backends (per Kconfig + the host probe results
 * recorded by os_early_checks()), validates its required ops are
 * non-NULL, sets the global um_backend, and returns the chosen
 * kind. Single source of truth for backend selection; nothing
 * else writes um_backend after this.
 */
enum um_backend_kind __init init_backend(const struct um_backend_args *args);

#ifdef CONFIG_UM_BACKEND_SECCOMP
extern const struct um_backend_ops um_backend_seccomp_ops;
#endif
#ifdef CONFIG_UM_BACKEND_KVM_V2
extern const struct um_backend_ops um_backend_kvm_v2_ops;
#endif

/*
 * Host probe and boot-param state from arch/um/os-Linux/. init_backend()
 * consumes these during early boot before the backend choice is immutable.
 */
extern int using_seccomp;
extern int backend_arg_requested;
extern int backend_arg_force;

#endif /* __ASM_UM_BACKEND_H */
