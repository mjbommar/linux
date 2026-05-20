# 02 — UBD: synchronous helper thread → io_uring

**Sprint:** post-2026-05-19
**Priority:** HIGH
**Effort:** medium-large (~400–700 LoC across `arch/um/drivers/ubd_*.c`
and `arch/um/os-Linux/file.c`)
**Status:** ALL PHASES DONE 2026-05-19.
  * Phase 1 substrate (`44a1ca55a72a`)
  * Phase 2a within-req parallel (`ef60f68cd398`)
  * Multi-bvec offset fix (`708b7c3f3253`)
  * Phase 3 vectored submission (`f25fcd47be37`)
  * Phase 2b cross-request parallel (`3e3cf923676d`)
  * Phase 4 O_DIRECT (`1ce2b2764cd1`) + lazy submit + fast-path
  * `um_ubd_no_uring=` A/B knob + bench (`b305e1ae5516`)
  * Phase 5 COW bitmap drain (this commit)
**Depends on:** host kernel ≥ 5.6 for io_uring; can run in parallel
with memos #1 and #3.

## Why this matters

UBD is UML's block driver — every guest with a virtual disk uses
it. It's also the loudest unmodernised hot-path in the tree after
the recent network/snapshot/host-resource work. Today every guest
block I/O serialises through a single host helper thread that
issues one synchronous `pread`/`pwrite` per scatter-gather entry.

Sub-agent finding (2026-05-19) verified against source:

```c
/* arch/um/drivers/ubd_kern.c */
#define UBD_MAX_REQUEST (8 * sizeof(long))   /* 48 on x86_64 */
#define MAX_SG 64

/* arch/um/drivers/ubd_kern.c:1407 */
static void do_io(struct io_thread_req *req, struct io_desc *desc)
{
    ...
    /* line 1443 read */
    n = os_pread_file(req->fds[bit], buf, len, off);
    ...
    /* line 1452 write */
    n = os_pwrite_file(req->fds[bit], buf, len, off);
    ...
}
```

```c
/* arch/um/os-Linux/file.c:271 / :298 */
int os_pread_file(int fd, void *buf, int len, unsigned long long offset)
{
    int n = pread64(fd, buf, len, offset);   /* synchronous host syscall */
    ...
}
```

The `do_io` loop runs in one helper thread that pulls request
pointers over a pipe, then iterates each request's segment array
issuing one host syscall per segment. With `MAX_SG = 64` a single
guest request can issue 64 sequential `pread64` syscalls.

This means:

- **Effective queue depth: 1.** No matter how many requests the
  guest queues, only one host I/O is ever in flight.
- **No AIO, no `O_DIRECT`, no batched submission.** Modern
  storage stacks expect deep queues.
- **Postgres / Django / kbuild workloads are I/O-bound under
  UML** because each `read()` or `write()` from the guest
  serialises behind a chain of host syscalls.
- **Memo 52 §3.3 (`O_DIRECT` for hostfs I/O) was deferred** for
  this exact reason — without an async substrate, `O_DIRECT`
  on top of a synchronous helper thread does not help.

## Current state

| Component | Path | State |
|-----------|------|-------|
| Block-mq dispatch | `arch/um/drivers/ubd_kern.c::ubd_queue_one_request` | active, sends one request per `write()` to the helper pipe |
| Helper thread | `arch/um/drivers/ubd_user.c` | single-threaded `poll(pipe)` → `do_io()` loop |
| Sync I/O | `arch/um/os-Linux/file.c::os_pread_file` / `os_pwrite_file` | bare `pread64` / `pwrite64` |
| COW bitmap update | `arch/um/drivers/ubd_kern.c:1399` | serialised `os_pwrite_file` for the bitmap word |
| Helper-to-kernel completion | pipe write of `io_thread_req *` | one cacheline at a time |

Per memo 52's `[host_resources]` plumbing the helper thread
inherits the process-level affinity but **does not honour the
io_uring substrate**.

## Proposed change

A staged port, each phase shippable:

### Phase 1 — `os_*` async substrate

Add `arch/um/os-Linux/io_uring.c` with:

```c
struct os_io_ring;
struct os_io_ring *os_io_ring_create(unsigned int entries);
void os_io_ring_destroy(struct os_io_ring *ring);
int os_io_ring_submit_pread(struct os_io_ring *ring, int fd,
                            void *buf, size_t len, off_t off,
                            void *user_data);
int os_io_ring_submit_pwrite(struct os_io_ring *ring, int fd,
                             const void *buf, size_t len, off_t off,
                             void *user_data);
int os_io_ring_submit_writev(struct os_io_ring *ring, int fd,
                             const struct iovec *iov, int iovcnt,
                             off_t off, void *user_data);
int os_io_ring_wait_cqe(struct os_io_ring *ring,
                        struct os_io_cqe *out_cqe,
                        int timeout_ms);
int os_io_ring_peek_cqe(struct os_io_ring *ring,
                        struct os_io_cqe *out_cqe);
```

Thin wrapper around the raw `io_uring_setup` / `io_uring_enter`
syscalls (skip `liburing` to avoid pulling in a library
dependency; UML's `os-Linux/` is hand-rolled syscall code by
convention). Behaviour: `op->res` flows back through the `cqe`
verbatim; the caller maps `user_data` back to its in-flight
request.

Fallback: if `io_uring_setup` returns `ENOSYS` / `EPERM`, the
caller can use a parallel `aio_*` shim (Phase 1.5 if needed) or
fall through to the existing helper-thread path.

### Phase 2 — Convert UBD `do_io` to ring submission

```c
/* New shape in ubd_user.c */
struct ubd_io_thread_state {
    struct os_io_ring *ring;
    struct io_thread_req *inflight[UBD_RING_DEPTH];
    int free_slots;
    ...
};

static void ubd_dispatch_request(struct ubd_io_thread_state *st,
                                  struct io_thread_req *req)
{
    for (int i = 0; i < req->desc_cnt; i++) {
        struct io_desc *d = &req->io_desc[i];
        void *ud = encode_ud(req, i);
        if (d->op == REQ_OP_READ)
            os_io_ring_submit_pread(st->ring, req->fds[i],
                                    d->buffer, d->length, d->offset, ud);
        else
            os_io_ring_submit_pwrite(st->ring, req->fds[i],
                                     d->buffer, d->length, d->offset, ud);
    }
}

static void ubd_completion_loop(struct ubd_io_thread_state *st)
{
    while (running) {
        struct os_io_cqe cqe;
        if (os_io_ring_wait_cqe(st->ring, &cqe, /*timeout_ms=*/100) > 0) {
            struct io_thread_req *req = decode_req(cqe.user_data);
            int idx = decode_idx(cqe.user_data);
            ubd_handle_segment_completion(req, idx, cqe.res);
        }
    }
}
```

Deep queue depth (target `UBD_RING_DEPTH = 256`) is the win.

### Phase 3 — Vectored submission

Where `io_desc[]` segments are contiguous in the guest, coalesce
them into a single `IORING_OP_WRITEV` (or `READV`) submission.
Cuts SQE count for sequential workloads by `MAX_SG` × at the limit.

### Phase 4 — Optional `O_DIRECT` path

With a real async substrate, `O_DIRECT` becomes useful: skip the
host pagecache, reduce double-buffering. Gate on a per-UBD
cmdline knob (and verify alignment from the guest side — UBD's
existing segment alignment is page-aligned, which satisfies
`O_DIRECT` requirements on common filesystems).

### Phase 5 — COW bitmap update path

`ubd_kern.c:1399` currently issues a synchronous `os_pwrite_file`
for each bitmap word update. Route this through the same ring
with `IOSQE_IO_DRAIN` to maintain the ordering invariant
(bitmap update commits before the data write is acknowledged).

## Effort breakdown

- Phase 1 (`os_io_ring` substrate): ~250 LoC + ~50 LoC of tests.
- Phase 2 (`do_io` → ring): ~200 LoC of replacement + ~50 LoC
  helper-thread tear-down.
- Phase 3 (vectored): ~80 LoC.
- Phase 4 (`O_DIRECT`): ~40 LoC + cmdline plumbing.
- Phase 5 (COW bitmap drain): ~30 LoC.

Total: ~700 LoC. Spread across two patches (Phase 1+2 is the
shippable minimum; 3/4/5 are follow-on improvements).

## Acceptance criteria

- **Functional gate:** existing UBD smoke tests
  (`tools/testing/selftests/um/ubd-*`) PASS with the new path.
- **Behavioural gate:** at queue depth 32 the new path
  outperforms the old by ≥ 5× on a `fio --rw=randread
  --iodepth=32 --bs=4k` workload.
- **Cold-start gate:** boot time with a 1 GiB UBD image is no
  worse than baseline (catches the case where async submission
  has overhead at queue depth 1).
- **Memo 52 integration:** the `umlctl mission` Phase 5 soak
  picks up a UBD-stressing workload (e.g., a kbuild template
  that builds against a UBD-mounted fs).

## Dependencies

- **Host kernel:** ≥ 5.6 for the core `IORING_OP_READ`/`WRITE`/
  `READV`/`WRITEV` opcodes. Older hosts fall back to the
  existing helper-thread path automatically.
- **seccomp profile:** `sandbox` and `fuzz` profiles need
  `io_uring_setup` and `io_uring_enter` allowed; verify against
  the existing audit-vector-sandbox harness.
- **No memo dependency:** can start immediately; doesn't block
  on the vector2 flip (#1) or any other memo in this sprint.

## Risk notes

- **io_uring CVE history.** Several CVE-2022 and 2023 entries
  hit io_uring sqpoll. Mitigation: do NOT use sqpoll mode for
  UBD; submit synchronously via `io_uring_enter`. The bug
  history is concentrated in optional features, not the core
  ring API we'd use.
- **CQE-handling backpressure.** If guest requests outpace host
  completion, the ring fills and `io_uring_enter` returns
  `EAGAIN`. Mitigation: block-mq dispatch already has back-
  pressure semantics; map ring-full to NEEDS_RETRY.
- **seccomp-only profile.** May need to allow `io_uring_*` syscalls
  in the production seccomp filter. Mitigation: keep the old
  helper-thread path as the fallback for profiles that exclude
  io_uring (the `sandbox` profile already has tight syscall
  filters; this is an opt-in).
- **Rollback:** the `os_io_ring_create` failure path keeps the
  driver functional by demoting to the synchronous helper-
  thread path. Worst case = pre-sprint behaviour.

## Cross-references

- Sub-agent's full investigation: 2026-05-19 report (item #1).
- Memo 52 §3.3 (deferred `O_DIRECT` for hostfs): the same
  async-substrate argument applies. hostfs gets its own
  port in memo 03 of this sprint.
- Memo 05 of this sprint plans a common epoll completion thread
  that owns UBD + hostfs + vector2 rings once all three land.
