/* AF_INET SOCK_DGRAM IPPROTO_UDPLITE.
 *
 * UDP-Lite was retired from the kernel in commit 56520b398e5e
 * ("ipv4: Retire UDP-Lite."). On post-retirement trees, socket()
 * with IPPROTO_UDPLITE = 136 now fails with EPROTONOSUPPORT (errno 93).
 * That's the permanent expected behavior, not a UML bug — emit
 * EXPECTED_FAIL so the substrate gate stays clean. test_socket's
 * 41 UDPLITE subtests need an upstream skip in the regrtest -x list.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifndef IPPROTO_UDPLITE
#define IPPROTO_UDPLITE 136
#endif

int main(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDPLITE);
	int e = errno;
	if (s < 0) {
		if (e == EPROTONOSUPPORT) {
			printf("REPRO: socket_udplite EXPECTED_FAIL retired_upstream errno=%d\n",
				e);
			return 0;
		}
		printf("REPRO: socket_udplite FAIL unexpected_errno=%d\n", e);
		return 0;
	}
	close(s);
	printf("REPRO: socket_udplite PASS\n");
	return 0;
}
