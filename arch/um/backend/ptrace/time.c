// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace backend: time ops (read_clock_ns, set_timer,
 * read_persistent_clock_ns).
 *
 * The underlying host primitives (os_nsecs, os_timer_*,
 * os_persistent_clock_emulation) are backend-neutral and shared by
 * the seccomp backend (and likely the future KVM backend); the per-
 * backend wrappers exist so the dispatch macro resolves to a named
 * symbol per the D11 dispatch convention.
 */
#include <linux/errno.h>
#include <linux/types.h>
#include <os.h>
#include <asm/backend.h>

#include "ptrace_backend.h"

/*
 * Read host wall clock in nanoseconds. Wraps the existing host-side
 * helper `os_persistent_clock_emulation()`. The cast to u64 is safe:
 * clock_gettime(CLOCK_REALTIME) returns a non-negative value modulo
 * the unix epoch.
 */
u64 ptrace_read_persistent_clock_ns(void)
{
	return (u64)os_persistent_clock_emulation();
}

/*
 * Read host monotonic clock in nanoseconds. Hot op; wraps os_nsecs()
 * (clock_gettime(CLOCK_MONOTONIC_RAW)).
 */
u64 ptrace_read_clock_ns(void)
{
	return (u64)os_nsecs();
}

/*
 * Configure host POSIX timer for @cpu. Maps the typed mode tag to
 * the existing os_timer_* family.
 */
int ptrace_set_timer(int cpu, u64 deadline_ns, enum um_timer_mode mode)
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
