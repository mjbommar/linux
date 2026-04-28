// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend: time ops (read_clock_ns, set_timer,
 * read_persistent_clock_ns).
 *
 * Workstream D-05a. The underlying host primitives (os_nsecs,
 * os_timer_*, os_persistent_clock_emulation) are backend-neutral
 * — ptrace and seccomp both wrap them identically. The KVM
 * backend does the same: these ops are about the HOST's clock
 * behaviour (used by UML's kernel-side timekeeping), not about
 * anything the KVM vCPU runs. Real guest-side timekeeping via
 * kvmclock is a phase-2 optimisation, explicitly deferred per
 * the design memo's "Not in first-phase scope" list.
 *
 * Unblocks: non-harness backend=kvm boot past time_init() —
 * which previously pre-console-panicked when set_timer returned
 * -EOPNOTSUPP. Also unblocks the D-04b.2b.1 late_initcall
 * harness relocation once a real clock source is available for
 * jiffies to advance.
 */
#include <linux/errno.h>
#include <linux/types.h>
#include <os.h>
#include <asm/backend.h>

#include "kvm_backend.h"

u64 kvm_read_persistent_clock_ns(void)
{
	return (u64)os_persistent_clock_emulation();
}

u64 kvm_read_clock_ns(void)
{
	return (u64)os_nsecs();
}

int kvm_set_timer(int cpu, u64 deadline_ns, enum um_timer_mode mode)
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
