/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Spawner ↔ worker IPC wire format (memo 25 R4 / memo 28 Part D).
 *
 * Fixed-size 128-byte binary messages over a per-worker UNIX socket
 * pair. SCM_RIGHTS rides on the cmsg channel for FD passing. Each
 * recvmsg() reads exactly one message.
 *
 * Protocol versioning: bump WORKER_IPC_MAGIC if the wire breaks.
 * Minor additions can use the `flags` field.
 *
 * Endianness: native (UML's spawner and worker run on the same
 * host architecture).
 *
 * This header is shared between the spawner (kernel TU) and the
 * worker (USER TU). Avoid kernel-only types here — use plain u32 /
 * u64 / u16 / u8 from <linux/types.h> in kernel context and the
 * matching uintN_t from <stdint.h> in USER context.
 */
#ifndef __UM_BACKEND_SECCOMP_WORKER_IPC_H
#define __UM_BACKEND_SECCOMP_WORKER_IPC_H

#ifdef __UM_HOST__
#include <stdint.h>
typedef uint64_t worker_u64;
typedef uint32_t worker_u32;
typedef uint16_t worker_u16;
typedef uint8_t  worker_u8;
#else
#include <linux/types.h>
typedef u64 worker_u64;
typedef u32 worker_u32;
typedef u16 worker_u16;
typedef u8  worker_u8;
#endif

#define WORKER_IPC_MAGIC	0x554D5734u	/* "UMW4" */
#define WORKER_MSG_SIZE		128u

enum worker_msg_type {
	WORKER_MSG_NONE          = 0,
	WORKER_MSG_SYSCALL_REQ   = 1,	/* worker → spawner: do this syscall */
	WORKER_MSG_SYSCALL_REP   = 2,	/* spawner → worker: here's the return */
	WORKER_MSG_SIGNAL        = 3,	/* spawner → worker: deliver signal */
	WORKER_MSG_MIGRATE_TO    = 4,	/* spawner → worker: receive task */
	WORKER_MSG_MIGRATE_ACK   = 5,	/* worker → spawner: migration done */
	WORKER_MSG_QUIESCE_REQ   = 6,	/* spawner → worker: stop scheduling */
	WORKER_MSG_QUIESCE_REP   = 7,	/* worker → spawner: quiesced */
	WORKER_MSG_SHUTDOWN      = 8,	/* spawner → worker: clean exit */
};

struct worker_msg_syscall {
	worker_u64 nr;
	worker_u64 args[6];
	worker_u64 reserved;
};

struct worker_msg_reply {
	worker_u64 retval;	/* sign-extended; negative = errno */
	worker_u64 reserved[7];
};

struct worker_msg_signal {
	worker_u32 sig;		/* signal number (1..31) */
	worker_u32 source;	/* opaque source tag for tracing */
	worker_u64 reserved[7];
};

struct worker_msg_migrate {
	worker_u64 src_task_handle;
	worker_u64 dst_task_handle;
	worker_u64 reserved[6];
};

struct worker_msg {
	worker_u32 magic;	/* WORKER_IPC_MAGIC */
	worker_u16 type;	/* enum worker_msg_type */
	worker_u16 flags;
	worker_u64 task_handle;
	union {
		struct worker_msg_syscall syscall;
		struct worker_msg_reply   reply;
		struct worker_msg_signal  signal;
		struct worker_msg_migrate migrate;
		worker_u8                 pad[112];
	} u;
};

/* Compile-time assertion that the wire is exactly 128 bytes. */
#ifdef __UM_HOST__
_Static_assert(sizeof(struct worker_msg) == WORKER_MSG_SIZE,
	       "worker_msg must be exactly 128 bytes on the wire");
#else
#include <linux/build_bug.h>
static inline void __worker_msg_size_check(void)
{
	BUILD_BUG_ON(sizeof(struct worker_msg) != WORKER_MSG_SIZE);
}
#endif

#endif /* __UM_BACKEND_SECCOMP_WORKER_IPC_H */
