// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: mm ops.
 *
 * Workstream A-03.S1 + memo 25 R2.
 *
 * Ops take `struct mm_struct *mm` (memo 25 R2 cleanup) and look up the
 * per-mm seccomp state via `&mm->context.id` internally. Other backends
 * (kvm-v2's per-mm worker process) will key their per-mm state off the
 * mm pointer differently — keeping mm_id seccomp-internal lets each
 * backend pick its own storage.
 *
 * mm_create delegates to start_userspace() which dispatches on the
 * stub_syscall_uses_futex capability to do the socketpair + futex
 * setup. mm_destroy kills the stub child and closes the per-mm
 * socketpair.
 */
#include <linux/mm_types.h>
#include <linux/types.h>
#include <asm/mmu.h>
#include <os.h>

#include "seccomp_backend.h"

int seccomp_mm_create(struct mm_struct *mm)
{
	struct mm_id *id = &mm->context.id;

	return start_userspace(id);
}

void seccomp_mm_destroy(struct mm_struct *mm)
{
	struct mm_id *id = &mm->context.id;

	if (id->pid > 0)
		os_kill_ptraced_process(id->pid, 1);
	if (id->sock)
		os_close_file(id->sock);
}

int seccomp_mm_region_added(struct mm_struct *mm, unsigned long virt,
			    unsigned long len, int prot, int phys_fd,
			    unsigned long long offset)
{
	struct mm_id *id = &mm->context.id;

	return um_stub_mm_map(id, virt, len, prot, phys_fd, offset);
}

int seccomp_mm_region_removed(struct mm_struct *mm, unsigned long addr,
			      unsigned long len)
{
	struct mm_id *id = &mm->context.id;

	return um_stub_mm_unmap(id, addr, len);
}
