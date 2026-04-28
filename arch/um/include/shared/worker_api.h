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
static inline int  worker_smoke_test(void) { return -ENODEV; }

#endif /* CONFIG_UM_WORKER_PROCESS */

#endif /* __SHARED_UM_WORKER_API_H */
