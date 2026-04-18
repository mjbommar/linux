// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: lifecycle ops (probe, init, shutdown).
 *
 * Workstream A-03.S1.4. Mirrors arch/um/backend/ptrace/lifecycle.c —
 * the real probe/init logic still lives in
 * arch/um/os-Linux/start_up.c::os_early_checks() which runs
 * unconditionally at boot. The ops-table entries here are stubs
 * until the boot sequence is unified (deferred to A-04).
 *
 * Once the arbiter takes over backend selection, seccomp_probe will
 * call init_seccomp() (currently scattered across os-Linux/start_up.c)
 * and return non-zero on failure so the arbiter can fall back to
 * ptrace.
 */
#include <linux/types.h>

#include "seccomp_backend.h"
#include <asm/backend.h>

int seccomp_probe(void)
{
	return 0;
}

int seccomp_init(const struct um_backend_args *args)
{
	(void)args;
	return 0;
}

void seccomp_shutdown(void)
{
}
