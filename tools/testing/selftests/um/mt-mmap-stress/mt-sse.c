#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <emmintrin.h>

#define ITERS 50
#define ALLOC_SZ 0x10000

/* Pure SSE2 memset - uses XMM. */
static void __attribute__((target("sse2")))
sse_memset(void *p, unsigned char val, size_t n) {
    __m128i v = _mm_set1_epi8((char)val);
    char *cp = (char *)p;
    while (n >= 16) {
        _mm_storeu_si128((__m128i *)cp, v);
        cp += 16;
        n -= 16;
    }
    while (n--) *cp++ = val;
}

static void *worker(void *arg) {
    long tid = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        void *p = mmap(NULL, ALLOC_SZ, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return (void *)1;
        sse_memset(p, (unsigned char)tid, ALLOC_SZ);
        for (size_t j = 0; j < ALLOC_SZ; j += 4096) {
            if (((unsigned char *)p)[j] != (unsigned char)tid) {
                fprintf(stderr, "FAIL T%ld iter%d off%#zx got=%u\n",
                        tid, i, j, ((unsigned char *)p)[j]);
                munmap(p, ALLOC_SZ);
                return (void *)2;
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
