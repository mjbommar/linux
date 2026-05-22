// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Anton Ivanov (aivanov@{brocade.com,kot-begemot.co.uk})
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2012-2014 Cisco Systems
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike{addtoit,linux.intel}.com)
 */

#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/signalfd.h>
#include <sys/time.h>
#include <kern_util.h>
#include <os.h>
#include <smp.h>
#include <string.h>
#include "internal.h"

static timer_t event_high_res_timer[CONFIG_NR_CPUS] = { 0 };

static inline long long timespec_to_ns(const struct timespec *ts)
{
	return ((long long) ts->tv_sec * UM_NSEC_PER_SEC) + ts->tv_nsec;
}

long long os_persistent_clock_emulation(void)
{
	struct timespec realtime_tp;

	clock_gettime(CLOCK_REALTIME, &realtime_tp);
	return timespec_to_ns(&realtime_tp);
}

#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif

/**
 * os_timer_create() - create an new posix (interval) timer
 */
int os_timer_create(void)
{
	int cpu = uml_curr_cpu();
	timer_t *t = &event_high_res_timer[cpu];
	struct sigevent sev = {
		.sigev_notify = SIGEV_THREAD_ID,
		.sigev_signo = SIGALRM,
		.sigev_value.sival_ptr = t,
		.sigev_notify_thread_id = gettid(),
	};

	if (timer_create(CLOCK_MONOTONIC, &sev, t) == -1)
		return -1;

	return 0;
}

int os_timer_set_interval(int cpu, unsigned long long nsecs)
{
	struct itimerspec its;

	its.it_value.tv_sec = nsecs / UM_NSEC_PER_SEC;
	its.it_value.tv_nsec = nsecs % UM_NSEC_PER_SEC;

	its.it_interval.tv_sec = nsecs / UM_NSEC_PER_SEC;
	its.it_interval.tv_nsec = nsecs % UM_NSEC_PER_SEC;

	if (timer_settime(event_high_res_timer[cpu], 0, &its, NULL) == -1)
		return -errno;

	return 0;
}

/*
 * Diagnostic: read timer expiry state via timer_getoverrun and
 * timer_gettime.  Used by pool-member triage to distinguish two
 * SIGALRM-after-swap failure modes:
 *   - overrun > 0 → host POSIX timer is firing but signals lost
 *     (delivery problem)
 *   - overrun == 0 + it_value > 0 → timer armed, hasn't fired
 *     yet (timer is genuinely stopped or far in future)
 *   - it_value == 0 → timer disarmed
 *
 * Stores results in @out_overrun and @out_it_value_ns.  Returns
 * 0 on success, -errno on failure.
 */
int os_timer_diagnose(unsigned long *out_overrun,
		      unsigned long long *out_it_value_ns)
{
	struct itimerspec its;
	int rc;
	int cpu = 0;

	rc = timer_getoverrun(event_high_res_timer[cpu]);
	if (rc < 0)
		return -errno;
	*out_overrun = (unsigned long)rc;

	if (timer_gettime(event_high_res_timer[cpu], &its) < 0)
		return -errno;
	*out_it_value_ns = (unsigned long long)its.it_value.tv_sec
				* UM_NSEC_PER_SEC
			   + its.it_value.tv_nsec;
	return 0;
}

/*
 * Diagnostic: busy-wait via clock_nanosleep for @nsecs.  Used by
 * pool-member variant 9 to wait for the POSIX timer to expire
 * without depending on UML's signal-driven scheduler tick.
 */
void os_busy_wait_ns(unsigned long long nsecs)
{
	struct timespec ts = {
		.tv_sec  = nsecs / UM_NSEC_PER_SEC,
		.tv_nsec = nsecs % UM_NSEC_PER_SEC,
	};

	/* CLOCK_MONOTONIC + relative request — does not depend on
	 * signal delivery to return.
	 */
	(void)clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

int os_timer_one_shot(int cpu, unsigned long long nsecs)
{
	struct itimerspec its = {
		.it_value.tv_sec = nsecs / UM_NSEC_PER_SEC,
		.it_value.tv_nsec = nsecs % UM_NSEC_PER_SEC,

		.it_interval.tv_sec = 0,
		.it_interval.tv_nsec = 0, // we cheat here
	};

	timer_settime(event_high_res_timer[cpu], 0, &its, NULL);
	return 0;
}

/**
 * os_timer_disable() - disable the posix (interval) timer
 * @cpu: the CPU for which the timer is to be disabled
 */
void os_timer_disable(int cpu)
{
	struct itimerspec its;

	memset(&its, 0, sizeof(struct itimerspec));
	timer_settime(event_high_res_timer[cpu], 0, &its, NULL);
}

/*
 * Forget inherited POSIX timer state after a fork() (workstream C-09,
 * commit 3c). Each CPU's timer was created with SIGEV_THREAD_ID
 * targeting the CPU thread's gettid() in the parent; in the forked
 * child those tids refer to threads that do not exist, and any
 * SIGALRM the kernel tries to deliver would be misdirected or lost.
 *
 * Disable all inherited timers. Do NOT timer_delete() them: the
 * child shares the timer_t handles with the parent, and timer_delete
 * in the child would destroy the parent's timers too. timer_settime
 * with a zero itimerspec is local to the caller and safe.
 *
 * Called only from the forkserver worker path via the wrapper in
 * arch/um/kernel/snapshot.c.
 */
void os_timer_worker_forget(void)
{
	int cpu;

	for (cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
		/* event_high_res_timer[] is zero-initialized; os_timer_create()
		 * populates only the CPUs that were brought up. Disable only
		 * the populated ones.
		 */
		if (event_high_res_timer[cpu] == 0)
			continue;
		os_timer_disable(cpu);
	}
}

/*
 * Re-create a POSIX timer in a forkserver worker (workstream C-09,
 * commit 3d-b). Pair with os_timer_worker_forget: that call
 * disabled parent-inherited timers whose SIGEV_THREAD_ID pointed
 * at threads that no longer exist in the child; this one installs
 * a fresh timer targeting the current thread's gettid(), which
 * IS valid in the worker.
 *
 * Overwrites event_high_res_timer[0] with the new handle. The
 * parent's handles survive in its own address space (fork COW'd
 * the timer_t array; the worker's writes here are local). Only
 * CPU 0 is rebuilt; UP is the fuzz-profile constraint today per
 * um_snapshot_assert_ready's num_online_cpus > 1 refusal.
 *
 * Returns 0 on success or -errno on failure. Commit 3d-b tolerates
 * failure (worker exits immediately anyway); commit 3d-c will
 * treat failure as a reason to skip the guest-code-resume path.
 */
int os_timer_worker_rebuild(void)
{
	int cpu = 0;	/* UP per D41; SMP deferred */
	timer_t *t = &event_high_res_timer[cpu];
	struct sigevent sev = {
		.sigev_notify = SIGEV_THREAD_ID,
		.sigev_signo = SIGALRM,
		.sigev_value.sival_ptr = t,
		.sigev_notify_thread_id = gettid(),
	};

	if (timer_create(CLOCK_MONOTONIC, &sev, t) == -1)
		return -errno;

	return 0;
}

long long os_nsecs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC,&ts);
	return timespec_to_ns(&ts);
}

/*
 * Per-thread CPU time in nanoseconds (CLOCK_THREAD_CPUTIME_ID).
 * Counts host CPU time accrued by the calling host thread, including
 * time spent inside ioctl(KVM_RUN, ...).  Used by the kvm-v2 backend
 * to credit user-CPU time accumulated during guest execution to the
 * calling guest task — without it, ITIMER_VIRTUAL accounting (and any
 * other utime-sensitive guest path) doesn't accrue.
 *
 * SMP-T80 fix: prior to this helper, kvm-v2's KVM_RUN time was
 * accounted to "system" via timer_handler's r.is_user=0 default,
 * giving ITIMER_VIRTUAL workloads 0 % progress under the kvm-v2
 * backend.  Crediting deltas of CLOCK_THREAD_CPUTIME_ID around each
 * ioctl(KVM_RUN) restores the user-time semantics CPython's
 * test_itimer_virtual depends on.
 */
long long os_thread_cputime_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
		return 0;
	return timespec_to_ns(&ts);
}

static __thread int wake_signals;

void os_idle_prepare(void)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	sigaddset(&set, IPI_SIGNAL);

	/*
	 * We need to use signalfd rather than sigsuspend in idle sleep
	 * because the IPI signal is a real-time signal that carries data,
	 * and unlike handling SIGALRM, we cannot simply flag it in
	 * signals_pending.
	 *
	 * FD disposition (C-09 commit 4): inherit. The signalfd
	 * targets signals blocked in the calling thread's mask; after
	 * a forkserver fork the worker inherits both the signal mask
	 * and the fd, and the fd remains correctly wired for the
	 * worker's own thread-local mask. No rebuild is needed. The
	 * separate POSIX timer is what gets rebuilt in
	 * os_timer_worker_rebuild() — that's the per-thread timer-
	 * delivery target; this fd is the idle-sleep wake source.
	 */
	wake_signals = signalfd(-1, &set, SFD_CLOEXEC);
	if (wake_signals < 0)
		panic("Failed to create signal FD, errno = %d", errno);
}

/**
 * os_idle_sleep() - sleep until interrupted
 */
void os_idle_sleep(void)
{
	sigset_t set;

	/*
	 * Block SIGALRM while performing the need_resched check.
	 * Note that, because IRQs are disabled, the IPI signal is
	 * already blocked.
	 */
	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	sigprocmask(SIG_BLOCK, &set, NULL);

	/*
	 * Because disabling IRQs does not block SIGALRM, it is also
	 * necessary to check for any pending timer alarms.
	 */
	if (!uml_need_resched() && !timer_alarm_pending())
		os_poll(1, &wake_signals);

	/* Restore the signal mask. */
	sigprocmask(SIG_UNBLOCK, &set, NULL);
}
