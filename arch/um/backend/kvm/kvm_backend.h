/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal header for arch/um/backend/kvm/.
 *
 * The ops-table struct, dispatch macro, and per-backend op
 * prototypes are all in <backend.h> (shared header). This header
 * is the stable include point for kvm TUs, matching the
 * ptrace/seccomp backends' convention.
 *
 * Workstream D-02: scaffold only.
 */
#ifndef __ARCH_UM_BACKEND_KVM_H
#define __ARCH_UM_BACKEND_KVM_H

#include <backend.h>

/*
 * Scaffold-stage kvm backend: no shared state yet. D-03 adds the
 * host-fd struct + per-VM plumbing and grows this header.
 */

#endif /* __ARCH_UM_BACKEND_KVM_H */
