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
#ifndef __SHARED_UM_WORKER_IPC_H
#define __SHARED_UM_WORKER_IPC_H

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

/*
 * E.3b ABI: STUB_ALLOC_REQ / WRITE_REGS / RETURN_VALUE / WRITE_REGS_ACK
 * are the minimum-viable cut from memo 28 Part K.6. They prove the
 * spawner→worker→spawner round-trip without yet integrating real
 * start_userspace() calls or SIGSYS handling — that's E.3c.
 */
#define WORKER_REGS_SLOTS	9u	/* rip,rsp,rax,rdi,rsi,rdx,r10,r8,r9 */

enum worker_msg_type {
	WORKER_MSG_NONE             = 0,
	WORKER_MSG_SYSCALL_REQ      = 1,	/* worker → spawner: do this syscall */
	WORKER_MSG_SYSCALL_REP      = 2,	/* spawner → worker: here's the return */
	WORKER_MSG_SIGNAL           = 3,	/* spawner → worker: deliver signal */
	WORKER_MSG_MIGRATE_TO       = 4,	/* spawner → worker: receive task */
	WORKER_MSG_MIGRATE_ACK      = 5,	/* worker → spawner: migration done */
	WORKER_MSG_QUIESCE_REQ      = 6,	/* spawner → worker: stop scheduling */
	WORKER_MSG_QUIESCE_REP      = 7,	/* worker → spawner: quiesced */
	WORKER_MSG_SHUTDOWN         = 8,	/* spawner → worker: clean exit */
	WORKER_MSG_STUB_ALLOC_REQ   = 9,	/* spawner → worker: mm_id snapshot, alloc local stub */
	WORKER_MSG_WRITE_REGS       = 10,	/* spawner → worker: synthetic regs */
	WORKER_MSG_RETURN_VALUE     = 11,	/* spawner → worker: sentinel for stub reply */
	WORKER_MSG_WRITE_REGS_ACK   = 12,	/* worker → spawner: regs round-trip ACK */
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

/*
 * Snapshot of struct mm_id (arch/um/include/shared/skas/mm_id.h) sent
 * from spawner to worker so the worker can populate a local mm_id and
 * drive the seccomp futex round-trip itself. See memo 28 Part K.2
 * (option γ: pass via IPC, no struct refactor).
 */
struct worker_msg_stub_alloc {
	worker_u64 stack;		/* host VA of stub_data page */
	worker_u32 pid;			/* echo-only in E.3b; worker overwrites post-spawn */
	worker_u32 syscall_data_len;
	worker_u32 sock;		/* parent-side sockpair fd */
	worker_u32 syscall_fd_num;
	worker_u32 syscall_fd_map[4];	/* matches STUB_MAX_FDS */
	worker_u64 reserved[6];
};

/*
 * Compact regs frame for E.3b's round-trip. Not the full uml_pt_regs
 * (that's MAX_REG_NR * 8 bytes — too large for the 112-byte ceiling).
 * E.3c will define a richer encoding once handle_syscall integration
 * lands; for now WORKER_REGS_SLOTS holds the 9 slots set_stub_state
 * needs to drive a simulated stub child reply.
 */
struct worker_msg_regs {
	worker_u64 slot[9];		/* WORKER_REGS_SLOTS; slot[2] = rax/sentinel */
	worker_u64 reserved[5];
};

struct worker_msg_retval {
	worker_u64 sentinel;		/* worker writes this into local regs->ax */
	worker_u64 reserved[13];
};

struct worker_msg {
	worker_u32 magic;	/* WORKER_IPC_MAGIC */
	worker_u16 type;	/* enum worker_msg_type */
	worker_u16 flags;
	worker_u64 task_handle;
	union {
		struct worker_msg_syscall    syscall;
		struct worker_msg_reply      reply;
		struct worker_msg_signal     signal;
		struct worker_msg_migrate    migrate;
		struct worker_msg_stub_alloc stub_alloc;
		struct worker_msg_regs       regs;
		struct worker_msg_retval     retval;
		worker_u8                    pad[112];
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

#endif /* __SHARED_UM_WORKER_IPC_H */
