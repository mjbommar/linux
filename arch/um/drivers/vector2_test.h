/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_VECTOR2_TEST_H
#define __UM_VECTOR2_TEST_H

#include <linux/errno.h>

#include <os.h>

/*
 * The inherited-fd multiqueue ABI uses base_fd + queue_index.  pipe(2)
 * normally returns adjacent descriptors, but UML has host helper threads that
 * may allocate an fd between the two allocations inside pipe(2).  Retry that
 * test setup race instead of making suite order decide whether the ABI tests
 * get the descriptor layout they are meant to exercise.
 */
static inline int um_vec2_test_adjacent_pipe(int *fds)
{
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < 100; attempt++) {
		ret = os_pipe(fds, 1, 1);
		if (ret)
			return ret;
		if (fds[1] == fds[0] + 1)
			return 0;

		os_close_file(fds[0]);
		os_close_file(fds[1]);
		fds[0] = -1;
		fds[1] = -1;
	}

	return -EAGAIN;
}

#endif /* __UM_VECTOR2_TEST_H */
