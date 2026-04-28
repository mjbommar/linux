/* Class C syscall repro: AF_INET SOCK_DGRAM IPPROTO_UDPLITE must succeed.
 * test_socket UDPLITE failures all trace to ENOPROTOOPT here — UML kernel
 * lacks CONFIG_IP_UDPLITE / udplite module. */
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
		printf("REPRO: socket_udplite FAIL errno=%d (Protocol_not_supported=%d)\n",
			e, EPROTONOSUPPORT);
		return 0;
	}
	close(s);
	printf("REPRO: socket_udplite PASS\n");
	return 0;
}
