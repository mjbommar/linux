/*
 * Minimal MT mmap reproducer - no memset, no munmap.
 * Just N threads doing mmap() in a tight loop, looking for:
 *  - duplicate addresses returned to different threads
 *  - addresses in unexpected ranges (e.g., < 0x40000000)
 *  - actual NULL returns
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ITERS 200
#define ALLOC_SZ 0x10000
#define MAX_THREADS 16
#define MAX_RECORDS (MAX_THREADS * ITERS)

struct record {
    long tid;
    int iter;
    void *p;
};
static struct record records[MAX_RECORDS];
static volatile int rec_idx = 0;
static pthread_mutex_t rec_mtx = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
    long tid = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        void *p = mmap(NULL, ALLOC_SZ, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) p = (void *)-1;
        pthread_mutex_lock(&rec_mtx);
        if (rec_idx < MAX_RECORDS) {
            records[rec_idx].tid = tid;
            records[rec_idx].iter = i;
            records[rec_idx].p = p;
            rec_idx++;
        }
        pthread_mutex_unlock(&rec_mtx);
        munmap(p, ALLOC_SZ);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 4;
    pthread_t t[MAX_THREADS];
    for (long i = 0; i < n; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);

    int low_addrs = 0, dups = 0, nulls = 0, fails = 0;
    for (int i = 0; i < rec_idx; i++) {
        if (records[i].p == (void *)-1) { fails++; continue; }
        if (records[i].p == NULL) { nulls++; continue; }
        if ((unsigned long)records[i].p < 0x40000000) low_addrs++;
        for (int j = i + 1; j < rec_idx; j++) {
            if (records[j].p == records[i].p) dups++;
        }
    }
    printf("MMAP_RACE n=%d total=%d fails=%d nulls=%d low_addrs=%d dups=%d\n",
           n, rec_idx, fails, nulls, low_addrs, dups);
    if (low_addrs == 0 && dups == 0 && nulls == 0 && fails == 0) {
        printf("MMAP_RACE: PASS\n");
        return 0;
    }
    /* Print first 5 anomalies */
    int printed = 0;
    for (int i = 0; i < rec_idx && printed < 5; i++) {
        if ((unsigned long)records[i].p < 0x40000000 && records[i].p != (void*)-1) {
            printf("  ANOMALY tid=%ld iter=%d p=%p\n",
                   records[i].tid, records[i].iter, records[i].p);
            printed++;
        }
    }
    return 1;
}
