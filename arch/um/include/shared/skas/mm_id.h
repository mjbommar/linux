/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2005 Jeff Dike (jdike@karaya.com)
 */

#ifndef __MM_ID_H
#define __MM_ID_H

#include <linux/compiler_types.h>

#define STUB_MAX_FDS 4

struct mm_id {
	int pid;
	unsigned long stack;
	int syscall_data_len;

	/* Only used with SECCOMP mode */
	int sock;
	int syscall_fd_num;
	int syscall_fd_map[STUB_MAX_FDS];

	/*
	 * Per-mm shadow PGD for the integrated KVM backend (#275).
	 * Allocated by kvm_mm_attach, freed by kvm_mm_detach. Other
	 * backends ignore it; opaque pointer keeps internals out of
	 * this header.
	 */
	void *kvm_shadow;
};

struct mutex *__get_turnstile(struct mm_id *mm_id);
void enter_turnstile(struct mm_id *mm_id) __acquires(__get_turnstile(mm_id));
void exit_turnstile(struct mm_id *mm_id) __releases(__get_turnstile(mm_id));

void notify_mm_kill(int pid);

#endif
