// Install a SIGABRT handler that captures the exact RIP that called
// __stack_chk_fail. That tells us which glibc function failed
// canary check.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ucontext.h>
#include <sys/wait.h>

static int log_fd = -1;

static void abort_handler(int sig, siginfo_t *info, void *ucv) {
	ucontext_t *uc = (ucontext_t *)ucv;
	unsigned long rip = uc->uc_mcontext.gregs[REG_RIP];
	unsigned long rsp = uc->uc_mcontext.gregs[REG_RSP];
	unsigned long rbp = uc->uc_mcontext.gregs[REG_RBP];
	char buf[160];
	int n = snprintf(buf, sizeof buf,
	    "ABORT_RIP=0x%lx RSP=0x%lx RBP=0x%lx sig=%d\n",
	    rip, rsp, rbp, sig);
	if (log_fd >= 0) write(log_fd, buf, n);
	/* Don't return - let the default action kill us. */
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(void) {
	log_fd = open("/tmp/locate.log", O_TRUNC|O_CREAT|O_WRONLY, 0644);

	struct sigaction sa = {0};
	sa.sa_sigaction = abort_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGABRT, &sa, NULL);

	pid_t p = fork();
	if (p == 0) {
		printf("CHILD_OK\n");
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(p, &st, 0);
	dprintf(log_fd, "PARENT_DONE st=0x%x\n", st);
	close(log_fd);
	return 0;
}
