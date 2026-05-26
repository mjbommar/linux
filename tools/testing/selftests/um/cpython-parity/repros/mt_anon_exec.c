/* Multi-thread anon mmap with PROT_EXEC + write+read. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <errno.h>
#define NTHREADS    5
#define ITERATIONS  20000
#define PAGE_SIZE   4096
static atomic_int errors = 0, done = 0;
struct ctx { int tid; };

static void *worker(void *arg) {
    struct ctx *c = arg;
    unsigned char want = 0x90 + c->tid;
    for (int i = 0; i < ITERATIONS; i++) {
        /* PROT_EXEC included */
        void *p = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "FAIL tid=%d iter=%d mmap errno=%d\n", c->tid, i, errno);
            atomic_fetch_add(&errors, 1); break;
        }
        memset(p, want, PAGE_SIZE);
        if (((unsigned char *)p)[PAGE_SIZE/2] != want) {
            fprintf(stderr, "FAIL tid=%d iter=%d corrupt\n", c->tid, i);
            atomic_fetch_add(&errors, 1); munmap(p, PAGE_SIZE); break;
        }
        munmap(p, PAGE_SIZE);
    }
    atomic_fetch_add(&done, 1);
    return NULL;
}

int main(void) {
    pthread_t t[NTHREADS]; struct ctx c[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) { c[i].tid = i; pthread_create(&t[i], NULL, worker, &c[i]); }
    for (int i = 0; i < NTHREADS; i++) pthread_join(t[i], NULL);
    int e = atomic_load(&errors);
    printf("REPRO: %s done=%d errors=%d\n", e ? "ERRORS" : "OK", atomic_load(&done), e);
    return e ? 1 : 0;
}
