/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host-side io_uring substrate.  See arch/um/os-Linux/io_uring.c.
 *
 * Per Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-
 * next-sprint/02-ubd-io-uring.md (memo 02, Phase 1).
 *
 * Consumers:
 *   * arch/um/drivers/ubd_kern.c (memo 02 Phase 2 — UBD io_uring).
 *   * fs/hostfs/hostfs_user.c (memo 03 Phase 2 — hostfs writeback).
 *
 * Returns NULL from os_io_ring_create() if io_uring_setup fails
 * (host kernel < 5.1 or seccomp-blocked); callers fall back to the
 * legacy synchronous helper-thread path.
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

#endif /* __UM_OS_IO_RING_H__ */
