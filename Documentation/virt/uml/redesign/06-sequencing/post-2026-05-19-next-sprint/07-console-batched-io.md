# 07 — Console batched I/O (`writev` / `vmsplice`)

**Sprint:** post-2026-05-19
**Priority:** LOW-MEDIUM
**Effort:** small (~100 LoC across `arch/um/drivers/chan_user.c`
and `arch/um/os-Linux/file.c`)
**Status:** investigated 2026-05-19; Phase 1 (ring-wrap coalescing
via writev) DONE 2026-05-19 (`25a339e9f460`).  Phase 2 (vmsplice
for pipe consoles) deferred — page-ownership accounting absent
from line.c's static ring buffer and the per-call win is small
vs Phase 1.
**Depends on:** none.

## Update 2026-05-19 — investigation note

The original sub-agent finding called the console path a
"byte-buffered `write()` loop" stalling on `dmesg | head -10000`.
Code reading shows that's not the current shape:

* `arch/um/drivers/line.c::flush_buffer` writes the LINE_BUFSIZE
  ring buffer in 1–2 `write_chan` calls (only 2 when the ring
  wraps).
* `arch/um/drivers/chan_user.c::generic_write` calls host
  `write()` once for the full buffer and only loops on short
  writes (`written += err; n - written`).

So the dramatic win the agent predicted does not apply to the
typical case. The remaining `writev` opportunity is:

* Coalescing the ring-wrap case (2 syscalls → 1).
* Multi-console fan-out (con0+con1+...) — printk currently
  walks each console with a separate `write_chan`; one `writev`
  per console group would help if N consoles > 1.

That's a real but small win (~5–10% syscall reduction on heavy
console traffic, no impact on interactive). **Recommendation:
deprioritise to "filler item, opportunistic"**; the larger
critical-path items (memo #2 UBD io_uring, memo #3 hostfs
io_uring) remain the right next investment.

The original design below is kept for reference if a future
maintainer wants to pick it up.

## Why this matters

UML's console driver
(`arch/um/drivers/chan_user.c::generic_write`) is a byte-buffered
`write()` loop. Fine for interactive use. But:

- `dmesg | head -10000` stalls on N tiny writes.
- Heavy boot logging issues hundreds of small syscalls per
  second.
- `umlctl logs <instance>` and the soak-daemon's per-iteration
  log capture amplify the cost — every line in the guest's
  `init.log` is one host `write()`.

It's a cheap win: a tens-of-lines patch, zero architectural
implications, no protocol changes guest-visible.

## Current state

```c
/* arch/um/drivers/chan_user.c (paraphrased) */
int generic_write(int fd, const char *buf, int n, void *unused)
{
    int err = write(fd, buf, n);
    if (err > 0)
        return err;
    if (errno == EAGAIN)
        return 0;
    return -errno;
}
```

Each call into the console layer is one host `write()`. The
guest kernel's printk batching doesn't propagate to the host
syscall layer.

## Proposed change

### Phase 1 — Buffer-aware `writev`

```c
/* arch/um/os-Linux/file.c (new) */
int os_console_write_iov(int fd, const struct iovec *iov, int iovcnt)
{
    int err;
    do {
        err = writev(fd, iov, iovcnt);
    } while (err < 0 && errno == EINTR);
    if (err < 0)
        return -errno;
    return err;
}
```

Caller in `chan_user.c` accumulates pending writes into an
iov array (capped at IOV_MAX) and submits in one syscall.

### Phase 2 — Optional `vmsplice` for pipe consoles

For consoles backed by pipes (UML's default `con0=fd:0,fd:1`
case), use `vmsplice(SPLICE_F_GIFT)` to hand pages directly to
the host pipe without a copy. Gated on a per-console check that
the fd is a pipe.

```c
ssize_t os_console_write_vmsplice(int fd, const struct iovec *iov,
                                  int iovcnt)
{
    return vmsplice(fd, iov, iovcnt, SPLICE_F_GIFT);
}
```

### Phase 3 — Caller integration

Modify the path in `chan_user.c` that today calls
`generic_write` once per character chunk to instead accumulate
into an iov and flush at:

- iov full (IOV_MAX reached),
- end-of-write (`\n` seen or buffer empty),
- explicit flush.

## Effort breakdown

- Phase 1 (`writev` wrapper + caller change): ~50 LoC.
- Phase 2 (`vmsplice` for pipes, optional): ~30 LoC.
- Phase 3 (caller integration): ~20 LoC + test update.

Total: ~100 LoC.

## Acceptance criteria

- **Functional gate:** `umlctl logs <instance>` output is
  byte-identical to baseline.
- **Performance gate:** `dmesg` of a 100-line boot log issues
  ≤ 10 host `write` syscalls (vs ≥ 100 today).
- **Functional gate:** `umlctl mission` Phase 1 KUnit passes
  unchanged (console doesn't break boot).
- **Interactive gate:** `umlctl up --foreground` with a shell
  init exhibits no visible lag (the change must not introduce
  buffering delay).

## Dependencies

- **Code:** none.
- **Host kernel:** `writev` is universal; `vmsplice` is
  Linux-specific but available everywhere UML runs.
- **Memo:** none.

## Risk notes

- **Buffering delay.** If the iov accumulator holds output too
  long, interactive sessions see lag. Mitigation: always flush
  on `\n` (terminal half-line behaviour) and on a 1-ms
  timer-driven flush as a safety net.
- **`vmsplice` ownership semantics.** `SPLICE_F_GIFT` transfers
  page ownership; the caller must not touch the pages after
  submission. Mitigation: only use on freshly-allocated buffers
  not on guest-visible memory.
- **Rollback:** revert is a one-line `#ifdef` flip back to the
  per-character `write()` loop.

## Cross-references

- Sub-agent's investigation: 2026-05-19 report (item #6).
- `arch/um/drivers/chan_user.c`: the file that grows the
  `writev` path.
