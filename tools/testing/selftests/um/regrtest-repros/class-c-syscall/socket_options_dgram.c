/* Class C syscall repro: IP_PKTINFO setsockopt/getsockopt round-trip. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>

int main(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		printf("REPRO: socket_options_dgram FAIL socket_errno=%d\n", errno);
		return 0;
	}

	int on = 1;
	if (setsockopt(s, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) < 0) {
		printf("REPRO: socket_options_dgram FAIL setsockopt_errno=%d\n", errno);
		close(s);
		return 0;
	}

	int got = 0;
	socklen_t got_len = sizeof(got);
	if (getsockopt(s, IPPROTO_IP, IP_PKTINFO, &got, &got_len) < 0) {
		printf("REPRO: socket_options_dgram FAIL getsockopt_errno=%d\n", errno);
		close(s);
		return 0;
	}
	close(s);

	if (got != 1) {
		printf("REPRO: socket_options_dgram FAIL roundtrip got=%d expected=1\n", got);
		return 0;
	}

	printf("REPRO: socket_options_dgram PASS\n");
	return 0;
}
