/*
 * Diagnostic variant of mt-byteset.
 *
 * Goal: distinguish between two hypotheses for the 4% NULL-deref crash:
 *  A) mmap() actually returns NULL (test bug: only checks MAP_FAILED).
 *  B) -0x8(%rbp) stack slot in slow_memset gets clobbered to 0 between
 *     prologue store and loop body load (UML-side memory corruption).
 *
 * We print the mmap return value and a SIGSEGV handler dumps registers
 * + stack neighborhood so we can see what state is at fault time.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <stdint.h>

#define ITERS 50
#define ALLOC_SZ 0x10000

static __thread void *last_mmap_p;
static __thread int last_iter;
static __thread long last_tid;

static volatile int crash_dumped = 0;

static void sigsegv_handler(int sig, siginfo_t *si, void *ctx_) {
    if (__sync_lock_test_and_set(&crash_dumped, 1)) _exit(2);
    ucontext_t *uc = (ucontext_t *)ctx_;
    greg_t *g = uc->uc_mcontext.gregs;
    fprintf(stderr,
            "DIAG_CRASH: tid=%ld iter=%d last_mmap_p=%p si_addr=%p\n"
            "  RIP=%llx RSP=%llx RBP=%llx RAX=%llx RDX=%llx RDI=%llx\n",
            last_tid, last_iter, last_mmap_p, si->si_addr,
            (unsigned long long)g[REG_RIP],
            (unsigned long long)g[REG_RSP],
            (unsigned long long)g[REG_RBP],
            (unsigned long long)g[REG_RAX],
            (unsigned long long)g[REG_RDX],
            (unsigned long long)g[REG_RDI]);
    /* Read -0x8(%rbp), -0x18(%rbp) - the stack slots */
    uint64_t rbp = (uint64_t)g[REG_RBP];
    if (rbp) {
        unsigned char *sp = (unsigned char *)rbp;
        fprintf(stderr, "  stack at rbp:");
        for (int i = -0x30; i <= 0x10; i += 8) {
            fprintf(stderr, " [rbp%+d]=0x%016llx",
                    i, *(unsigned long long *)(sp + i));
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    _exit(3);
}

static void slow_memset(void *p, unsigned char val, size_t n) {
    volatile unsigned char *vp = (volatile unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        vp[i] = val;
    }
}

static void *worker(void *arg) {
    long tid = (long)arg;
    last_tid = tid;
    for (int i = 0; i < ITERS; i++) {
        last_iter = i;
        void *p = mmap(NULL, ALLOC_SZ, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        last_mmap_p = p;
        if (p == MAP_FAILED) {
            fprintf(stderr, "MMAP_FAILED tid=%ld iter=%d\n", tid, i);
            return (void *)1;
        }
        if (p == NULL) {
            fprintf(stderr, "MMAP_RETURNED_NULL tid=%ld iter=%d\n", tid, i);
            return (void *)4;
        }
        slow_memset(p, (unsigned char)tid, ALLOC_SZ);
        for (size_t j = 0; j < ALLOC_SZ; j += 4096) {
            if (((unsigned char *)p)[j] != (unsigned char)tid) {
                fprintf(stderr, "FAIL T%ld iter%d off%#zx got=%u expect=%u\n",
                        tid, i, j, ((unsigned char *)p)[j], (unsigned char)tid);
                munmap(p, ALLOC_SZ);
                return (void *)2;
            }
        }
        munmap(p, ALLOC_SZ);
    }
    return NULL;
}

int main(int argc, char **argv) {
    struct sigaction sa = { .sa_sigaction = sigsegv_handler,
                            .sa_flags = SA_SIGINFO | SA_RESETHAND };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);

    int n = (argc > 1) ? atoi(argv[1]) : 3;
    pthread_t t[16];
    for (long i = 0; i < n; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        void *r;
        pthread_join(t[i], &r);
        if (r) rc = 1;
    }
    if (rc == 0) printf("ALL_OK n=%d\n", n);
    return rc;
}
