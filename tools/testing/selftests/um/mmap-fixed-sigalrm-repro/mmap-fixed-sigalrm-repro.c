/*
 * Minimal Linux-only repro of the mmap-FIXED-to-different-inode +
 * POSIX timer SIGALRM-delivery interaction.
 *
 * Setup:
 *   1. mmap a tmpfs O_TMPFILE at FIXED VA, MAP_SHARED, identical
 *      to UML's setup_physmem call shape
 *   2. Create a POSIX timer with SIGEV_THREAD_ID targeting gettid()
 *   3. Arm a 100 ms one-shot
 *   4. mmap-FIXED swap to a DIFFERENT O_TMPFILE (identical content
 *      via mmap+memcpy)
 *   5. Re-arm 100 ms one-shot post-swap
 *   6. sigsuspend wait, capture how many SIGALRMs fire over 500 ms
 *
 * If output shows SIGALRMs arriving post-swap, the host kernel
 * preserves signal delivery and the UML-side bug is elsewhere.
 *
 * If output shows ZERO SIGALRMs post-swap, this is a host-kernel
 * signal-delivery regression.
 *
 * Build:  cc -O2 -Wall -o /tmp/mmap-fixed-sigalrm-repro \
 *             /tmp/mmap-fixed-sigalrm-repro.c
 * Run:    /tmp/mmap-fixed-sigalrm-repro
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define REGION_SZ  (4UL * 1024 * 1024)	/* 4 MiB sample */
#define VA_TARGET  ((void *)0x68000000UL)	/* arbitrary high VA */

static volatile unsigned long sigalrm_count;
static volatile unsigned long sigsegv_count;

static void on_alarm(int sig, siginfo_t *si, void *ucontext)
{
	(void)sig; (void)si; (void)ucontext;
	sigalrm_count++;
}

static void on_segv(int sig, siginfo_t *si, void *ucontext)
{
	(void)sig; (void)si; (void)ucontext;
	sigsegv_count++;
	_exit(2);
}

static int make_tmpfile(unsigned long size)
{
	int fd = open("/dev/shm",
		      O_RDWR | O_TMPFILE | O_CLOEXEC | O_EXCL, 0600);
	if (fd < 0)
		fd = open("/tmp",
			  O_RDWR | O_TMPFILE | O_CLOEXEC | O_EXCL, 0600);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, size) < 0) { close(fd); return -1; }
	return fd;
}

static int gettid_sys(void)
{
	return (int)syscall(SYS_gettid);
}

int main(void)
{
	struct sigaction sa = {0};
	timer_t timer;
	struct sigevent sev = {0};
	struct itimerspec its;
	int fd1, fd2;
	void *m1, *m2_scratch;
	struct timespec ts;
	unsigned long pre_swap, post_swap;
	int rc;

	/* SIGALRM handler */
	sa.sa_sigaction = on_alarm;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL) < 0) {
		perror("sigaction SIGALRM");
		return 1;
	}
	/* SIGSEGV handler so a fault in mmap-FIXED prints reason */
	sa.sa_sigaction = on_segv;
	if (sigaction(SIGSEGV, &sa, NULL) < 0) {
		perror("sigaction SIGSEGV");
		return 1;
	}

	/* Create two O_TMPFILEs and populate them with identical content. */
	fd1 = make_tmpfile(REGION_SZ);
	fd2 = make_tmpfile(REGION_SZ);
	if (fd1 < 0 || fd2 < 0) {
		fprintf(stderr, "make_tmpfile failed\n");
		return 1;
	}
	/* Initial bytes: pattern 'A' at offset 0 in both. */
	{
		char buf[16] = "AAAAAAAAAAAAAAAA";
		pwrite(fd1, buf, sizeof(buf), 0);
		pwrite(fd2, buf, sizeof(buf), 0);
	}

	/* mmap fd1 at fixed VA, MAP_SHARED. */
	m1 = mmap64(VA_TARGET, REGION_SZ,
		    PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_SHARED | MAP_FIXED | MAP_POPULATE, fd1, 0);
	if (m1 != VA_TARGET) {
		perror("mmap fd1 at VA_TARGET");
		return 1;
	}
	printf("init: m1=%p fd1=%d fd2=%d\n", m1, fd1, fd2);

	/* Create POSIX timer with SIGEV_THREAD_ID. */
	sev.sigev_notify = SIGEV_THREAD_ID;
	sev.sigev_signo  = SIGALRM;
	sev._sigev_un._tid = gettid_sys();
	if (timer_create(CLOCK_MONOTONIC, &sev, &timer) < 0) {
		perror("timer_create");
		return 1;
	}

	/* Arm 50 ms one-shot. */
	its.it_value.tv_sec  = 0;
	its.it_value.tv_nsec = 50000000L;
	its.it_interval.tv_sec  = 0;
	its.it_interval.tv_nsec = 0;
	if (timer_settime(timer, 0, &its, NULL) < 0) {
		perror("timer_settime pre");
		return 1;
	}

	/* Busy-wait 200 ms via clock_nanosleep so SIGALRMs accrue. */
	ts.tv_sec = 0;
	ts.tv_nsec = 200000000L;
	clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
	pre_swap = sigalrm_count;

	/* mmap-FIXED swap to fd2: different inode, identical content
	 * (both just have 'A' at offset 0).
	 */
	m2_scratch = mmap64(NULL, REGION_SZ, PROT_READ | PROT_WRITE,
			    MAP_SHARED, fd2, 0);
	if (m2_scratch == MAP_FAILED) {
		perror("mmap scratch fd2");
		return 1;
	}
	/* Copy m1 content into m2_scratch (so the swap is content-
	 * preserving like UML's um_pool_replicate_physmem).
	 */
	memcpy(m2_scratch, m1, REGION_SZ);
	munmap(m2_scratch, REGION_SZ);

	/* The actual swap. */
	{
		void *loc = mmap64(VA_TARGET, REGION_SZ,
				   PROT_READ | PROT_WRITE | PROT_EXEC,
				   MAP_SHARED | MAP_FIXED | MAP_POPULATE,
				   fd2, 0);
		if (loc != VA_TARGET) {
			perror("mmap-FIXED swap to fd2");
			return 1;
		}
	}
	printf("swap: VA still %p, backing now fd2=%d\n",
	       VA_TARGET, fd2);

	/* Arm fresh 50 ms one-shot post-swap. */
	if (timer_settime(timer, 0, &its, NULL) < 0) {
		perror("timer_settime post");
		return 1;
	}

	/* Sleep 200 ms more, counting SIGALRMs post-swap. */
	clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
	post_swap = sigalrm_count - pre_swap;

	printf("RESULT: pre_swap_sigalrms=%lu post_swap_sigalrms=%lu sigsegv=%lu\n",
	       pre_swap, post_swap, sigsegv_count);

	rc = (post_swap >= 1) ? 0 : 1;
	if (rc == 0)
		printf("=> SIGALRM delivered post-swap; this is NOT a host-kernel bug\n");
	else
		printf("=> SIGALRM did NOT deliver post-swap; host-kernel-level issue suspected\n");

	timer_delete(timer);
	close(fd1);
	close(fd2);
	munmap(m1, REGION_SZ);
	return rc;
}
