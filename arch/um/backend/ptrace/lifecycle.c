// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace backend: lifecycle ops (probe, init, shutdown).
 *
 * Workstream A-02.COLD-3.
 *
 * For the migration window the ptrace boot probe still runs
 * unconditionally from arch/um/os-Linux/start_up.c::os_early_checks()
 * (start_ptraced_child + init_pid_registers + stop_ptraced_child).
 * The ops-table entries here are stubs:
 *
 *   probe()    — returns 0 unconditionally; real probe stays at boot.
 *                The arbiter trusts that ptrace is always available
 *                when CONFIG_UM_BACKEND_PTRACE=y.
 *   init()     — no-op; existing init scattered across os-Linux/.
 *   shutdown() — no-op; halt/reboot paths are kernel-side and use
 *                halt_skas()/reboot_skas() directly via the longjmp
 *                pattern (see arch/um/kernel/reboot.c).
 *
 * Lifting the real probe and init logic into these ops is the A3
 * refactor, scoped in Documentation/virt/uml/redesign/02-
 * workstreams/A-backend-abstraction/08-layer1-probe-refactor.md +
 * decisions-log D72 (2026-04-24). Plan: expose check_ptrace from
 * start_up.c, have this probe() call it, have init_backend drive
 * probes instead of os_early_checks.
 */
#include <linux/types.h>

#include "ptrace_backend.h"
#include <asm/backend.h>

int ptrace_probe(void)
{
	return 0;
}

int ptrace_init(const struct um_backend_args *args)
{
	(void)args;
	return 0;
}

void ptrace_shutdown(void)
{
}
