/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host-side io_uring substrate.  See arch/um/os-Linux/io_uring.c.
 *
 * Consumers:
 *   * arch/um/drivers/ubd_kern.c for block I/O.
 *   * fs/hostfs/hostfs_user.c for hostfs writeback.
 *
 * Returns NULL from os_io_ring_create() if io_uring_setup fails
 * (host kernel < 5.1 or seccomp-blocked); callers fall back to the
 * synchronous helper-thread path.
 */
#ifndef __UM_OS_IO_RING_H__
#define __UM_OS_IO_RING_H__

#include <linux/types.h>

struct iovec;
struct os_io_ring;

struct os_io_cqe {
	__u64 user_data;
	int   res;	/* >=0 bytes / 0 success; <0 -errno */
};

struct os_io_ring *os_io_ring_create(unsigned int entries);
void os_io_ring_destroy(struct os_io_ring *ring);

int os_io_ring_submit_pread(struct os_io_ring *ring, int fd, void *buf,
			    size_t len, unsigned long long off,
			    __u64 user_data);
int os_io_ring_submit_pwrite(struct os_io_ring *ring, int fd,
			     const void *buf, size_t len,
			     unsigned long long off, __u64 user_data);
int os_io_ring_submit_preadv(struct os_io_ring *ring, int fd,
			     const struct iovec *iov, int iovcnt,
			     unsigned long long off, __u64 user_data);
int os_io_ring_submit_pwritev(struct os_io_ring *ring, int fd,
			      const struct iovec *iov, int iovcnt,
			      unsigned long long off, __u64 user_data);
int os_io_ring_submit_fsync(struct os_io_ring *ring, int fd,
			    __u64 user_data);

int os_io_ring_peek_cqe(struct os_io_ring *ring, struct os_io_cqe *out);
int os_io_ring_wait_cqe(struct os_io_ring *ring, struct os_io_cqe *out,
			int timeout_ms);

unsigned int os_io_ring_in_flight(const struct os_io_ring *ring);

/*
 * Attach an eventfd to the ring. The host kernel signals it on every
 * CQE. The eventfd can be added to UML's global epollfd loop so all
 * io_uring consumers' completions flow through one thread.
 *
 * Returns 0 on success and stores the eventfd in *out_fd; the caller
 * owns it and must close it on ring tear-down. Returns -errno on
 * failure (older host kernel without IORING_REGISTER_EVENTFD, or
 * eventfd blocked by seccomp).
 */
int os_io_ring_register_eventfd(struct os_io_ring *ring, int *out_fd);

/*
 * Close every io_uring fd in the calling process's fd table,
 * identified by /proc/self/fd/<n> symlink target containing
 * "io_uring".  Used by the pool-member fork-child entry to drop
 * inherited io_uring task_work associations before any signal-
 * sensitive operation. Returns the number of fds closed, or -errno on
 * opendir failure.
 */
int os_close_inherited_io_uring_fds(void);

#endif /* __UM_OS_IO_RING_H__ */
