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
 * D-03a: the /dev/kvm fd is held in lifecycle.c. Consumers that
 * need it (D-03b+ — mm_attach issuing KVM_CREATE_VM, etc.) go
 * through this accessor rather than touching the static in
 * lifecycle.c directly. Returns -1 until init() has run; callers
 * ordered against init_backend() can treat that as a bug.
 */
int kvm_backend_fd(void);

#endif /* __ARCH_UM_BACKEND_KVM_H */
