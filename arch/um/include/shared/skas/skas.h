/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __SKAS_H
#define __SKAS_H

#include <sysdep/ptrace.h>

struct mm_struct;

extern void new_thread_handler(void);
extern void handle_syscall(struct uml_pt_regs *regs);
extern unsigned long current_stub_stack(void);
extern struct mm_id *current_mm_id(void);
extern void current_mm_sync(void);
extern unsigned int unscheduled_userspace_iterations;
void initial_jmpbuf_lock(void);
void initial_jmpbuf_unlock(void);

/*
 * Walks the per-process mm_list (private to arch/um/kernel/skas/mmu.c)
 * and applies a SIGKILL+wait4 sweep across every stub child. The
 * respawn helper then recreates those stubs with start_userspace_redo.
 * Used by the template-pause fork-on-resume loop to eliminate
 * stub-pid aliasing across a fork(2) of the host UML process.
 *
 * Both helpers may block (kill is non-blocking, wait is not, and
 * start_userspace performs a futex round-trip). Callers must be in
 * process context and must not hold locks that conflict with
 * mm_list_lock.
 *
 * Safe-to-call invariant: mm_list must be quiescent, with no
 * concurrent init_new_context or destroy_context. The template-pause
 * caller achieves this by gating UML signal dispatch and by being the
 * only userspace syscall in flight.
 */
extern int um_skas_teardown_all_stubs(void);
extern int um_skas_respawn_all_stubs(void);

/*
 * Child-only stub forget. Used after fork(2) in the forked child to
 * disclaim the inherited stub-child pids, which are owned by the
 * parent, without killing them.
 *
 * Marks every mm's id.pid = -1 and zeros its stub_data round-trip
 * fields so start_userspace_redo can clone-and-handshake fresh.
 * The original stub-child host processes stay alive in the parent;
 * the child gets a fresh tree.
 */
extern int um_skas_forget_all_stubs(void);

/*
 * um_skas_disown_inherited() - post-fork helper for pool-member children.
 *
 * Forgets the master's stub pid/socket references so the child can spawn
 * fresh stubs with start_userspace_fresh().  Returns the disowned-entry count
 * or a negative errno.
 */
extern int um_skas_disown_inherited(void);

/*
 * um_skas_force_resync_mm() - mark every present PTE in @mm as
 * needing sync, then call um_tlb_sync. Pushes all of @mm's
 * existing pages to the active backend's stub via
 * mm_region_added.  Used by template_pause's pool-member fork
 * loop after spawning a fresh stub.
 */
extern int um_skas_force_resync_mm(struct mm_struct *mm);

/*
 * um_skas_other_mm_mid_syscall() - detect unsafe concurrent stub work.
 *
 * Returns true if any mm in mm_list other than @caller has
 * stub_data->futex == FUTEX_IN_KERN, meaning it is currently parked
 * in the kernel-handles-syscall half of the stub round-trip.
 *
 * Used by template_pause's fork-safety check to refuse the fork path
 * when another guest task is mid-syscall. Tearing down that task's
 * stub would leave it parked forever, indistinguishable from a stub
 * crash.
 *
 * @caller may be NULL (no caller to exempt).
 */
/* Returns 1 if a mid-syscall mm was found, 0 otherwise. */
extern int um_skas_other_mm_mid_syscall(struct mm_id *caller);

#endif
