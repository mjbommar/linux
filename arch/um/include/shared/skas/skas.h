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

/*
 * Phase 2a — child-only stub forget.  Used after fork(2) in the
 * forked child to disclaim the inherited stub-child pids (which
 * are owned by the parent, not the child) without killing them.
 * Marks every mm's id.pid = -1 and zeros its stub_data round-trip
 * fields so start_userspace_redo can clone-and-handshake fresh.
 * The original stub-child host processes stay alive in the parent;
 * the child gets a fresh tree.
 */
extern int um_skas_forget_all_stubs(void);

/*
 * um_skas_disown_inherited() — post-fork helper for pool-member
 * children.  Forgets master's stub_pid/sock references AND swaps
 * each mm_id->stack to a fresh __get_free_pages allocation so the
 * physmem fd phys_mapping() resolves is private to this child.
 * Must be paired with a subsequent um_skas_respawn_all_stubs() to
 * clone fresh stubs that use the new pages.  Returns disowned-
 * entry count or -ENOMEM.
 */
extern int um_skas_disown_inherited(void);

/*
 * um_skas_force_resync_mm() — mark every present PTE in @mm as
 * needing sync, then call um_tlb_sync.  Pushes all of @mm's
 * existing pages to the active backend's stub via
 * mm_region_added.  Used by template_pause's pool-member fork
 * loop after spawning a fresh stub.
 */
extern int um_skas_force_resync_mm(struct mm_struct *mm);

/*
 * um_skas_other_mm_mid_syscall() — Memo 09 Phase 2a defensive check.
 *
 * Returns true if any mm in mm_list other than @caller has
 * stub_data->futex == FUTEX_IN_KERN (i.e., is currently parked in
 * the kernel-handles-syscall half of the stub round-trip).
 *
 * Used by template_pause's assert_fork_safety to refuse the fork
 * path when another guest task is mid-syscall — tearing down its
 * stub now would leave it parked forever, indistinguishable from
 * a stub crash, and the existing mm_sigchld_irq path would fire
 * fatal_sigsegv on it.  See PHASE2A-DESIGN.md §3.5.
 *
 * @caller may be NULL (no caller to exempt).
 */
/* Returns 1 if a mid-syscall mm was found, 0 otherwise. */
extern int um_skas_other_mm_mid_syscall(struct mm_id *caller);

#endif
