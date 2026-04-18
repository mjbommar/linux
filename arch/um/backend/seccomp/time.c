// SPDX-License-Identifier: GPL-2.0
/*
 * seccomp backend: time ops.
 *
 * Workstream A-03.S1.5. The host POSIX timer + clock_gettime
 * primitives are backend-neutral; these wrappers exist for the
 * dispatch macro to resolve.
 */
#include <linux/errno.h>
#include <linux/types.h>
#include <os.h>
#include <asm/backend.h>

#include "seccomp_backend.h"

u64 seccomp_read_persistent_clock_ns(void)
{
	return (u64)os_persistent_clock_emulation();
}

u64 seccomp_read_clock_ns(void)
{
	return (u64)os_nsecs();
}

int seccomp_set_timer(int cpu, u64 deadline_ns, enum um_timer_mode mode)
{
	switch (mode) {
	case UM_TIMER_DISABLE:
		os_timer_disable(cpu);
		return 0;
	case UM_TIMER_ONE_SHOT:
		return os_timer_one_shot(cpu, deadline_ns);
	case UM_TIMER_PERIODIC:
		return os_timer_set_interval(cpu, deadline_ns);
	}
	return -EINVAL;
}
