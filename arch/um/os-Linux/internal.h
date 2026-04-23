/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_OS_LINUX_INTERNAL_H
#define __UM_OS_LINUX_INTERNAL_H

#include <mm_id.h>
#include <stub-data.h>
#include <signal.h>

/*
 * USER-TU-safe __READ_ONCE equivalent.
 *
 * <asm-generic/rwonce.h> pulls in <linux/kcsan-checks.h> when KCSAN
 * is enabled, which in turn references `struct list_head` that USER
 * TUs don't have forward-declared. Rather than pull kernel type
 * plumbing into host-userspace compiles, use the compiler-barrier
 * form directly. Semantically equivalent to __READ_ONCE for the
 * uses in os-Linux/skas/process.c; no KCSAN instrumentation, which
 * is correct for USER TUs (they aren't guest kernel code).
 */
#define UM_USER_READ_ONCE(x) (*(const volatile typeof(x) *)&(x))

/*
 * elf_aux.c
 */
void scan_elf_aux(char **envp);

/*
 * mem.c
 */
void check_tmpexec(void);

/*
 * signal.c
 */
extern __thread int signals_enabled;
int timer_alarm_pending(void);

/*
 * start_up.c / skas/process.c — `using_seccomp` intermediary.
 *
 * Defined in arch/um/os-Linux/skas/process.c. Written by
 * os_early_checks (start_up.c) based on the host seccomp
 * probe. Read by init_backend (arch/um/kernel/backend.c, has
 * its own extern) to pick the dynamic-mode backend.
 *
 * USER-TU-private by design as of 2026-04-23 (D59 Phase II
 * Lift #4d+). The public skas.h declaration was removed
 * after all twelve runtime readers in arch/um/os-Linux/
 * moved to um_backend_ops capability flags; do NOT add it
 * back — backend-discriminating runtime code should consult
 * um_backend->*flag instead.
 */
extern int using_seccomp;

/*
 * skas/process.c
 */
void wait_stub_done(int pid);
void wait_stub_done_seccomp(struct mm_id *mm_idp, int running, int wait_sigsys);

/*
 * smp.c
 */
#define IPI_SIGNAL	SIGRTMIN

#endif /* __UM_OS_LINUX_INTERNAL_H */
