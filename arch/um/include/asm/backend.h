/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML backend kernel-only API surface.
 *
 * Most of the contract (struct definitions, the dispatch macro, the
 * extern um_backend pointer, per-backend op prototypes) lives in
 * <backend.h> so it's reachable from USER TUs too. This header adds
 * the kernel-only declarations: the init_backend() entry point.
 *
 * Per-op semantics: Documentation/virt/uml/backend-contract.rst.
 */
#ifndef __ASM_UM_BACKEND_H
#define __ASM_UM_BACKEND_H

#include <linux/types.h>
#include <linux/compiler_types.h>
#include <backend.h>

/*
 * Called once at boot from setup_arch(). Picks one of the
 * compiled-in backends (per Kconfig + the host probe results
 * recorded by os_early_checks()), validates its HOT ops are
 * non-NULL, sets the global um_backend, and returns the chosen
 * kind. Single source of truth for backend selection — nothing
 * else writes um_backend after this.
 */
enum um_backend_kind init_backend(const struct um_backend_args *args);

#endif /* __ASM_UM_BACKEND_H */
