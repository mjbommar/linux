/* Multi-thread file-backed mmap (PROT_READ only — no exec) of /etc/hostname. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <errno.h>
#define NTHREADS    5
#define ITERATIONS  5000
static atomic_int errors = 0, done = 0;
struct ctx { int tid; const char *path; };

static void *worker(void *arg) {
    struct ctx *c = arg;
    for (int i = 0; i < ITERATIONS; i++) {
        int fd = open(c->path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "FAIL tid=%d iter=%d open errno=%d\n", c->tid, i, errno);
            atomic_fetch_add(&errors, 1); break;
        }
        struct stat st;
        if (fstat(fd, &st) < 0) { close(fd); atomic_fetch_add(&errors, 1); break; }
        size_t map_sz = (st.st_size + 4095) & ~4095UL;
        if (map_sz == 0) map_sz = 4096;
        void *p = mmap(NULL, map_sz, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) {
            fprintf(stderr, "FAIL tid=%d iter=%d mmap errno=%d\n", c->tid, i, errno);
            atomic_fetch_add(&errors, 1); break;
        }
        /* Touch first byte to force fault-in. */
        volatile unsigned char b = ((volatile unsigned char *)p)[0];
        (void)b;
        munmap(p, map_sz);
    }
    atomic_fetch_add(&done, 1);
    return NULL;
}

int main(int argc, char **argv) {
    pthread_t t[NTHREADS]; struct ctx c[NTHREADS];
    const char *path = argc > 1 ? argv[1] : "/etc/hostname";
    for (int i = 0; i < NTHREADS; i++) { c[i].tid = i; c[i].path = path; pthread_create(&t[i], NULL, worker, &c[i]); }
    for (int i = 0; i < NTHREADS; i++) pthread_join(t[i], NULL);
    int e = atomic_load(&errors);
    printf("REPRO: %s done=%d errors=%d\n", e ? "ERRORS" : "OK", atomic_load(&done), e);
    return e ? 1 : 0;
}
