#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ITERS 100
#define ALLOC_SZ 0x10000

static void *worker(void *arg) {
    long tid = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        void *p = mmap(NULL, ALLOC_SZ, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE, -1, 0);
        if (p == MAP_FAILED) return (void *)1;
        memset(p, (int)tid, ALLOC_SZ);
        for (size_t j = 0; j < ALLOC_SZ; j += 4096) {
            if (((unsigned char *)p)[j] != (unsigned char)tid) {
                fprintf(stderr, "T%ld iter%d off%#zx got=%u\n", tid, i, j, ((unsigned char *)p)[j]);
                munmap(p, ALLOC_SZ);
                return (void *)2;
            }
        }
        munmap(p, ALLOC_SZ);
    }
    return NULL;
}

int main(void) {
    pthread_t t[3];
    for (long i = 0; i < 3; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    int rc = 0;
    for (int i = 0; i < 3; i++) {
        void *r;
        pthread_join(t[i], &r);
        if (r) rc = 1;
    }
    if (rc == 0) printf("ALL_OK\n");
    return rc;
}
