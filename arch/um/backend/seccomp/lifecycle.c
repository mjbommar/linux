// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: lifecycle ops (probe, init, shutdown).
 *
 * The real probe/init logic lives in
 * arch/um/os-Linux/start_up.c::os_early_checks() which runs
 * unconditionally at boot (before init_backend()) and sets
 * `using_seccomp`. The ops-table entries here are stubs.
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
