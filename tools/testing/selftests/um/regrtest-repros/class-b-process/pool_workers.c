/* Class B process repro: multiprocessing.Pool analog. Parent forks N
 * workers connected via socketpair, dispatches tasks, collects
 * results. Stresses concurrent stub-child IPC + clean reaping. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <poll.h>

#define N_WORKERS 8
#define N_TASKS 80

struct task { int idx; long arg; };
struct result { int idx; long val; };

static ssize_t full_read(int fd, void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
		if (r == 0) return got;
		if (r < 0) { if (errno == EINTR) continue; return -1; }
		got += r;
	}
	return got;
}

static ssize_t full_write(int fd, const void *buf, size_t n)
{
	size_t put = 0;
	while (put < n) {
		ssize_t w = write(fd, (const char *)buf + put, n - put);
		if (w < 0) { if (errno == EINTR) continue; return -1; }
		put += w;
	}
	return put;
}

static void worker(int sock)
{
	for (;;) {
		struct task t;
		ssize_t r = full_read(sock, &t, sizeof(t));
		if (r == 0) _exit(0);
		if (r != sizeof(t)) _exit(1);
		if (t.idx < 0) _exit(0);
		struct result res = { .idx = t.idx, .val = t.arg * t.arg };
		if (full_write(sock, &res, sizeof(res)) != sizeof(res)) _exit(2);
	}
}

int main(void)
{
	int socks[N_WORKERS];
	pid_t pids[N_WORKERS];

	for (int i = 0; i < N_WORKERS; i++) {
		int sv[2];
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
			printf("REPRO: pool_workers FAIL socketpair_errno=%d\n", errno);
			return 0;
		}
		pid_t p = fork();
		if (p < 0) {
			printf("REPRO: pool_workers FAIL fork_errno=%d\n", errno);
			return 0;
		}
		if (p == 0) {
			close(sv[0]);
			worker(sv[1]);
			_exit(0);
		}
		close(sv[1]);
		socks[i] = sv[0];
		pids[i] = p;
	}

	long results[N_TASKS];
	int got_mask[N_TASKS];
	memset(got_mask, 0, sizeof(got_mask));

	int next_task = 0;
	int collected = 0;
	int inflight[N_WORKERS];
	memset(inflight, 0, sizeof(inflight));

	/* Prime: send first round */
	for (int w = 0; w < N_WORKERS && next_task < N_TASKS; w++) {
		struct task t = { .idx = next_task, .arg = next_task };
		if (full_write(socks[w], &t, sizeof(t)) != sizeof(t)) {
			printf("REPRO: pool_workers FAIL prime_write w=%d\n", w);
			return 0;
		}
		inflight[w] = 1;
		next_task++;
	}

	while (collected < N_TASKS) {
		struct pollfd pf[N_WORKERS];
		int n_pf = 0, idx_map[N_WORKERS];
		for (int w = 0; w < N_WORKERS; w++) {
			if (inflight[w]) {
				pf[n_pf].fd = socks[w];
				pf[n_pf].events = POLLIN;
				pf[n_pf].revents = 0;
				idx_map[n_pf] = w;
				n_pf++;
			}
		}
		if (n_pf == 0) break;
		int pr = poll(pf, n_pf, 5000);
		if (pr < 0) {
			if (errno == EINTR) continue;
			printf("REPRO: pool_workers FAIL poll_errno=%d\n", errno);
			return 0;
		}
		if (pr == 0) {
			printf("REPRO: pool_workers FAIL poll_timeout collected=%d\n", collected);
			return 0;
		}
		for (int k = 0; k < n_pf; k++) {
			if (!(pf[k].revents & POLLIN)) continue;
			int w = idx_map[k];
			struct result res;
			if (full_read(socks[w], &res, sizeof(res)) != sizeof(res)) {
				printf("REPRO: pool_workers FAIL res_read w=%d\n", w);
				return 0;
			}
			if (res.idx < 0 || res.idx >= N_TASKS) {
				printf("REPRO: pool_workers FAIL bad_idx=%d\n", res.idx);
				return 0;
			}
			results[res.idx] = res.val;
			got_mask[res.idx] = 1;
			collected++;
			inflight[w] = 0;
			if (next_task < N_TASKS) {
				struct task t = { .idx = next_task, .arg = next_task };
				if (full_write(socks[w], &t, sizeof(t)) != sizeof(t)) {
					printf("REPRO: pool_workers FAIL dispatch_w=%d\n", w);
					return 0;
				}
				inflight[w] = 1;
				next_task++;
			}
		}
	}

	/* Shutdown: send sentinel and close. */
	for (int w = 0; w < N_WORKERS; w++) {
		struct task t = { .idx = -1, .arg = 0 };
		(void)full_write(socks[w], &t, sizeof(t));
		close(socks[w]);
	}

	int reaped = 0, bad = 0;
	for (int w = 0; w < N_WORKERS; w++) {
		int status = 0;
		pid_t r = waitpid(pids[w], &status, 0);
		if (r != pids[w]) { bad++; continue; }
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) bad++;
		else reaped++;
	}

	int correct = 1;
	for (int i = 0; i < N_TASKS; i++) {
		if (!got_mask[i] || results[i] != (long)i * i) { correct = 0; break; }
	}

	if (correct && reaped == N_WORKERS && bad == 0)
		printf("REPRO: pool_workers PASS tasks=%d workers=%d\n", N_TASKS, N_WORKERS);
	else
		printf("REPRO: pool_workers FAIL correct=%d reaped=%d bad=%d collected=%d\n",
			correct, reaped, bad, collected);
	return 0;
}
