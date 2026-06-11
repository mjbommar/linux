// SPDX-License-Identifier: GPL-2.0
//
// Diagnostic version of malloc-stress-child: installs a SIGSEGV
// handler that dumps register state and surrounding memory when the
// child trips the deterministic glibc _int_malloc NULL deref at
// IP 0x4074ed (cmp %rdx, 0x10(%rdi); rdi=NULL).
//
// The dump tells us WHAT BYTES are wrong in glibc's heap arena:
// distinguishes:
//   - All zeros: page reused without re-zeroing.
//   - Random bytes resembling parent data: uninitialized page.
//   - Specific patterns (text, addresses): leak from sibling worker.
//   - Stack canary / glibc poison values: glibc hardening detected
//     pre-existing corruption
//
// Build:
//   cc -O0 -static -o malloc-stress-child-diag \
//      -DCHILD_MAIN malloc-stress-child-diag.c
// Use as a drop-in replacement for malloc-stress-child in the
// threaded-fork-malloc parent (rename child binary or symlink).

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static void hex_dump(const char *label, const void *addr, size_t len)
{
	const unsigned char *p = addr;
	size_t i;

	fprintf(stderr, "  %s @ %p:\n    ", label, addr);
	for (i = 0; i < len; i++) {
		fprintf(stderr, "%02x", p[i]);
		if ((i & 15) == 15 && i + 1 < len)
			fprintf(stderr, "\n    ");
		else if ((i & 7) == 7)
			fprintf(stderr, " ");
		else if ((i & 3) == 3)
			fprintf(stderr, " ");
	}
	fprintf(stderr, "\n");
}

static void segv_handler(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = (ucontext_t *)ucv;
	greg_t *regs = uc->uc_mcontext.gregs;
	void *cr2 = si->si_addr;
	greg_t rip = regs[REG_RIP];
	greg_t rdx = regs[REG_RDX];
	greg_t rdi = regs[REG_RDI];
	greg_t r11 = regs[REG_R11];
	greg_t rsi = regs[REG_RSI];

	fprintf(stderr,
		"MALLOC-STRESS-DIAG SIGSEGV: cr2=%p ip=0x%llx pid=%d\n"
		"  GPRs: rax=%llx rbx=%llx rcx=%llx rdx=%llx\n"
		"        rsi=%llx rdi=%llx rbp=%llx rsp=%llx\n"
		"        r8=%llx r9=%llx r10=%llx r11=%llx\n"
		"        r12=%llx r13=%llx r14=%llx r15=%llx\n",
		cr2, (unsigned long long)rip, getpid(),
		(unsigned long long)regs[REG_RAX],
		(unsigned long long)regs[REG_RBX],
		(unsigned long long)regs[REG_RCX],
		(unsigned long long)regs[REG_RDX],
		(unsigned long long)regs[REG_RSI],
		(unsigned long long)regs[REG_RDI],
		(unsigned long long)regs[REG_RBP],
		(unsigned long long)regs[REG_RSP],
		(unsigned long long)regs[REG_R8],
		(unsigned long long)regs[REG_R9],
		(unsigned long long)regs[REG_R10],
		(unsigned long long)regs[REG_R11],
		(unsigned long long)regs[REG_R12],
		(unsigned long long)regs[REG_R13],
		(unsigned long long)regs[REG_R14],
		(unsigned long long)regs[REG_R15]);

	/* Dump XMM register state to detect cross-task FPU leaks.
	 * If glibc's MOVUPS write of fd+bk lost the high half (bk = 0),
	 * then xmm0 at fault time may show the corruption.
	 * Note: by the time SIGSEGV handler runs, the user code may have
	 * advanced past the MOVUPS, but XMM0..XMM15 should still have
	 * the values from the failed write context. */
	{
		struct _libc_fpstate *fp = (struct _libc_fpstate *)uc->uc_mcontext.fpregs;
		if (fp) {
			int i;
			fprintf(stderr,
				"  FPU: cwd=%x swd=%x ftw=%x fop=%x mxcsr=%x mxcsr_mask=%x\n",
				fp->cwd, fp->swd, fp->ftw, fp->fop, fp->mxcsr, fp->mxcr_mask);
			for (i = 0; i < 8; i++) {
				fprintf(stderr,
					"  XMM%d: %08x %08x %08x %08x\n",
					i,
					fp->_xmm[i].element[0],
					fp->_xmm[i].element[1],
					fp->_xmm[i].element[2],
					fp->_xmm[i].element[3]);
			}
		} else {
			fprintf(stderr, "  FPU: ucontext->fpregs == NULL!\n");
		}
	}

	/* The chunk that we were walking. rdx is `victim` per the
	 * disassembly. Dump 64 bytes from rdx so we see the chunk
	 * header (size, prev_size) and the bk/fd pointers. */
	if (rdx > 0x100000ULL) {
		hex_dump("chunk @rdx (victim)", (void *)rdx, 64);
	}

	/* r11 in _int_malloc holds the arena (av) pointer. Dump first
	 * 128 bytes so we see arena header (mutex, flags, fastbins). */
	if (r11 > 0x100000ULL) {
		hex_dump("arena @r11 (av)", (void *)r11, 128);
	}

	/* rsi holds the requested size at function entry. */
	fprintf(stderr, "  alloc_size=%llu\n", (unsigned long long)rsi);

	(void)sig;
	fflush(stderr);
	_exit(177);  /* distinct exit code so parent can detect */
}

/*
 * SIGABRT handler: fires when glibc's heap-integrity check kills the
 * process via abort(). Captures the same register/XMM state as
 * segv_handler so we can diagnose the rare heap-corruption class
 * (~1/120000 fork rate) where heap corruption manifests as abort
 * instead of segfault.
 */
static void abrt_handler(int sig, siginfo_t *si, void *ucv)
{
	fprintf(stderr,
		"MALLOC-STRESS-DIAG SIGABRT (likely glibc heap-integrity check) pid=%d\n",
		getpid());
	/* Reuse the same state-dumper as segv_handler: same register
	 * layout, same registers of interest. */
	segv_handler(sig, si, ucv);
}

int main(void)
{
	struct sigaction sa = {0};
	enum { N = 100 };
	void *p[N];
	int i;
	static const size_t sizes[] = {
		16, 32, 48, 64, 80, 96, 128, 192, 256, 384,
		512, 768, 1024, 1536, 2048, 3072, 4096, 8192,
		16384, 65536, 131072,
	};
	const int nsizes = sizeof(sizes) / sizeof(sizes[0]);

	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);

	/* SIGABRT for glibc heap-integrity failures. */
	sa.sa_sigaction = abrt_handler;
	sigaction(SIGABRT, &sa, NULL);

	for (i = 0; i < N; i++) {
		size_t sz = sizes[i % nsizes];
		p[i] = malloc(sz);
		if (!p[i]) return 2;
		((char *)p[i])[0] = 0xa5;
		((char *)p[i])[sz - 1] = 0x5a;
	}
	for (i = N - 1; i >= 0; i--)
		free(p[i]);
	for (i = 0; i < N; i++) {
		size_t sz = sizes[(i * 7) % nsizes];
		p[i] = malloc(sz);
		if (!p[i]) return 2;
	}
	for (i = 0; i < N; i++)
		if ((i & 1) == 0) free(p[i]);
	for (i = 0; i < N; i++)
		if ((i & 1) == 1) free(p[i]);
	return 0;
}
