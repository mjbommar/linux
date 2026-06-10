// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: mm ops.
 *
 * Ops take `struct mm_struct *mm` and look up the
 * per-mm seccomp state via `&mm->context.id` internally. Keeping
 * mm_id seccomp-internal lets each backend pick its own storage.
 *
 * mm_create delegates to start_userspace() which dispatches on the
 * stub_syscall_uses_futex capability to do the socketpair + futex
 * setup. mm_destroy kills the stub child and closes the per-mm
 * socketpair.
 */
#include <linux/mm_types.h>
#include <linux/types.h>
#include <asm/mmu.h>
#include <asm/um_memory.h>
#include <os.h>
#include <worker_api.h>

#include "seccomp_backend.h"

int seccomp_mm_create(struct mm_struct *mm)
{
	struct mm_id *id = &mm->context.id;

#ifdef CONFIG_UM_WORKER_PROCESS
	/*
	 * Under WORKER_PROCESS=y, hand the stub-child allocation off to a
	 * freshly-spawned per-mm worker. The worker runs start_userspace()
	 * inside its own VA and ships back the
	 * parent-side socketpair fd via SCM_RIGHTS so this id->sock is a
	 * spawner-side fd to the same kernel file.
	 *
	 * On failure fall back to the in-spawner clone so boots can
	 * complete even with a broken worker setup.
	 */
	if (!worker_alloc_stub_for_mm(mm, id))
		return 0;
#endif
	return start_userspace(id);
}

void seccomp_mm_destroy(struct mm_struct *mm)
{
	struct mm_id *id = &mm->context.id;

#ifdef CONFIG_UM_WORKER_PROCESS
	if (mm->context.worker) {
		/*
		 * The stub child lives inside the worker's VA; killing the
		 * worker (PR_SET_PDEATHSIG=SIGTERM was set in worker_main
		 * but the explicit SIGTERM via reap_worker_process is the
		 * primary path) reaps the stub child. id->sock here is the
		 * spawner-side SCM_RIGHTS fd; close it so we don't leak.
		 */
		if (id->sock)
			os_close_file(id->sock);
		reap_worker_for_mm(mm);
		return;
	}
#endif
	if (id->pid > 0)
		os_kill_ptraced_process(id->pid, 1);
	if (id->sock)
		os_close_file(id->sock);
}

int seccomp_mm_region_added(struct mm_struct *mm,
			    const struct um_memory_region *region)
{
	struct mm_id *id = &mm->context.id;

	return um_stub_mm_map(id, region->va, region->len, region->prot,
			      region->phys_fd, region->offset);
}

int seccomp_mm_region_removed(struct mm_struct *mm,
			      const struct um_memory_region *region)
{
	struct mm_id *id = &mm->context.id;

	return um_stub_mm_unmap(id, region->va, region->len);
}
