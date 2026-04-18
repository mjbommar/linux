// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace backend: mm_map / mm_unmap.
 *
 * Workstream A-02.HOT-2. Thin wrappers around the shared stub helpers
 * `um_stub_mm_map()` and `um_stub_mm_unmap()` (formerly bare-named
 * `map()` and `unmap()` in arch/um/os-Linux/skas/mem.c).
 *
 * Why wrappers, not direct registration of the shared symbols?
 *   - The dispatch macro um_backend_dispatch(mm_map, ...) token-pastes
 *     to ptrace_mm_map(...) in CONFIG_UM_BACKEND_PTRACE_ONLY=y builds;
 *     the symbol must exist with that exact name.
 *   - The shared impl is reused by the (forthcoming, A-03) seccomp
 *     backend's seccomp_mm_map() wrapper. The using_seccomp branches
 *     inside um_stub_mm_map (per-mm FD slot management for SCM_RIGHTS)
 *     remain inside the shared impl until A-03 splits them.
 *   - The compiler inlines these one-liners in single-backend builds,
 *     producing zero indirect-dispatch cost.
 *
 * KERNEL-side TU. The shared helpers are USER-side functions in
 * os-Linux/skas/mem.c; the linker resolves the call across the
 * boundary, mirroring the existing kernel→USER call pattern (see
 * kernel/tlb.c also calling `os_map_memory`/`os_unmap_memory`).
 */
#include <linux/types.h>
#include <os.h>

#include "ptrace_backend.h"

int ptrace_mm_map(struct mm_id *mm_idp, unsigned long virt,
		  unsigned long len, int prot, int phys_fd,
		  unsigned long long offset)
{
	return um_stub_mm_map(mm_idp, virt, len, prot, phys_fd, offset);
}

int ptrace_mm_unmap(struct mm_id *mm_idp, unsigned long addr,
		    unsigned long len)
{
	return um_stub_mm_unmap(mm_idp, addr, len);
}

/*
 * A-02.COLD-3: mm_attach / mm_detach.
 *
 * mm_attach wraps start_userspace() (clones the stub child, waits
 * for it to land in PTRACE_O_TRACESYSGOOD).
 * mm_detach wraps os_kill_ptraced_process() for the per-mm case.
 * The seccomp-only socket close (mm->id.sock) currently lives in
 * destroy_context() and migrates to seccomp_mm_detach() in A-03.
 */
int ptrace_mm_attach(struct mm_id *id)
{
	return start_userspace(id);
}

void ptrace_mm_detach(struct mm_id *id)
{
	if (id->pid > 0) {
		os_kill_ptraced_process(id->pid, 1);
		id->pid = -1;
	}
}
