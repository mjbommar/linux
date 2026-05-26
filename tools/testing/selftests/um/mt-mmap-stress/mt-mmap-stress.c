#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ITERS 50
#define ALLOC_SZ 0x10000
static int nthreads = 3;

static void *worker(void *arg) {
    long tid = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        void *p = mmap(NULL, ALLOC_SZ, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return (void *)1;
        memset(p, (int)tid, ALLOC_SZ);
        for (size_t j = 0; j < ALLOC_SZ; j += 4096) {
            if (((unsigned char *)p)[j] != (unsigned char)tid) {
                fprintf(stderr, "T%ld iter%d off%#zx p=%p got=%u expect=%u\n",
                        tid, i, j, p, ((unsigned char *)p)[j], (unsigned char)tid);
                /* dump 16 bytes */
                fprintf(stderr, "T%ld bytes: ", tid);
                for (int k = 0; k < 16; k++)
                    fprintf(stderr, "%02x ", ((unsigned char *)p)[j+k]);
                fprintf(stderr, "\n");
                munmap(p, ALLOC_SZ);
                return (void *)2;
            }
        }
        munmap(p, ALLOC_SZ);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) nthreads = atoi(argv[1]);
    pthread_t threads[16];
    for (long i = 0; i < nthreads; i++)
        pthread_create(&threads[i], NULL, worker, (void *)i);
    int rc = 0;
    for (int i = 0; i < nthreads; i++) {
        void *retval;
        pthread_join(threads[i], &retval);
        if (retval) rc = 1;
    }
    if (rc == 0) printf("ALL_OK n=%d\n", nthreads);
    return rc;
}
