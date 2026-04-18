// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: mm ops.
 *
 * Workstream A-03.S1. mm_map / mm_unmap delegate to the same
 * shared `um_stub_mm_*` helpers as the ptrace backend (the underlying
 * stub-syscall queueing handles both modes via using_seccomp checks
 * inside the helpers; eventually those checks go away once
 * per-backend FD-slot management is fully separated).
 *
 * mm_attach delegates to start_userspace() which currently dispatches
 * on `using_seccomp` to do the seccomp-specific socketpair + futex
 * setup. mm_detach kills the stub child and closes the per-mm
 * socketpair (the using_seccomp-gated `os_close_file(id->sock)` that
 * was previously in destroy_context() is folded in here).
 */
#include <linux/types.h>
#include <os.h>

#include "seccomp_backend.h"

int seccomp_mm_attach(struct mm_id *id)
{
	return start_userspace(id);
}

void seccomp_mm_detach(struct mm_id *id)
{
	if (id->pid > 0)
		os_kill_ptraced_process(id->pid, 1);
	if (id->sock)
		os_close_file(id->sock);
}

int seccomp_mm_map(struct mm_id *mm_idp, unsigned long virt,
		   unsigned long len, int prot, int phys_fd,
		   unsigned long long offset)
{
	return um_stub_mm_map(mm_idp, virt, len, prot, phys_fd, offset);
}

int seccomp_mm_unmap(struct mm_id *mm_idp, unsigned long addr,
		     unsigned long len)
{
	return um_stub_mm_unmap(mm_idp, addr, len);
}
