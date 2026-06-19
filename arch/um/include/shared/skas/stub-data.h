/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2005 Jeff Dike (jdike@karaya.com)
 */

#ifndef __STUB_DATA_H
#define __STUB_DATA_H

#include <linux/compiler_types.h>
#include <as-layout.h>
#include <sysdep/tls.h>
#include <sysdep/stub-data.h>
#include <mm_id.h>

#define FUTEX_IN_CHILD 0
#define FUTEX_IN_KERN 1

struct stub_init_data {
	int seccomp;

	unsigned long stub_start;

	int stub_code_fd;
	unsigned long stub_code_offset;
	int stub_data_fd;
	unsigned long stub_data_offset;

	unsigned long signal_handler;
	unsigned long signal_restorer;
};

#define STUB_NEXT_SYSCALL(s) \
	((struct stub_syscall *) (((unsigned long) s) + (s)->cmd_len))

enum stub_syscall_type {
	STUB_SYSCALL_UNSET = 0,
	STUB_SYSCALL_MMAP,
	STUB_SYSCALL_MUNMAP,
};

struct stub_syscall {
	struct {
		unsigned long addr;
		unsigned long length;
		unsigned long offset;
		int fd;
		int prot;
	} mem;

	enum stub_syscall_type syscall;
};

struct stub_data {
	long err;

	int syscall_data_len;
	/* 128 leaves enough room for additional fields in the struct */
	struct stub_syscall syscall_data[(UM_KERN_PAGE_SIZE - 128) / sizeof(struct stub_syscall)] __aligned(16);

	/* data shared with signal handler (only used in seccomp mode) */
	short restart_wait;
	unsigned int futex;
	int signal;
	unsigned short si_offset;
	unsigned short mctx_offset;

	/* seccomp architecture specific state restore */
	struct stub_data_arch arch_data;

	/*
	 * Clock gadget (seccomp): when nonzero, the stub answers
	 * clock_gettime(CLOCK_MONOTONIC) in-place as
	 *   guest_mono = host_CLOCK_MONOTONIC - clock_mono_offset
	 * skipping the futex handoff. UML stamps the offset
	 * (os_nsecs() - ktime_get_ns()) on a crossing clock_gettime, and only
	 * when NOT in time-travel mode (so the gadget is implicitly disabled
	 * for virtual time). The guest clocksource IS host CLOCK_MONOTONIC, so
	 * the offset is the constant host-uptime-at-guest-boot.
	 *
	 * clock_real_offset does the same for CLOCK_REALTIME. REALTIME can be
	 * stepped (settimeofday/NTP/suspend), so a stamped-once offset is only
	 * exact until the guest's wall clock is adjusted; a production version
	 * should re-stamp on a budget. MONOTONIC never jumps and is exact.
	 */
	long long clock_mono_offset;
	long long clock_real_offset;

	/* Stack for signal handlers and stub syscall execution. */
	unsigned char sigstack[UM_KERN_PAGE_SIZE] __aligned(UM_KERN_PAGE_SIZE);
};

#endif
