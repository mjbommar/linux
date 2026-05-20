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

/*
 * Memo 09 Phase 2a — bulk stub teardown / respawn.
 *
 * Walks the per-process mm_list (private to arch/um/kernel/skas/mmu.c)
 * and applies a SIGKILL+wait4 sweep across every stub child, then a
 * start_userspace_redo sweep to re-create them.  Used by the template-
 * pause fork-on-resume loop to eliminate stub-pid aliasing across a
 * fork(2) of the host UML process.
 *
 * Both helpers MAY block (kill is non-blocking but the wait isn't,
 * and start_userspace performs a futex round-trip).  Callers must be
 * in process context and must hold no lock that conflicts with
 * mm_list_lock.
 *
 * Safe-to-call invariant: mm_list must be quiescent — no concurrent
 * init_new_context or destroy_context.  The template-pause caller
 * achieves this by gating UML signal dispatch
 * (os_snapshot_block_iter_signals) and being the only userspace
 * syscall in flight (its own write to /proc/um/template_pause).
 */
extern int um_skas_teardown_all_stubs(void);
extern int um_skas_respawn_all_stubs(void);

#endif
