// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011 Richard Weinberger <richrd@nod.at>
 *
 * This vDSO turns all calls into a syscall so that UML can trap them.
 */


/* Disable profiling for userspace code */
#define DISABLE_BRANCH_PROFILING

#include <vdso/gettime.h>
#include <linux/time.h>
#include <asm/unistd.h>
#include "um_vdso.h"

#define UM_CLOCK_REALTIME_COARSE	5
#define UM_CLOCK_MONOTONIC_COARSE	6
#define UM_PAGE_SIZE			4096UL

/* The data page sits immediately below the (single) vDSO code page. */
static inline struct um_vdso_data *um_get_vdso_data(void)
{
	unsigned long rip;

	__asm__("lea 0(%%rip), %0" : "=r" (rip));
	return (struct um_vdso_data *)((rip & ~(UM_PAGE_SIZE - 1)) - UM_PAGE_SIZE);
}

int __vdso_clock_gettime(clockid_t clock, struct __kernel_timespec *ts)
{
	struct um_vdso_data *vd = um_get_vdso_data();
	long ret;

	/* Coarse clocks: read the per-tick snapshot with no syscall/trap. */
	if ((clock == UM_CLOCK_MONOTONIC_COARSE ||
	     clock == UM_CLOCK_REALTIME_COARSE) &&
	    READ_ONCE(vd->valid)) {
		unsigned int seq;

		/*
		 * Seqlock read: retry while the writer is mid-update (odd seq)
		 * or the snapshot changed under us. The READ_ONCE accesses order
		 * the seq reads around the field copies; x86 is TSO so no
		 * explicit barrier is needed.
		 */
		do {
			seq = READ_ONCE(vd->seq);
			if (seq & 1)
				continue;
			if (clock == UM_CLOCK_MONOTONIC_COARSE) {
				ts->tv_sec  = vd->mono_coarse_sec;
				ts->tv_nsec = vd->mono_coarse_nsec;
			} else {
				ts->tv_sec  = vd->real_coarse_sec;
				ts->tv_nsec = vd->real_coarse_nsec;
			}
		} while (seq != READ_ONCE(vd->seq));
		return 0;
	}

	asm("syscall"
		: "=a" (ret)
		: "0" (__NR_clock_gettime), "D" (clock), "S" (ts)
		: "rcx", "r11", "memory");

	return ret;
}
int clock_gettime(clockid_t, struct __kernel_timespec *)
	__attribute__((weak, alias("__vdso_clock_gettime")));

int __vdso_gettimeofday(struct __kernel_old_timeval *tv, struct timezone *tz)
{
	long ret;

	asm("syscall"
		: "=a" (ret)
		: "0" (__NR_gettimeofday), "D" (tv), "S" (tz)
		: "rcx", "r11", "memory");

	return ret;
}
int gettimeofday(struct __kernel_old_timeval *, struct timezone *)
	__attribute__((weak, alias("__vdso_gettimeofday")));

__kernel_old_time_t __vdso_time(__kernel_old_time_t *t)
{
	long secs;

	asm volatile("syscall"
		: "=a" (secs)
		: "0" (__NR_time), "D" (t) : "cc", "r11", "cx", "memory");

	return secs;
}
__kernel_old_time_t time(__kernel_old_time_t *t) __attribute__((weak, alias("__vdso_time")));
