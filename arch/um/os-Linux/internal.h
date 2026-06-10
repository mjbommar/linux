/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_OS_LINUX_INTERNAL_H
#define __UM_OS_LINUX_INTERNAL_H

#include <mm_id.h>
#include <stub-data.h>
#include <signal.h>

/*
 * Host-built __READ_ONCE equivalent.
 *
 * <asm-generic/rwonce.h> pulls in <linux/kcsan-checks.h> when KCSAN
 * is enabled, which in turn references struct list_head that these
 * host-built files do not have forward-declared. Rather than pull
 * kernel type plumbing into host-userspace compiles, use the
 * compiler-barrier form directly. Semantically equivalent to
 * __READ_ONCE for the uses in os-Linux/skas/process.c; no KCSAN
 * instrumentation, which is correct for host-side UML code.
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
 * start_up.c / skas/process.c - using_seccomp handoff.
 *
 * Defined in arch/um/os-Linux/skas/process.c. Written by
 * os_early_checks (start_up.c) based on the host seccomp
 * probe. Read by init_backend() through the kernel-only backend
 * header to pick the dynamic-mode backend.
 *
 * USER-TU-private by design. The public skas.h declaration was removed
 * after runtime readers in arch/um/os-Linux moved to um_backend_ops
 * capability flags; do NOT add it back. Backend-discriminating runtime
 * code should consult
 * um_backend->*flag instead.
 */
extern int using_seccomp;

/*
 * skas/process.c
 */
void wait_stub_done(int pid);
void wait_stub_done_seccomp(struct mm_id *mm_idp, int running, int wait_sigsys);
void send_stub_syscall_fds(struct mm_id *mm_idp);

/*
 * smp.c
 */
#define IPI_SIGNAL	SIGRTMIN

#endif /* __UM_OS_LINUX_INTERNAL_H */
