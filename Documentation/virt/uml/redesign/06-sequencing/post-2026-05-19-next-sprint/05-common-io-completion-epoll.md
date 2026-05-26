# 05 — Common epoll I/O completion thread

**Sprint:** post-2026-05-19
**Priority:** MEDIUM
**Effort:** medium (~300–500 LoC across
`arch/um/os-Linux/{io_uring,irq,process}.c` and the per-subsystem
callers in `arch/um/drivers/`)
**Status:** planned (depends on memos #2 and #3 landing)
**Depends on:** memo 02 (UBD io_uring) AND memo 03 (hostfs
io_uring) provide the per-fd rings this memo consolidates.

## Why this matters

Without consolidation, every io_uring user spawns a dedicated
helper thread (or thread pool) for completion harvesting. With
the per-system memos in this sprint landing:

  - memo #2 (UBD): one ring, one thread.
  - memo #3 (hostfs): one ring per superblock (one mount), one
    thread.
  - vector2 (existing): one RX thread per queue.

If we don't consolidate, a typical guest with UBD + a hostfs
mount + a vector2 NIC ends up with **three or more new helper
threads** in addition to the UML kernel threads. Each costs
~8 KiB of host stack at minimum, plus cross-thread cache traffic
on every completion delivery.

UML already has a single `epollfd` for in-kernel IRQs at
`arch/um/os-Linux/irq.c`. Folding the io_uring CQ fds into the
same epoll loop:

1. Eliminates the per-subsystem helper-thread tax.
2. Gets us one consistent host-side I/O completion model
   that's easy for the seccomp filter to allow.
3. Reduces cross-thread bouncing on completions —
   completions arrive on the same thread that already runs
   the kernel's IRQ delivery.

## Current state

| Path | Host-side thread | File |
|------|-----------------|------|
| In-kernel IRQs | one `epoll_wait` loop | `arch/um/os-Linux/irq.c` |
| UBD pre-memo-#2 | one helper thread, `poll(pipe)` | `arch/um/drivers/ubd_user.c` |
| UBD post-memo-#2 | one helper thread, `io_uring_enter` | `arch/um/drivers/ubd_user.c` (planned) |
| hostfs pre-memo-#3 | none — synchronous in caller | `fs/hostfs/hostfs_user.c` |
| hostfs post-memo-#3 | one ring, sync writeback caller | `fs/hostfs/hostfs_kern.c` (planned) |
| vector2 RX | one thread per queue | `arch/um/drivers/vector2_host_*.c` |

Note the **vector2 RX thread layout doesn't need to change for
this memo**; vector2 doesn't use io_uring today (sendmmsg + epoll
on the socket fd). It already integrates with the global epoll
loop. Memo 05 specifically targets the new io_uring consumers.

## Proposed change

### Phase 1 — Promote `os_io_ring` CQ to `epollfd` registration

Each io_uring instance gets registered with `IORING_REGISTER_
EVENTFD` so its CQ completion drops a wake on a host eventfd.
That eventfd is added to the global epoll set.

```c
/* arch/um/os-Linux/io_uring.c (extends memo #2's substrate) */
int os_io_ring_register_eventfd(struct os_io_ring *ring, int *out_eventfd);
int os_io_ring_unregister_eventfd(struct os_io_ring *ring);
```

### Phase 2 — Single global completion thread

`arch/um/os-Linux/irq.c::os_idle_sleep` (or equivalent —
the function that owns the `epoll_wait` loop) gets a new
handler entry for each io_uring's eventfd:

```c
case OS_EPOLL_FD_KIND_IO_URING:
    while (os_io_ring_peek_cqe(fd->ring, &cqe) > 0) {
        fd->callback(fd->ctx, &cqe);
    }
    break;
```

Where `fd->callback` is the per-subsystem completion handler
(UBD: complete the request; hostfs: mark the folio dirty/done;
etc.).

### Phase 3 — Retire the per-subsystem helper threads

UBD's `ubd_user.c` and hostfs's writeback caller no longer spawn
their own threads. The ring is shared with the global epoll
thread; the per-subsystem code only submits SQEs.

### Phase 4 — seccomp filter audit

The `sandbox` and `fuzz` profiles have tight syscall filters
that may not allow `io_uring_*` syscalls. With the global
completion thread consolidation, the filter change is one
location (the global thread's seccomp scope) rather than
per-subsystem. Audit the filter and add the required
`io_uring_setup` / `io_uring_enter` / `io_uring_register`
syscalls.

## Effort breakdown

- Phase 1 (eventfd registration): ~50 LoC in
  `os-Linux/io_uring.c`.
- Phase 2 (global completion dispatch): ~150 LoC in
  `os-Linux/irq.c` + a callback table.
- Phase 3 (per-subsystem retirement): ~100 LoC of deletions
  across UBD + hostfs (negative diff).
- Phase 4 (seccomp filter audit): ~50 LoC + selftest update.

Net: ~350 LoC, with a meaningful negative diff in the
per-subsystem helper-thread code.

## Acceptance criteria

- **Functional gate:** all existing UBD + hostfs + vector2
  selftests PASS unchanged.
- **Thread-count gate:** `cat /proc/$pid/status | grep Threads:`
  on a booted UML with UBD + hostfs mount + vector2 NIC shows
  N − 2 threads vs pre-memo (UBD helper retired, hostfs helper
  retired).
- **Latency gate:** UBD `fio --rw=randread --iodepth=32` median
  latency unchanged vs memo #2 baseline (no regression from
  consolidation).
- **seccomp gate:** `sandbox` profile boots and runs `df` /
  `ls` without `io_uring_*` syscall denials.

## Dependencies

- **Memo #2 (UBD io_uring):** must land first. The substrate
  this memo consolidates doesn't exist without #2.
- **Memo #3 (hostfs io_uring):** must land first for the same
  reason.
- **Existing global epollfd loop:** assumed working.

## Risk notes

- **Completion-thread bottleneck.** One thread harvesting all
  I/O completions can saturate on a guest issuing both UBD and
  hostfs traffic at scale. Mitigation: leave the per-subsystem
  ring distinct (one ring per device), so dispatch is
  per-ring; the global thread runs `peek_cqe` on each ring
  round-robin. If saturation appears, partition by NUMA node.
- **Latency-vs-throughput tradeoff.** Per-subsystem threads
  could conceivably get scheduled in parallel on a busy host;
  one consolidated thread serialises. In practice the
  io_uring CQE harvest is microseconds; serialisation is fine
  up to ~10⁶ completions/sec. Mitigation: revisit if the
  bench data shows a real regression.
- **seccomp surface expansion.** Adding `io_uring_*` to the
  `sandbox` profile is an explicit security/perf tradeoff.
  Mitigation: gate behind a per-profile config bit; default
  to "allow" for `prod-fast` / `research`, "deny" for
  `sandbox`. Sandbox falls back to the synchronous helper-
  thread path for UBD / hostfs.
- **Rollback:** revert is per-subsystem. Each driver retains
  the old code path under `CONFIG_UML_OS_IO_URING_CONSOLIDATED=n`
  until the new path is proven.

## Cross-references

- Sub-agent's investigation: 2026-05-19 report (item #5).
- Memo #2 (UBD io_uring) and Memo #3 (hostfs io_uring): produce
  the rings this memo consolidates.
- `arch/um/os-Linux/irq.c`: the global epoll loop that grows
  the new completion-source kind.
