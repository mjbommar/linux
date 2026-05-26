#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALLOC_SZ 0x10000
static int nthreads = 2;
static volatile int barrier_count = 0;
static pthread_mutex_t bm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bc = PTHREAD_COND_INITIALIZER;

static void *worker(void *arg) {
    long tid = (long)arg;
    /* Sync start: wait for all threads to be ready */
    pthread_mutex_lock(&bm);
    barrier_count++;
    if (barrier_count < nthreads)
        while (barrier_count < nthreads)
            pthread_cond_wait(&bc, &bm);
    else
        pthread_cond_broadcast(&bc);
    pthread_mutex_unlock(&bm);

    void *p = mmap(NULL, ALLOC_SZ, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    memset(p, (int)tid, ALLOC_SZ);
    /* Verify */
    for (size_t j = 0; j < ALLOC_SZ; j += 4096) {
        if (((unsigned char *)p)[j] != (unsigned char)tid) {
            fprintf(stderr, "T%ld off%#zx p=%p got=%u expect=%u\n",
                    tid, j, p, ((unsigned char *)p)[j], (unsigned char)tid);
            return (void *)2;
        }
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
