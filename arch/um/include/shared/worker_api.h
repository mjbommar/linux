/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML per-mm host worker process API (memo 25 R4 / memo 28).
 *
 * The "spawner" is the original UML host process — owns control-plane
 * state, signals, the seccomp probe, and the syscall-dispatch loop.
 * Each guest mm gets its own "worker" process: separate VA space
 * (clone without CLONE_VM), separate signal table (no CLONE_SIGHAND
 * back to spawner), and — under v2 — its own KVM context.
 *
 * Today's seccomp model with one stub-child-per-mm running in the
 * spawner's VA space is the WORKER_PROCESS=n path. WORKER_PROCESS=y
 * splits each mm into its own worker process. Either compiles; the
 * runtime split happens when memo 28's E.6 commit flips the
 * defconfig toggle.
 *
 * The IPC wire format between spawner and worker is in
 * arch/um/backend/seccomp/worker_ipc.h.
 */
#ifndef __SHARED_UM_WORKER_API_H
#define __SHARED_UM_WORKER_API_H

#include <linux/errno.h>

struct mm_struct;
struct mm_id;
struct um_worker;	/* opaque to spawner code; defined in
			 * arch/um/os-Linux/spawner.c */
struct worker_msg;

#ifdef CONFIG_UM_WORKER_PROCESS

/*
 * Spawner lifecycle. Called from arch/um/os-Linux/main.c init /
 * exit paths.
 */
int  spawner_init(void);
void spawner_shutdown(void);

/*
 * Per-mm worker lifecycle. Wired from
 * arch/um/backend/seccomp/mm.c::seccomp_mm_create when
 * CONFIG_UM_WORKER_PROCESS=y.
 *
 * spawn_worker_for_mm creates the worker process for the given mm,
 * stashes the resulting handle in mm->context.worker, and returns 0
 * on success or a negative errno on failure. Callers must hold no
 * locks; spawner_init must have completed.
 *
 * reap_worker_for_mm tears down the worker (signal SIGTERM, wait
 * for SIGCHLD, free IPC channel). Idempotent.
 */
int  spawn_worker_for_mm(struct mm_struct *mm);
void reap_worker_for_mm(struct mm_struct *mm);

/*
 * Bring up the per-mm worker AND its stub child (memo 28 E.3d.0).
 * On success populates `*id_out` (including id_out->sock from the
 * worker's SCM_RIGHTS reply) and starts the dispatcher kthread.
 * On failure returns a negative errno; mm->context.worker is NULL
 * so the caller can fall back to the legacy in-spawner path.
 */
int worker_alloc_stub_for_mm(struct mm_struct *mm, struct mm_id *id_out);

/*
 * Send one worker_msg over the per-mm IPC socket. E.3d's seccomp
 * integration is the production caller. Returns 0 on success or a
 * negative errno (-ENODEV if the mm has no worker; -EIO on short
 * write).
 */
int worker_send_msg_for_mm(struct mm_struct *mm, const struct worker_msg *msg);

/*
 * worker_run_pending_syscalls — drain SYSCALL_REQs queued by the
 * dispatcher and execute them under the calling task's `current`
 * (memo 28 Part C.E "wait-queue bounce" / E.3d.1).
 *
 * Why this exists: the per-worker dispatcher kthread (E.3c) cannot
 * call handle_syscall directly. handle_syscall's callees deref
 * `current` heavily — sys_call_table[] entries pull credentials,
 * files, fs, signals, ns, seccomp, ptrace from current — and the
 * dispatcher's task_struct is a kthread, not the originating guest
 * task. The dispatcher therefore enqueues each SYSCALL_REQ on the
 * worker's `pending_reqs` list and wakes `reply_wait`; the
 * originating guest task (E.3d.2: the vcpu_run-equivalent loop)
 * calls this function, which fields the request under its own real
 * `current`, runs handle_syscall, and ships SYSCALL_REP back over
 * the IPC socket via worker_send_msg_for_mm.
 *
 * Blocks in wait_event_interruptible until either a request is
 * queued or the worker's dispatcher exits. Loops forever; returns
 * only on:
 *   -EINTR    a signal arrived (caller can re-enter)
 *   -ENODEV   the mm has no worker, or the dispatcher has exited
 *
 * E.3d.1 lands this API but no production caller invokes it yet —
 * E.3d.2 reroutes vcpu_run through it.
 */
int worker_run_pending_syscalls(struct mm_struct *mm);

/*
 * E.3b smoke test entry point. Spawns a throwaway worker, drives the
 * STUB_ALLOC_REQ → WRITE_REGS → RETURN_VALUE → WRITE_REGS_ACK
 * round-trip, verifies the sentinel echoes, reaps. Not called from
 * any production path; documented entry point for E.3c.
 */
int worker_smoke_test(void);

#else /* !CONFIG_UM_WORKER_PROCESS */

static inline int  spawner_init(void)              { return 0; }
static inline void spawner_shutdown(void)          { }
static inline int  spawn_worker_for_mm(struct mm_struct *mm) { return 0; }
static inline void reap_worker_for_mm(struct mm_struct *mm)  { }
static inline int  worker_alloc_stub_for_mm(struct mm_struct *mm,
					    struct mm_id *id_out)
{
	return -ENODEV;
}
static inline int  worker_send_msg_for_mm(struct mm_struct *mm,
					  const struct worker_msg *msg)
{
	return -ENODEV;
}
static inline int  worker_run_pending_syscalls(struct mm_struct *mm)
{
	return -ENODEV;
}
static inline int  worker_smoke_test(void) { return -ENODEV; }

#endif /* CONFIG_UM_WORKER_PROCESS */

#endif /* __SHARED_UM_WORKER_API_H */
