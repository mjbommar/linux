// SPDX-License-Identifier: GPL-2.0
//
// Minimal guest-side TCP sender for the Memo 01 Step 2 throughput
// bench.  Removes the Python GIL / per-loop overhead that the
// Python sender pays — exposes the true driver-bound throughput.
//
// Build (host): gcc -O2 -o tcp-send tools/testing/selftests/um/net-bench/tcp-send.c
// Invoke (guest):  tcp-send HOST_IP PORT DURATION_SEC
// Output:  one line, "BENCH_RESULT mbps=%.1f bytes=%lld dt=%.2f\n"
//
// Uses sendfile()-style large writes (1 MiB buffer of 'X') so the
// TCP stack sees the maximum possible single skb size.  With
// NETIF_F_TSO advertised on the device, the kernel coalesces
// these into large GSO skbs that vec2's fd transport hands off
// to the host kernel as one virtio_net_hdr-tagged write.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE (1 << 20)

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	const char *host;
	int port, duration;
	int fd;
	int one = 1;
	struct sockaddr_in addr;
	char *buf;
	long long total = 0;
	double t0, t1, mbps;

	if (argc != 4) {
		fprintf(stderr, "usage: %s HOST PORT DURATION_SEC\n", argv[0]);
		return 2;
	}
	host = argv[1];
	port = atoi(argv[2]);
	duration = atoi(argv[3]);

	buf = malloc(BUF_SIZE);
	if (!buf) {
		perror("malloc");
		return 1;
	}
	memset(buf, 'X', BUF_SIZE);

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		fprintf(stderr, "inet_pton: invalid host %s\n", host);
		return 1;
	}
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		return 1;
	}

	t0 = now_sec();
	while (now_sec() - t0 < (double)duration) {
		ssize_t n = send(fd, buf, BUF_SIZE, 0);

		if (n < 0) {
			perror("send");
			return 1;
		}
		total += n;
	}
	t1 = now_sec();
	mbps = (double)total * 8.0 / (t1 - t0) / 1e6;
	printf("BENCH_RESULT mbps=%.1f bytes=%lld dt=%.2f\n",
	       mbps, total, t1 - t0);
	close(fd);
	free(buf);
	return 0;
}
