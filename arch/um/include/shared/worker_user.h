/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Spawner-side host helpers for the per-mm worker process model.
 *
 * These functions are declared from arch/um/kernel/spawner.c but
 * defined in arch/um/os-Linux/worker_user.c, which is built with libc
 * available. To avoid ABI coupling, the helpers take plain ints rather
 * than touching the kernel-side struct um_worker layout.
 */
#ifndef __SHARED_UM_WORKER_USER_H
#define __SHARED_UM_WORKER_USER_H

/*
 * Spawn a worker process. On success returns 0 and writes:
 *   *out_pid  - worker's host pid
 *   *out_sock - spawner-side end of the IPC UNIX socketpair
 *               (worker holds the other end internally)
 * On failure returns a negative errno and *out_* are unmodified.
 */
int spawn_worker_process(int *out_pid, int *out_sock);

/*
 * Reap a worker process with SIGTERM + waitpid. Closes sock if >= 0.
 * Idempotent for pid <= 0.
 */
void reap_worker_process(int pid, int sock);

#endif /* __SHARED_UM_WORKER_USER_H */
