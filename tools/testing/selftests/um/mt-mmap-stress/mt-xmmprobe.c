#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ITERS 50
#define ALLOC_SZ 0x10000

/* Load XMM with known pattern, then triggered fault by writing across page boundaries.
 * After fault, verify XMM still has the pattern. */
static void *worker(void *arg) {
    long tid = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        void *p = mmap(NULL, ALLOC_SZ, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return (void *)1;

        /* Use embedded asm: load XMM with pattern, do faulting writes,
         * read XMM back into memory, check.
         */
        unsigned char before[16] __attribute__((aligned(16)));
        unsigned char after[16] __attribute__((aligned(16)));
        unsigned char val = (unsigned char)tid;

        for (int b = 0; b < 16; b++) before[b] = val;

        /* Load xmm0 with [val,val,val,...]. Then write to all 16 pages
         * using xmm0. Then read xmm0 to memory. */
        __asm__ __volatile__(
            "movdqa  %[before], %%xmm0       \n"  /* load pattern */
            "mov     %[ptr], %%rdi           \n"
            "mov     $16, %%rcx              \n"
            "1:                              \n"
            "movdqu  %%xmm0, (%%rdi)         \n"  /* faulting write */
            "add     $4096, %%rdi            \n"
            "loop    1b                      \n"
            "movdqa  %%xmm0, %[after]        \n"  /* readback xmm */
            : [after] "=m"(after)
            : [before] "m"(before), [ptr] "r"(p)
            : "rdi", "rcx", "xmm0", "memory"
        );

        /* Compare before vs after */
        if (memcmp(before, after, 16) != 0) {
            fprintf(stderr, "FAIL T%ld iter%d XMM_DRIFT before=", tid, i);
            for (int b = 0; b < 16; b++) fprintf(stderr, "%02x", before[b]);
            fprintf(stderr, " after=");
            for (int b = 0; b < 16; b++) fprintf(stderr, "%02x", after[b]);
            fprintf(stderr, "\n");
            munmap(p, ALLOC_SZ);
            return (void *)2;
        }

        /* Verify writes landed: each page byte 0 should be val */
        for (size_t j = 0; j < ALLOC_SZ; j += 4096) {
            if (((unsigned char *)p)[j] != val) {
                fprintf(stderr, "FAIL T%ld iter%d MEM off%#zx got=%u\n",
                        tid, i, j, ((unsigned char *)p)[j]);
                munmap(p, ALLOC_SZ);
                return (void *)3;
            }
        }
        munmap(p, ALLOC_SZ);
    }
    return NULL;
}

int main(int argc, char **argv) {
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
