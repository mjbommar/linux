// SPDX-License-Identifier: GPL-2.0
/*
 * Host-side io_uring substrate for UML drivers.
 *
 * Per Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-
 * next-sprint/02-ubd-io-uring.md (memo 02, Phase 1).
 *
 * Minimal raw-syscall interface (no liburing dependency, by convention
 * for arch/um/os-Linux/).  Provides the same shape io_uring callers
 * have come to expect:
 *
 *   - os_io_ring_create(N)                  : set up a ring with N SQEs
 *   - os_io_ring_destroy(ring)              : tear down
 *   - os_io_ring_submit_pread/pwrite/...    : enqueue one SQE
 *   - os_io_ring_wait_cqe(ring, *cqe, ms)   : harvest one CQE
 *   - os_io_ring_peek_cqe(ring, *cqe)       : non-blocking harvest
 *
 * The user_data field of the SQE/CQE is opaque to this module; callers
 * encode req+segment identity into it.  UBD's converted do_io() (memo
 * 02 Phase 2) is the first consumer; hostfs writeback (memo 03 Phase
 * 2) reuses the same substrate.
 *
 * Fallback:  os_io_ring_create() returns NULL on EPERM/ENOSYS so
 * callers can demote to the legacy synchronous helper-thread path.
 *
 * Compiled as a USER_OBJ (this directory's convention).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <linux/io_uring.h>

#include <os_io_ring.h>

/*
 * Helper that does the raw syscall.  We avoid glibc's wrapper to keep
 * the dependency on a recent glibc out of the picture; UML routinely
 * builds against older user-space.
 */
static inline int sys_io_uring_setup(unsigned int entries,
				     struct io_uring_params *p)
{
	return (int)syscall(__NR_io_uring_setup, entries, p);
}

static inline int sys_io_uring_enter(int fd, unsigned int to_submit,
				     unsigned int min_complete,
				     unsigned int flags, void *arg,
				     size_t argsz)
{
	return (int)syscall(__NR_io_uring_enter, fd, to_submit,
			    min_complete, flags, arg, argsz);
}

struct os_io_ring {
	int		fd;

	/* SQ ring */
	void		*sq_mmap;
	size_t		sq_mmap_size;
	unsigned int	sq_entries;
	__u32		*sq_head;
	__u32		*sq_tail;
	__u32		*sq_ring_mask;
	__u32		*sq_array;

	/* SQEs */
	struct io_uring_sqe *sqes;
	size_t		sqes_mmap_size;

	/* CQ ring */
	void		*cq_mmap;
	size_t		cq_mmap_size;
	unsigned int	cq_entries;
	__u32		*cq_head;
	__u32		*cq_tail;
	__u32		*cq_ring_mask;
	struct io_uring_cqe *cqes;

	/* Bookkeeping for callers. */
	unsigned int	in_flight;
};

struct os_io_ring *os_io_ring_create(unsigned int entries)
{
	struct os_io_ring *r;
	struct io_uring_params p;
	int rc;

	r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;

	memset(&p, 0, sizeof(p));
	r->fd = sys_io_uring_setup(entries, &p);
	if (r->fd < 0) {
		/*
		 * EPERM: io_uring blocked by seccomp / system policy.
		 * ENOSYS: host kernel < 5.1.
		 * Either way callers should fall back to the legacy path.
		 */
		free(r);
		return NULL;
	}

	r->sq_entries = p.sq_entries;
	r->cq_entries = p.cq_entries;

	/* mmap the SQ ring */
	r->sq_mmap_size = p.sq_off.array +
			  p.sq_entries * sizeof(__u32);
	r->sq_mmap = mmap(NULL, r->sq_mmap_size,
			  PROT_READ | PROT_WRITE,
			  MAP_SHARED | MAP_POPULATE,
			  r->fd, IORING_OFF_SQ_RING);
	if (r->sq_mmap == MAP_FAILED)
		goto err_close;

	r->sq_head      = (__u32 *)((char *)r->sq_mmap + p.sq_off.head);
	r->sq_tail      = (__u32 *)((char *)r->sq_mmap + p.sq_off.tail);
	r->sq_ring_mask = (__u32 *)((char *)r->sq_mmap + p.sq_off.ring_mask);
	r->sq_array     = (__u32 *)((char *)r->sq_mmap + p.sq_off.array);

	/* mmap the SQEs */
	r->sqes_mmap_size = p.sq_entries * sizeof(struct io_uring_sqe);
	r->sqes = mmap(NULL, r->sqes_mmap_size,
		       PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_POPULATE,
		       r->fd, IORING_OFF_SQES);
	if (r->sqes == MAP_FAILED)
		goto err_unmap_sq;

	/* mmap the CQ ring */
	r->cq_mmap_size = p.cq_off.cqes +
			  p.cq_entries * sizeof(struct io_uring_cqe);
	r->cq_mmap = mmap(NULL, r->cq_mmap_size,
			  PROT_READ | PROT_WRITE,
			  MAP_SHARED | MAP_POPULATE,
			  r->fd, IORING_OFF_CQ_RING);
	if (r->cq_mmap == MAP_FAILED)
		goto err_unmap_sqes;

	r->cq_head      = (__u32 *)((char *)r->cq_mmap + p.cq_off.head);
	r->cq_tail      = (__u32 *)((char *)r->cq_mmap + p.cq_off.tail);
	r->cq_ring_mask = (__u32 *)((char *)r->cq_mmap + p.cq_off.ring_mask);
	r->cqes         = (struct io_uring_cqe *)
				((char *)r->cq_mmap + p.cq_off.cqes);

	/*
	 * SQ ring's array[] maps entry-position-in-ring to SQE index.
	 * Identity-map it so position-N picks sqes[N].
	 */
	for (rc = 0; rc < (int)p.sq_entries; rc++)
		r->sq_array[rc] = rc;

	r->in_flight = 0;
	return r;

err_unmap_sqes:
	munmap(r->sqes, r->sqes_mmap_size);
err_unmap_sq:
	munmap(r->sq_mmap, r->sq_mmap_size);
err_close:
	close(r->fd);
	free(r);
	return NULL;
}

void os_io_ring_destroy(struct os_io_ring *r)
{
	if (!r)
		return;
	munmap(r->cq_mmap, r->cq_mmap_size);
	munmap(r->sqes, r->sqes_mmap_size);
	munmap(r->sq_mmap, r->sq_mmap_size);
	close(r->fd);
	free(r);
}

/*
 * Reserve one SQE and return a pointer to it.  Returns NULL if the SQ
 * ring is full; caller should harvest CQEs and retry.
 */
static struct io_uring_sqe *sqe_acquire(struct os_io_ring *r)
{
	__u32 head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
	__u32 tail = *r->sq_tail;

	if (tail - head >= r->sq_entries)
		return NULL;

	return &r->sqes[tail & *r->sq_ring_mask];
}

/*
 * Commit the SQE: advance the SQ tail (release ordering pairs with
 * the kernel's acquire-load) and call io_uring_enter to wake the
 * kernel-side worker.
 */
static int sqe_commit(struct os_io_ring *r)
{
	__u32 tail = *r->sq_tail;

	__atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
	r->in_flight++;

	if (sys_io_uring_enter(r->fd, 1, 0, 0, NULL, 0) < 0)
		return -errno;
	return 0;
}

int os_io_ring_submit_pread(struct os_io_ring *r, int fd, void *buf,
			    size_t len, unsigned long long off,
			    __u64 user_data)
{
	struct io_uring_sqe *sqe = sqe_acquire(r);

	if (!sqe)
		return -EAGAIN;

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_READ;
	sqe->fd     = fd;
	sqe->addr   = (__u64)(uintptr_t)buf;
	sqe->len    = (__u32)len;
	sqe->off    = off;
	sqe->user_data = user_data;
	return sqe_commit(r);
}

int os_io_ring_submit_pwrite(struct os_io_ring *r, int fd,
			     const void *buf, size_t len,
			     unsigned long long off, __u64 user_data)
{
	struct io_uring_sqe *sqe = sqe_acquire(r);

	if (!sqe)
		return -EAGAIN;

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_WRITE;
	sqe->fd     = fd;
	sqe->addr   = (__u64)(uintptr_t)buf;
	sqe->len    = (__u32)len;
	sqe->off    = off;
	sqe->user_data = user_data;
	return sqe_commit(r);
}

int os_io_ring_submit_preadv(struct os_io_ring *r, int fd,
			     const struct iovec *iov, int iovcnt,
			     unsigned long long off, __u64 user_data)
{
	struct io_uring_sqe *sqe = sqe_acquire(r);

	if (!sqe)
		return -EAGAIN;

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_READV;
	sqe->fd     = fd;
	sqe->addr   = (__u64)(uintptr_t)iov;
	sqe->len    = (__u32)iovcnt;
	sqe->off    = off;
	sqe->user_data = user_data;
	return sqe_commit(r);
}

int os_io_ring_submit_pwritev(struct os_io_ring *r, int fd,
			      const struct iovec *iov, int iovcnt,
			      unsigned long long off, __u64 user_data)
{
	struct io_uring_sqe *sqe = sqe_acquire(r);

	if (!sqe)
		return -EAGAIN;

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_WRITEV;
	sqe->fd     = fd;
	sqe->addr   = (__u64)(uintptr_t)iov;
	sqe->len    = (__u32)iovcnt;
	sqe->off    = off;
	sqe->user_data = user_data;
	return sqe_commit(r);
}

int os_io_ring_submit_fsync(struct os_io_ring *r, int fd, __u64 user_data)
{
	struct io_uring_sqe *sqe = sqe_acquire(r);

	if (!sqe)
		return -EAGAIN;

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_FSYNC;
	sqe->fd     = fd;
	sqe->user_data = user_data;
	return sqe_commit(r);
}

/*
 * Non-blocking CQE peek.  Returns 1 if a CQE was harvested, 0 if the
 * CQ ring is empty.
 */
int os_io_ring_peek_cqe(struct os_io_ring *r, struct os_io_cqe *out)
{
	__u32 head = *r->cq_head;
	__u32 tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
	struct io_uring_cqe *cqe;

	if (head == tail)
		return 0;

	cqe = &r->cqes[head & *r->cq_ring_mask];
	out->user_data = cqe->user_data;
	out->res       = cqe->res;

	__atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
	r->in_flight--;
	return 1;
}

/*
 * Blocking CQE wait.  timeout_ms < 0 = wait forever, 0 = poll once,
 * > 0 = wait up to N ms.  Returns 1 (cqe ready) / 0 (timeout) /
 * -errno.
 */
int os_io_ring_wait_cqe(struct os_io_ring *r, struct os_io_cqe *out,
			int timeout_ms)
{
	struct __kernel_timespec ts;
	struct io_uring_getevents_arg arg = { 0 };
	void *argp = NULL;
	size_t argsz = 0;
	int rc;

	/* Fast path: a CQE may already be sitting in the ring. */
	if (os_io_ring_peek_cqe(r, out) == 1)
		return 1;

	if (timeout_ms == 0)
		return 0;

	if (timeout_ms > 0) {
		ts.tv_sec  = timeout_ms / 1000;
		ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
		arg.ts = (__u64)(uintptr_t)&ts;
		argp = &arg;
		argsz = sizeof(arg);
	}

	rc = sys_io_uring_enter(r->fd, 0, 1,
				IORING_ENTER_GETEVENTS |
				(argp ? IORING_ENTER_EXT_ARG : 0),
				argp, argsz);
	if (rc < 0) {
		if (errno == ETIME || errno == EINTR)
			return 0;
		return -errno;
	}

	return os_io_ring_peek_cqe(r, out);
}

unsigned int os_io_ring_in_flight(const struct os_io_ring *r)
{
	return r->in_flight;
}
