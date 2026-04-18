// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: lifecycle ops (probe, init, shutdown).
 *
 * Workstream A-03.S1.4. Mirrors arch/um/backend/ptrace/lifecycle.c —
 * the real probe/init logic lives in
 * arch/um/os-Linux/start_up.c::os_early_checks() which runs
 * unconditionally at boot (before init_backend()) and sets
 * `using_seccomp`. The ops-table entries here are stubs.
 *
 * Lifting the probe/init logic into these ops is a future cleanup
 * (no workstream currently owns it); it needs to land together with
 * a reorganization of the os-Linux early-boot sequence so that the
 * arbiter can run probe() itself and fall back on failure.
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
