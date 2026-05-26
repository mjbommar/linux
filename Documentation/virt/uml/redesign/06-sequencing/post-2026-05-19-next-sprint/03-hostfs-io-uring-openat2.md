# 03 — hostfs: `openat2(RESOLVE_BENEATH)` + io_uring writeback

**Sprint:** post-2026-05-19
**Priority:** HIGH
**Effort:** medium (~200–400 LoC across `fs/hostfs/{hostfs_user,hostfs_kern}.c`)
**Status:** Phase 1 DONE 2026-05-19 (`fa6af32c14ea`).  Phase 2
DONE 2026-05-19 (`3625a424df36`).  Phase 3 (FSYNC on the ring)
DONE 2026-05-19 (`4c1e7a1d86f8`).  FALLOCATE follow-on remains
optional (host filesystem rarely sees explicit fallocate from
hostfs callers).
**Depends on:** host kernel ≥ 5.6 for io_uring; ≥ 5.6 for `openat2`.
Can run in parallel with memos #1 and #2.

## Why this matters

Two wins in one workstream:

1. **Security:** today hostfs resolves every guest path via bare
   `open64(path, ...)`, which is subject to symlink-based escape
   from a hostfs mount root. Switching to
   `openat2(root_fd, rel, RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS)`
   closes a long-standing concern.
2. **Performance:** today `hostfs_writepages` issues one
   `pwrite64` per folio. Replacing that with batched io_uring
   submission cuts host-syscall count for any write-heavy guest
   workload by an order of magnitude.

## Current state

Sub-agent finding (2026-05-19) verified against source:

### Path resolution — symlink escape concern

```c
/* fs/hostfs/hostfs_user.c:95 */
fd = open64(path, mode);
```

`path` is the absolute host path the guest's view maps to.
There's no `RESOLVE_BENEATH` constraint; if a guest creates a
symlink in the hostfs that points to `/etc/passwd`, opening it
from the host's hostfs follows the symlink out of the mount
root. The `follow_link` code path in `hostfs_kern.c:139` then
returns host-rooted data to the guest.

Whether this is a "vulnerability" depends on threat model
(hostfs's documented behaviour is "host fs window for the
guest"), but it's at minimum a tight-isolation regression vs
what `openat2` enables.

### Writeback hot path — one `pwrite` per folio

```c
/* fs/hostfs/hostfs_kern.c:399 */
static int hostfs_writepages(struct address_space *mapping,
                              struct writeback_control *wbc)
{
    ...
    /* line 417 */
    ret = write_file(HOSTFS_I(inode)->fd, &pos, buffer, count);
    ...
}
```

`write_file` is the `os_pwrite_file` wrapper from
`hostfs_user.c:150`:

```c
n = pwrite64(fd, buf, len, *offset);
```

Synchronous, one folio at a time, called inside the writeback
loop. For a workload that dirties 10000 pages, that's 10000
sequential syscalls.

### Already modernised

- `statx` is already in use (`hostfs_user.c:51-62`) — good.
- File open uses `O_LARGEFILE` and `O_CREAT | O_RDWR` flags as
  appropriate.

## Proposed change

### Phase 1 — `openat2(RESOLVE_BENEATH)` for path resolution

```c
/* fs/hostfs/hostfs_user.c (new shape) */
static int hostfs_root_fd = -1;        /* opened once at mount */

int hostfs_init_root(const char *root_path)
{
    hostfs_root_fd = open(root_path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    return hostfs_root_fd < 0 ? -errno : 0;
}

int open_file(char *path, int r, int w, int append)
{
    struct open_how how = {
        .flags     = open_flags(r, w, append),
        .mode      = 0,
        .resolve   = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS,
    };
    /* path is now relative to the hostfs mount root. */
    return openat2(hostfs_root_fd, relative(path), &how, sizeof(how));
}
```

Behaviour change: hostfs becomes a closed-world filesystem with
no escape via symlinks or magic links (`/proc/self/fd/N`, etc.).

**Opt-out:** add a `hostfs_resolve_loose` mount option that
restores the bare `open64` path for users who depend on the
legacy escape behaviour. Default = strict (`RESOLVE_BENEATH`).

### Phase 2 — io_uring writeback in `hostfs_writepages`

Pre-condition: memo #2's `os_io_ring_*` substrate is in place.
hostfs reuses it.

```c
/* fs/hostfs/hostfs_kern.c::hostfs_writepages */
static int hostfs_writepages(struct address_space *mapping,
                              struct writeback_control *wbc)
{
    struct os_io_ring *ring = HOSTFS_SB(sb)->writeback_ring;
    struct folio_batch fbatch;
    folio_batch_init(&fbatch);

    while (filemap_get_folios_tag(mapping, ..., &fbatch)) {
        for_each_folio(folio, &fbatch) {
            void *ud = encode_ud(folio);
            os_io_ring_submit_pwrite(ring,
                                     HOSTFS_I(inode)->fd,
                                     folio_address(folio),
                                     folio_size(folio),
                                     folio_pos(folio),
                                     ud);
        }
    }
    /* Harvest completions, mark each folio done. */
    while (in_flight > 0) {
        struct os_io_cqe cqe;
        os_io_ring_wait_cqe(ring, &cqe, /*ms=*/100);
        hostfs_complete_writepage(decode_ud(cqe.user_data), cqe.res);
        in_flight--;
    }
}
```

Effective queue depth: the writeback budget. Typical workload
sees 32–128 folios per `writepages` call; that's 32–128×
parallelism vs today's serial loop.

### Phase 3 — `IORING_OP_FALLOCATE` / `FSYNC` integration

`hostfs_fsync` and any `fallocate` paths can also flow through
the ring, removing additional serialisation points.

## Effort breakdown

- Phase 1 (`openat2` resolution): ~120 LoC including the mount
  option plumbing + KUnit case validating symlink escape is
  blocked under strict mode.
- Phase 2 (writeback ring): ~200 LoC (depends on memo #2's
  `os_io_ring_*` substrate; net hostfs cost is ~150 LoC).
- Phase 3 (fsync / fallocate): ~50 LoC.

Total: ~370 LoC if memo #2 substrate is shared.

## Acceptance criteria

- **Security gate:** a guest that creates a symlink in hostfs
  pointing to `/etc/passwd` cannot read it (returns `-ELOOP` or
  `-EXDEV`) under the default mount options.
- **Backwards-compat gate:** `hostfs_resolve_loose` mount option
  restores legacy behaviour and is documented.
- **Performance gate:** `dd if=/dev/zero of=/hostfs/test bs=1M
  count=512` host syscall count drops by ≥ 10× vs baseline,
  measured by `perf stat -e syscalls:sys_enter_pwrite64`.
- **Functional gate:** existing hostfs selftests
  (`tools/testing/selftests/um/hostfs-*` if present, or
  `df-preserve`, the `userspace-smoke` family) PASS unchanged.

## Dependencies

- **Host kernel:** ≥ 5.6 for both `openat2` and io_uring.
  Fallback paths gated on `EINVAL`/`ENOSYS` probe at module
  init.
- **Memo #2:** wants the `os_io_ring_*` substrate. Can land
  independently of memo #2 if Phase 2 ships its own ring (less
  ideal — argues for ordering: do memo #2 first, then memo #3
  Phase 2 reuses the substrate).
- **Memo #1:** independent.

## Risk notes

- **`RESOLVE_BENEATH` semantics change.** Some hostfs users rely
  on the symlink-traversal behaviour. Mitigation: opt-out mount
  option preserves the legacy path.
- **Writeback ring backpressure.** If the host's I/O backlog grows
  faster than completions drain, the ring fills and
  `io_uring_submit` returns `EBUSY`. Mitigation: writeback
  caller falls back to synchronous `pwrite` for any folio that
  fails to enqueue.
- **`openat2` on older kernels:** ≥ 5.6 covers most production
  hosts but UML often runs on minimal CI hosts. Mitigation:
  probe + fall back.
- **`RESOLVE_NO_MAGICLINKS`** breaks `/proc/self/fd/N` style
  paths inside hostfs. This is intentional but may surface
  unexpected guest userspace failures (e.g., a build system
  that opens `/proc/self/fd/0` in the guest). Mitigation:
  the symlink resolves entirely inside the guest's view of the
  fs; only host-side resolution is affected. Verify the
  semantic boundary in selftests.

## Cross-references

- Sub-agent's full investigation: 2026-05-19 report (item #2).
- Memo 52 §3.3 (deferred `O_DIRECT` for hostfs): would also benefit
  from this work; deferred there because no async substrate
  existed.
- Memo 02 (UBD io_uring): shares the `os_io_ring_*` substrate.
- Memo 05 (common epoll completion thread): expected consumer of
  the hostfs writeback ring.
