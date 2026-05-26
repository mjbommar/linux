/* SCM_RIGHTS fd-passing round-trip across socketpair.
 *
 * E.3d.0 will pass the worker's stub-child syscall_fd_map[] back to
 * the spawner over the per-mm UNIX socket via SCM_RIGHTS. Verify the
 * host kernel honours sendmsg/recvmsg(SCM_RIGHTS) on a stream
 * socketpair — if UML diverges from host glibc semantics here, we
 * find out before E.3d.0 ships.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		printf("REPRO: scm_rights_fdpass FAIL socketpair errno=%d\n", errno);
		return 0;
	}

	int pipefd[2];
	if (pipe(pipefd) < 0) {
		printf("REPRO: scm_rights_fdpass FAIL pipe errno=%d\n", errno);
		return 0;
	}

	char ctl[CMSG_SPACE(sizeof(int))];
	memset(ctl, 0, sizeof(ctl));
	struct iovec iov = { .iov_base = "x", .iov_len = 1 };
	struct msghdr m = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = ctl, .msg_controllen = sizeof(ctl),
	};
	struct cmsghdr *c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type  = SCM_RIGHTS;
	c->cmsg_len   = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &pipefd[1], sizeof(int));

	if (sendmsg(sv[0], &m, 0) != 1) {
		printf("REPRO: scm_rights_fdpass FAIL sendmsg errno=%d\n", errno);
		return 0;
	}

	char ctl2[CMSG_SPACE(sizeof(int))];
	memset(ctl2, 0, sizeof(ctl2));
	char buf;
	struct iovec iov2 = { .iov_base = &buf, .iov_len = 1 };
	struct msghdr m2 = {
		.msg_iov = &iov2, .msg_iovlen = 1,
		.msg_control = ctl2, .msg_controllen = sizeof(ctl2),
	};
	if (recvmsg(sv[1], &m2, 0) != 1) {
		printf("REPRO: scm_rights_fdpass FAIL recvmsg errno=%d\n", errno);
		return 0;
	}

	struct cmsghdr *c2 = CMSG_FIRSTHDR(&m2);
	if (!c2 || c2->cmsg_level != SOL_SOCKET || c2->cmsg_type != SCM_RIGHTS) {
		printf("REPRO: scm_rights_fdpass FAIL no_cmsg\n");
		return 0;
	}
	int got;
	memcpy(&got, CMSG_DATA(c2), sizeof(got));

	if (write(got, "ok", 2) != 2) {
		printf("REPRO: scm_rights_fdpass FAIL write_received_fd errno=%d\n",
			errno);
		return 0;
	}
	char rb[3] = { 0 };
	if (read(pipefd[0], rb, 2) != 2 || rb[0] != 'o' || rb[1] != 'k') {
		printf("REPRO: scm_rights_fdpass FAIL pipe_readback\n");
		return 0;
	}

	printf("REPRO: scm_rights_fdpass PASS got_fd=%d\n", got);
	return 0;
}
