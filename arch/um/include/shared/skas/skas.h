/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __SKAS_H
#define __SKAS_H

#include <sysdep/ptrace.h>

/*
 * `extern int using_seccomp` used to live here; removed in
 * 2026-04-23 as D59 Phase II Lift #4d+ after all twelve
 * runtime readers moved to the typed um_backend_ops
 * capability flags. The flag is now purely an
 * os_early_checks → init_backend arbiter intermediary,
 * declared privately to those two translation units
 * (arch/um/kernel/backend.c has its own extern; USER-TU
 * consumers pick it up via arch/um/os-Linux/internal.h).
 */

extern void new_thread_handler(void);
extern void handle_syscall(struct uml_pt_regs *regs);
extern unsigned long current_stub_stack(void);
extern struct mm_id *current_mm_id(void);
extern void current_mm_sync(void);
void initial_jmpbuf_lock(void);
void initial_jmpbuf_unlock(void);

#endif
