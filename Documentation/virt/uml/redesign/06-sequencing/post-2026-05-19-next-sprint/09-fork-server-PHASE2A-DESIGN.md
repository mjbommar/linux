# 09 — Fork-server Phase 2a — SKAS stub re-binding design

**Sprint:** post-2026-05-19 fork-server sprint
**Status:** designing 2026-05-20 (sequel to the broken Phase 2a in tree)
**Author:** michael@bommaritollc.com (research session)
**Scope:** describe a fix for `CONFIG_UM_TEMPLATE_PAUSE_FORK=y` so the
fork-on-resume loop in `arch/um/kernel/template_pause.c` actually works
end-to-end, instead of panicking the master right after the second
`fork()`.  No code is written in this memo; the implementation step list
in §4 is patch-ordered for the next session.

Reading prerequisites: read this memo in full alongside
`09-fork-server-snapshot-restore.md` (the parent design) and
`09-fork-server-STATUS.md` (the failure-analysis ledger). The previous
session's CoW-divergence root cause sketch in STATUS is **superseded by
§1 below** — that theory is mechanically wrong; the real failure mode
is a stub-child aliasing race between the master and the forked child.

---

## 1. Investigation findings

This section captures what the code actually says (with line numbers).
No paraphrasing of the previous session's notes — every claim is
re-derived from a fresh read.

### 1.1 `arch/um/kernel/template_pause.c` (current Phase 1a + 2a)

  * Lines 81–92: `template_pause_setup` parses `um_template_pause` and
    optionally `=fork`; sets `template_pause_armed_flag` and (under
    `=fork`) `template_pause_fork_armed_flag`.
  * Lines 155–174: `one_pause_cycle` — prints "raising SIGSTOP" →
    calls `os_template_pause_stop_self()` (host SIGSTOP/SIGCONT) →
    increments `template_pause_count` → prints "resumed via SIGCONT" →
    reads identity blob.  This is the per-iteration unit the loop body
    invokes.
  * Lines 187–195: `assert_fork_safety` refuses `=fork` under
    `um_backend->kind == UM_BACKEND_KIND_KVM`; same reasoning as
    `snapshot.c::um_snapshot_assert_ready` (line 191).
  * Lines 218–273: `fork_on_resume_loop` — the broken body.  Calls
    `os_snapshot_block_iter_signals()` once at entry, then `for(;;)`:
    `os_template_pause_fork()` → child path: `um_snapshot_worker_init()`,
    unblock, return; parent path: write child pid to memfd, call
    `one_pause_cycle` again.  **Pre-fork: no stub teardown.  Post-fork
    (parent): no stub respawn.  Post-fork (child): only
    `um_snapshot_worker_init()`'s sigio/timer/sched forget+rebuild — no
    stub state touched.**  This is the gap §3 closes.

State this TU owns that is relevant to fork:

  * `template_pause_armed_flag`, `template_pause_fork_armed_flag` —
    `__read_mostly` flags in `.data..read_mostly`; CoW'd into the child
    cheaply.
  * `template_pause_count` — `atomic_t` in `.bss`; CoW'd at first write
    in the parent after fork.
  * Stack: blob, identity_fd, child_pid.  Local to the calling thread.

Assumptions about the process tree: none stated.  The loop body
implicitly assumes the parent and child can both safely return up the
proc_write → vfs_write → sys_write stack and re-enter userspace via
whatever stub the kernel happens to point at — which is exactly the
broken assumption (§1.7 and §2.1).

### 1.2 `arch/um/include/asm/um-template-pause.h` (identity blob)

  * Lines 35–49: `struct um_template_identity` — 4 magic + 4 version +
    64 instance_name + 6 mac + 2 pad + 16 tap_name + 20 ipv4_cidr + 16
    ipv4_gateway + 96 mconsole_path + 32 reserved = 260 bytes total.
  * `os_template_pause_write_child_pid` writes a `__u32` at offset 260
    (one byte past the blob).  Supervisor reads it back from there.

No state owned at fork time beyond layout constants.

### 1.3 `arch/um/os-Linux/template_pause.c` (host helpers)

  * Lines 64–75: `os_template_pause_stop_self` — `kill(getpid(), SIGSTOP)`.
    No mask handling; SIGSTOP cannot be caught or masked, so this is a
    one-syscall primitive.  Returns 0 on resume.
  * Lines 83–101: `os_template_pause_identity_fd` — reads
    `UM_TEMPLATE_IDENTITY_FD` env var, validates with `fcntl(F_GETFD)`,
    returns the fd.
  * Lines 117–125: `os_template_pause_fork` — raw `syscall(__NR_fork)`,
    same mechanics as `os_snapshot_fork_worker`.  This is the call that
    duplicates the master's host process and, critically, **does not
    duplicate the stub child** (the stub is a separate task, cloned
    earlier with CLONE_VM/CLONE_VFORK from the master — see §1.7).
  * Lines 136–160: `os_template_pause_write_child_pid` — `lseek` +
    `write` of 4 bytes to a memfd at the given offset.
  * Lines 169–194: `os_template_pause_read_identity` — `lseek(0)` +
    looped `read` of N bytes.

This TU owns no process-tree state.  All work is pure host syscalls.

### 1.4 `arch/um/kernel/snapshot.c` (the AFL forkserver precedent)

The load-bearing comments are at lines 142–198 (`um_snapshot_assert_ready`)
and 494–615 (`um_snapshot_worker_init`).  Recap:

  * `um_snapshot_assert_ready` runs four invariants: not in IRQ/softirq
    (line 146); no pending non-SIGCHLD signals (151); `num_online_cpus()
    == 1` (156); `um_get_signals() == 1` (161); not under KVM (191).
  * **The decisive passage for our design is the comment at lines 518–522**
    inside `um_snapshot_worker_init`:

    > Seccomp stub children: no stubs exist at ready-point because
    > no guest userspace task has run yet. First user-space syscall
    > in a worker (commit 3d) will lazily spawn one via
    > start_userspace().

    The AFL forkserver's ready point is reached **before any guest
    userspace has run**.  That is why the worker's lazy stub spawn just
    works: the master has no stubs to begin with at fork time, so the
    fork doesn't alias any stub references into the child.  Template
    pause, by contrast, runs **from inside `template_pause_proc_write`
    after init.sh has written to /proc/um/template_pause** — meaning at
    least one stub exists by the time we fork.

  * `um_snapshot_worker_init` (lines 533–615): step 0 disable static
    key; step 1 `os_sigio_worker_forget`, `os_timer_worker_forget`;
    step 1b `sched_worker_detach_other_tasks`; step 2
    `os_sigio_worker_rebuild`, `os_timer_worker_rebuild`.
    **Nothing about stubs.**  Stubs are deliberately out of scope
    (line 520).

  * The forkserver loop blocks UML signal dispatch via
    `os_snapshot_block_iter_signals()` only for the loop body (line
    290); workers do NOT unblock until rebuild is complete (lines
    337–356 commentary).

What state this TU owns at fork time: nothing relevant to stubs.
What it assumes about the process tree: that no stubs exist (513–522)
— the assumption template-pause violates.

### 1.5 `arch/um/os-Linux/process.c` lines 75–300 (snapshot primitives)

  * Lines 123–131: `os_snapshot_fork_worker` — raw `__NR_fork`.
  * Lines 169–205: `os_snapshot_waitpid_status` — raw `__NR_wait4`
    bypassing glibc's cancellation/TLS path.  **Lines 187–192 document
    a wait4 hazard:** even with UML signal gating + raw syscall, the
    parent crashes during the wait. The non-blocking
    `os_snapshot_reap_zombies` (222–242) is the workaround.
    Implication for §3: any pre-fork stub teardown that reaps with
    `waitpid` must use the non-blocking pattern or accept that we
    cannot reap synchronously inside `fork_on_resume_loop`.
  * Lines 263–273: `os_snapshot_block_iter_signals` /
    `os_snapshot_unblock_iter_signals` — flip
    `um_set_signals(0)` saved across the pair.  Used by the current
    `fork_on_resume_loop`; per the wait4 hazard above, **gating UML
    signals does not gate host signals** — `waitpid`/`wait4` in
    blocking form still hits the scheduler-reentry crash mode.
  * Lines 502–522: `init_new_thread_signals` — installs SIGSEGV/SIGTRAP/
    SIGFPE/SIGILL/SIGBUS/SIGIO/SIGCHLD handlers.  SIGCHLD is gated by
    `um_backend->uses_stub_reaper` (line 519), so seccomp has it.

State owned: `os_snapshot_signals_save` static, helper PIDs (per the
sigio sub-module).  No mm/stub state.

### 1.6 `arch/um/os-Linux/mem.c` lines 30–77 (KASAN MADV_DONTFORK skip)

  * Lines 32–47: `kasan_map_memory` mmaps the shadow with `MAP_PRIVATE
    | MAP_ANONYMOUS | MAP_NORESERVE` and `MADV_DONTDUMP`.
  * Lines 49–77: under `CONFIG_UM_FUZZ_HOOKS`, the `MADV_DONTFORK` call
    is **skipped** so the shadow propagates via CoW to the forked
    worker.  `CONFIG_UM_TEMPLATE_PAUSE_FORK` `select`s
    `UM_FUZZ_HOOKS` (Kconfig line 573) and so inherits this.

State owned: a 16 TB KASAN-shadow VA range that is CoW-shared between
the master and any forked child.  No stub state.  This means after fork
the child's KASAN reads/writes are local to its CoW page; no aliasing
with the master.

### 1.7 `arch/um/os-Linux/skas/process.c` ENTIRELY

This is the load-bearing TU for the bug.  Key passages:

  * Lines 376–476: `start_userspace(struct mm_id *mm_id)`.  Allocates a
    1-page temp stack (mmap private/anon), creates a socketpair (line
    409, NO O_CLOEXEC by design — line 401–408), seeds
    `proc_data->futex = FUTEX_IN_CHILD` (lines 423–424), then:

    ```
    mm_id->pid = clone(userspace_tramp, (void *)sp,
                       CLONE_VFORK | CLONE_VM | SIGCHLD,
                       (void *)&tramp_data);
    ```

    The flags matter: `CLONE_VFORK` (parent blocks until child execs),
    `CLONE_VM` (share mm_struct with master), `SIGCHLD` (deliver
    SIGCHLD to master when stub dies).  After `clone`, the master
    `wait_stub_done_seccomp(mm_id, 1, 1)` (line 442) for the stub to
    futex-acknowledge readiness.  Munmaps temp stack, closes one half
    of the socketpair, retains the other (`mm_id->sock`) for the
    seccomp FD-passing channel.

  * Lines 203–275: `userspace_tramp` — the child side of `clone`.
    Resolves `init_data.stub_code_fd` / `stub_data_fd` via
    `phys_mapping(uml_to_phys(...))` — these are **memfd-backed
    physmem file descriptors**, not anonymous mappings.  Closes all
    other fds via `close_range`.  Writes init_data to the socketpair.
    `execveat(stub_exe_fd, ...)` into the stub binary.

  * Lines 82–179: `wait_stub_done_seccomp(struct mm_id *, running,
    wait_sigsys)`.  The futex round-trip with the stub child.
    `data->futex = FUTEX_IN_CHILD` + FUTEX_WAKE → wait for stub to flip
    back to FUTEX_IN_KERN.  Inside the wait loop checks
    `UM_USER_READ_ONCE(mm_idp->pid) < 0` (line 139, 152) to bail if the
    stub died.

  * Lines 49–80: `send_stub_syscall_fds` — sendmsg with SCM_RIGHTS on
    `mm_idp->sock`.  This is how the master passes syscall-fd-map
    entries (e.g., the host fd to munmap) to the stub for it to actually
    invoke the syscall with the correct fd.

State this TU owns that is relevant to fork:

  * `stub_exe_fd` (file-static int): the executable memfd for the
    stub binary.  Held open for the kernel's lifetime; inherited via
    fork (no CLOEXEC issue because the fd table is duplicated).
  * `using_seccomp` (int) — old global, mostly migrated to capability
    flags but still file-static here.  CoW'd at fork.

Process-tree assumptions:

  * `start_userspace` assumes the calling thread (master) is the
    parent of the new clone.  After `clone(SIGCHLD|CLONE_VM|CLONE_VFORK)`,
    the stub is the master's child; SIGCHLD on stub death lands on the
    master (handled by `mm_sigchld_irq` in `arch/um/kernel/skas/mmu.c`
    line 186).
  * `wait_stub_done_seccomp` assumes one — and only one — UML process
    is driving each `mm_id` at a time.  The `mm_idp->sock` is a
    socketpair endpoint; concurrent senders on the same endpoint
    interleave bytes.  **There is no lock around access to mm_id from
    multiple host processes.**

Pre-fork tear-down hooks into this TU: there is no existing entry
point. §3 adds `start_userspace_redo(struct mm_id *)` that kills the
old stub, zero-fills the stub-data page, and re-runs the spawn flow.

Post-fork respawn hooks here similarly — see §3.

### 1.8 `arch/um/include/shared/skas/{skas.h, stub-data.h, mm_id.h}`

  * `struct mm_id` (mm_id.h lines 13–22): `int pid` (the stub child
    host pid), `unsigned long stack` (the kernel VA of the stub_data
    region, allocated via `__get_free_pages` in `init_new_context`),
    `int syscall_data_len`, `int sock` (the socketpair endpoint),
    `int syscall_fd_num`, `int syscall_fd_map[STUB_MAX_FDS]`.
  * `struct stub_data` (stub-data.h lines 55–74): per-mm
    kernel-and-stub shared page laid out as `long err`,
    `int syscall_data_len`, then `syscall_data[N]`, then the SIGSYS
    handshake fields (`restart_wait`, `futex`, `signal`,
    `si_offset`, `mctx_offset`), then `arch_data`, then a
    `UM_KERN_PAGE_SIZE` sigstack.  Total: `STUB_DATA_PAGES *
    UM_KERN_PAGE_SIZE` (= 2 pages, per `as-layout.h` line 26).
  * `skas.h` (lines 23–26): `current_mm_id()` returns
    `&current->mm->context.id`, used by both the seccomp vcpu_run path
    and the stub-data acccessors.

State owned (per mm): the `struct mm_id` plus the 2-page stub_data
region.  Both are part of the UML kernel's per-mm context, allocated by
`init_new_context` (kernel/skas/mmu.c line 83).  No globals.

### 1.9 `arch/um/kernel/skas/{mmu.c, process.c}` (kernel-side stub mgmt)

  * `init_new_context` (mmu.c 47–125): allocates `stub_data`
    (`__get_free_pages(... __GFP_ZERO ...)`), initializes mm_id fields
    (`pid = -1`, `sock = -1`), inserts into `mm_list` (line 101),
    dispatches `um_backend->mm_create` (line 104) which for seccomp
    calls `start_userspace`.
  * `destroy_context` (mmu.c 127–177): un-list, dispatch
    `mm_destroy` (seccomp_mm_destroy → `os_kill_ptraced_process(id->pid,
    1)`), free the 2-page stub_data.
  * `mm_sigchld_irq` (mmu.c 179–217): SIGCHLD irq handler.  Loops
    `os_reap_child()` (WNOHANG) → walks mm_list to find the dead
    pid → marks `mm_context->id.pid = -1` and flips `stub_data->futex
    = FUTEX_IN_KERN` + futex_wake to release any kernel thread blocked
    in `wait_stub_done_seccomp`.
  * `kill_off_processes` (reboot.c 21–38): `for_each_process` →
    `find_lock_task_mm` → `os_kill_ptraced_process(t->mm->context.id.pid,
    1)`.  The pattern §3 reuses for pre-fork tear-down.

`mm_list` is file-local to `mmu.c` (line 28 `static struct list_head
mm_list`).  Iteration from outside that TU requires either a new
public helper or piggybacking on `for_each_process`.  §3 chooses the
latter (cheaper patch, no new global lock interaction).

### 1.10 The Memo 09 design and the STATUS doc

  * Memo 09 §3 Phase 2 (lines 281–293): the kernel applies identity
    (MAC, IPv4, tap fd swap) AFTER the SIGCONT.  Phase 2a's loop body
    must therefore complete cleanly so Phase 2 can hook in.
  * Memo 09 §1 (lines 82–89): take latency target is 5 ms median /
    50 ms p99.  Pre-fork stub teardown adds latency; budget impact is
    discussed in §6.
  * STATUS doc Fix sketch #1 ("tear down stubs in parent pre-fork,
    respawn in both halves") is the right shape.  This memo turns it
    into an executable design.
  * STATUS doc Fix sketch #2 ("early ready point pre-init") is a
    parallel acceptable design; this memo prefers #1 because (a) the
    bootstrap-script-driven seam Phase 1a built is a feature, not an
    accident — it lets the operator run guest setup before pause — and
    (b) sketch #2 still does not solve the stub-aliasing race for any
    OTHER mm that may exist post-fork (init kernel kthread's own mm if
    boot ordering changes, future guest payloads that pre-spawn worker
    processes, etc.).

### 1.11 Answering the specific "how many stubs at template_pause time"

At the moment `template_pause_proc_write` fires:

  * **At least one** stub exists: the writer's mm has been instantiated
    by `init_new_context` → `seccomp_mm_create` → `start_userspace`,
    which spawned a stub child via `clone(CLONE_VM|CLONE_VFORK|SIGCHLD)`.
    The stub is in `wait_stub_done_seccomp`'s futex-wait, holding
    `data->futex == FUTEX_IN_KERN` because the master is currently in
    `handle_syscall` for the `write(2)` that triggered the proc op.
  * **Up to N** more stubs exist: every UML guest userspace task with a
    live mm has its own stub.  In practice with a typical init.sh-driven
    boot, you have 2–4 (init.sh itself, the proc-write redirector child,
    any backgrounded helpers like agetty).
  * **All stubs share the master's mm_struct via CLONE_VM** — they are
    children of the master process (because `clone` was issued from the
    master), and they each map the per-mm stub_data via a memfd at
    `STUB_DATA` (`uml-userspace`'s `mmap` of `init_data.stub_data_fd`
    at `init_data.stub_data_offset` in `stub_exe.c::real_init` lines
    63–69).  The memfd is the **physmem memfd**, shared by everyone.
  * What happens to them on master `fork(2)`: **the stubs are NOT
    duplicated.**  `fork(2)` only duplicates the calling thread of the
    master.  Each stub is a separate host task whose `current->mm` is
    the master's pre-fork mm (M0).  The forked child UML process gets
    a new mm_struct (M1) via host kernel's `dup_mm`, with CoW page
    tables (KASAN shadow excepted per §1.6; physmem-memfd MAP_SHARED
    mappings stay shared per `mm_struct` rules).  Critically: M1's
    UML-level task list (the guest task_structs) is a CoW dup of M0's,
    so the forked child's task structs still reference `mm->context.id.
    pid` values that are the MASTER's stub children — not its own.

### 1.12 Why the previous session's "CLONE_VM CoW divergence" was wrong

The previous session wrote (STATUS doc):

> Stub child STILL has CLONE_VM-shared access to the parent's mm —
> but now any write the parent does triggers CoW, and the new physical
> page is in the parent's mm only.  The stub child still sees the
> ORIGINAL physical page through its CLONE_VM mapping.

This conflates two distinct mechanisms.  CLONE_VM means the stub and
the master share the same `mm_struct` (host kernel object).  Page
tables and CoW state live in `mm_struct`.  When the master `fork(2)`s,
host kernel does `dup_mm`: it creates a NEW `mm_struct` (M1) for the
forked child, write-protects M0's PTEs for CoW, and leaves the MASTER
attached to M0.  The stub child also stays attached to M0 (same
mm_struct).  Any subsequent CoW-fault on M0 splits at the M0 level —
both master AND stub see the new page (they share M0's page tables).
There is no "stub-side ORIGINAL mapping" separate from the master's.

The only thing the stub would see differently from the master is if
the stub were attached to a separately-CoW'd `mm_struct` — but
CLONE_VM rules out that case by construction.

Furthermore, the stub_data page in particular is mmap'd by the stub
binary via `MAP_FIXED | MAP_SHARED` on the physmem memfd (stub_exe.c
line 67).  `MAP_SHARED` mappings of the same file at the same offset
**always** see the same physical pages, regardless of process mm_struct
identity.  So even if the stub were on a separate mm_struct, its
stub_data view would still match the master's, because both go through
the page cache.

The actual failure mechanism is **stub-pid aliasing + concurrent
drivers**, described in §2.

---

## 2. The hazard model

After `os_template_pause_fork()` returns in the parent, the world looks
like this:

```
host process tree (post-fork iter 1):

  supervisor (umlctl pool spawn)
   └── master (M0, original UML kernel)
        ├── SC0   stub child for init.sh's mm, CLONE_VM with M0
        ├── SC1   stub child for /bin/echo's mm, CLONE_VM with M0
        └── M-fork1 (M1, new UML kernel from fork)
             — has its own mm_struct M1 (CoW dup of M0)
             — has its own host fd table (dup of master's)
             — UML task list inherited by CoW: every guest task still
               points at M0's mm_structs via current->mm, which in turn
               point at SC0/SC1 via mm->context.id.pid
             — has NO stub children of its own
             — physmem memfd is fd-table-dup'd; MAP_SHARED stub_data
               pages point at the same page-cache pages as master sees
```

The forked child M-fork1 returns from `um_template_pause_enter`
through `um_snapshot_worker_init` (sigio/timer/sched forget+rebuild)
and unwinds up to `template_pause_proc_write`'s caller.  The kernel
returns to userspace.  M-fork1's next `seccomp_vcpu_run` calls
`current_mm_id()` → `&current->mm->context.id` → `id.pid = SC0_pid`.
M-fork1 calls `set_stub_state` on the shared stub_data, sets
`futex = FUTEX_IN_CHILD`, sends FUTEX_WAKE.

Concurrently the master returns from the first `one_pause_cycle` into
the loop body, writes the child pid to the memfd, enters `one_pause_cycle`
again, prints "raising SIGSTOP" — and is about to `kill(self, SIGSTOP)`.

At this moment:

  * SC0 is waking from FUTEX_WAIT on master's behalf (because the
    initial syscall — `write(2)` to /proc/um/template_pause — is still
    in flight from SC0's perspective).
  * M-fork1 is also waking SC0 — separately, for ITS init.sh's
    return-from-syscall.
  * Both write to stub_data fields (`syscall_data_len`,
    `futex`, etc.).
  * SC0 reads garbage — `si_offset` or `mctx_offset` points at a
    bogus address (the small numbers `0x265d57`, `0x2670fc`, `0x26ac13`
    seen across runs are precisely what you'd expect from a corrupted
    16-bit field re-interpreted as an offset into the sigstack — see
    `stub-data.h` lines 65–67 where these are `unsigned short`).
  * SC0's signal-handler-return path dereferences the bad offset and
    SEGVs at `0x68803bde`, deep inside stub-code.  The faultinfo
    relayed back to UML shows kernel-mode IP, address `< TASK_SIZE`,
    `is_user == false`.  `arch/um/kernel/trap.c` line 372 panics:

    > Kernel tried to access user memory at addr 0x%lx, ip 0x%lx

This is the exact crash signature.

The hazard inventory — per state, with whether
`um_snapshot_worker_init()` handles it today:

| # | State item                                | fork inherits | worker_init today | required |
|---|-------------------------------------------|---------------|-------------------|----------|
| 1 | Stub child pid in `current->mm->context.id.pid` | yes (stale; points at master's SC0) | **not handled** | new helper, §3.3 |
| 2 | `mm->context.id.sock` (socketpair endpoint) | yes (CoW dup, same kernel file)    | **not handled** | new helper, §3.3 |
| 3 | stub_data page (memfd MAP_SHARED)         | yes (shared backing)               | **not handled** | new helper, §3.3 |
| 4 | mm_list entries (file-local to mmu.c)     | yes (CoW dup; entries reference M0 mm_structs) | not handled | new helper iterating via for_each_process, §3 |
| 5 | `write_sigio_td` + epollfd                 | stale handle                       | yes (`os_sigio_worker_forget` + `_rebuild`) | unchanged |
| 6 | POSIX timers (`event_high_res_timer[cpu]`) | stale tid                          | yes (`os_timer_worker_forget` + `_rebuild`) | unchanged |
| 7 | CFS runqueue (kthreads attached)          | yes                                | yes (`sched_worker_detach_other_tasks`) | unchanged |
| 8 | KASAN shadow                              | CoW per §1.6                       | n/a (CoW just works)              | unchanged |
| 9 | `stub_exe_fd` (file-static int)            | yes (fd table dup'd)               | n/a (read-only)                   | unchanged |
| 10 | Master's `SIGCHLD` IRQ registration       | yes (CoW data)                     | partial (rebuild via init_new_thread_signals not called) | **see §3.4** |
| 11 | `mm_sigchld_irq` ↔ `mm_list` interaction  | M-fork1 keeps the parent's IRQ table; SIGCHLD on a stub_child death lands on the master, not on M-fork1 (the master is the stub's parent — fork doesn't reparent) | not handled in child | **see §3.4** |
| 12 | `template_pause_count` atomic_t           | CoW (per-process)                  | n/a                               | unchanged |
| 13 | `os_snapshot_signals_save` static          | CoW                                | n/a                               | unchanged |
| 14 | `cpu_tasks[]` array                        | CoW                                | partial (sched_worker_detach handles their runqueue link, not the array entries themselves) | tolerable for one-shot child; review at §6 |

Items 1–4 are the new work.  Items 10–11 are subtler: the forked
child M-fork1 has no stubs as direct children, so its `SIGCHLD` would
fire only when ITS own future stubs die (which won't happen until
M-fork1 lazily spawns one).  However, the master's `mm_sigchld_irq`
still fires when one of the master's stubs dies — and if the master
has already torn the stub down (§3.2), the master will see the
death-event and the SIGCHLD irq handler will walk mm_list, mark the
pid -1, futex-wake — all idempotent and safe.  Item 11 is documented
for the next maintainer; no code change.

---

## 3. The fix design

The plan in five named pieces.

  **(A) Pre-fork: in the master, kill every stub child and zero its
       stub_data page.**  All UML mm_structs in mm_list survive (we do
       NOT destroy mms), but their backing stub processes are gone and
       their per-mm shared state is reset.
  **(B) Fork.**  Same `os_template_pause_fork()` as today.
  **(C-parent) Post-fork in the master: re-spawn fresh stubs for
       every mm.**  Walk mm_list, call `start_userspace_redo()` for
       each.  Master's task list is unchanged (it's the same UML
       kernel process tree as before the fork).
  **(D-child) Post-fork in the forked child: re-spawn fresh stubs
       for every mm INSIDE the child's process tree.**  Walk the
       inherited mm_list, call `start_userspace_redo()` for each.
       The forked child's task list is the CoW dup of the master's;
       every mm_struct's `id.pid` field is updated to point at a stub
       that is M-fork1's own child.
  **(E) Continue.**  Master loops back to the next SIGSTOP; child
       returns up the stack to its caller and resumes guest userspace
       via its own fresh stubs.

The control flow lives in `template_pause.c::fork_on_resume_loop`.
The respawn primitive lives in `os-Linux/skas/process.c::start_userspace_
redo`.  The iteration helper lives in `arch/um/kernel/skas/mmu.c` (it
needs `mm_list_lock` access, file-local today).

### 3.1 New os-Linux helper: `start_userspace_redo`

In `arch/um/os-Linux/skas/process.c`:

```c
/**
 * start_userspace_redo() - replace a stub child after a fork(2).
 * @mm_id: existing per-mm id; its current pid/sock are killed/closed
 *         in place and a fresh stub child is spawned.
 *
 * Caller has either (a) just emerged from os_template_pause_fork() in
 * the parent and needs every per-mm stub child re-spawned because the
 * previous ones were intentionally killed pre-fork, or (b) is in the
 * forked-child path and inherited stub-pid references that point at
 * the parent's stubs (which exist in a different process tree).
 *
 * Idempotent w.r.t. the kill side: if @mm_id->pid is -1 (already
 * dead/never spawned), the kill is skipped.  Always re-spawns.
 *
 * Returns 0 on success, -errno on clone/socketpair failure.  Failure
 * leaves @mm_id with pid = -1 so subsequent vcpu_run sees a dead-mm
 * state (the existing mm_sigchld_irq logic handles this).
 */
int start_userspace_redo(struct mm_id *mm_id);
```

Body:

  1. If `mm_id->pid > 0`: `kill(mm_id->pid, SIGKILL); CATCH_EINTR(
     waitpid(mm_id->pid, NULL, __WALL))`.  Use raw `__NR_wait4` per
     `os_snapshot_waitpid_status`'s rationale (glibc cancellation
     path is fragile in UML kernel context).
  2. If `mm_id->sock >= 0`: `close(mm_id->sock); mm_id->sock = -1;`.
  3. Zero the stub_data page that backs `(struct stub_data *)mm_id->
     stack`.  This resets `futex`, `signal`, `si_offset`, `mctx_offset`,
     `syscall_data_len` (`syscall_data_len = 0` is critical — a
     leftover from before the kill would cause the new stub's first
     `stub_signal_interrupt` iteration to attempt a recvmsg for a
     non-existent fd map).
  4. `mm_id->pid = -1; mm_id->syscall_data_len = 0; mm_id->syscall_fd_num
     = 0;`
  5. Call the existing `start_userspace(mm_id)`.  This is unchanged
     from its boot-time use — clone, futex round-trip, retain the
     parent-side sockpair fd.
  6. Return the result of `start_userspace`.

Cost: one mmap + one socketpair + one clone + one futex round-trip per
mm.  Empirically `start_userspace` takes ~200 µs in cold boot (per
memo 26 timings); we incur this per mm per take.  With 2–4 mms typical
that's 0.4–0.8 ms — well within the 5 ms median budget (memo 09 §1).

### 3.2 New kernel-side iterator: `um_template_pause_teardown_stubs` and `_respawn_stubs`

In `arch/um/kernel/skas/mmu.c` (so we can access `mm_list_lock`
without exposing it):

```c
/**
 * um_skas_teardown_all_stubs() - kill every stub child in mm_list.
 *
 * Walks mm_list under mm_list_lock and, for each mm with id.pid > 0,
 * sends SIGKILL + reaps the stub.  Sets id.pid = -1 and id.sock = -1
 * for each.  No mm_struct is destroyed.
 *
 * Caller must hold no mm_list_lock and must be in a context where
 * blocking is acceptable (the wait4 inside the helper blocks).
 *
 * Used by the template-pause fork-on-resume loop pre-fork (master
 * side) and post-fork (both halves).
 */
void um_skas_teardown_all_stubs(void);

/**
 * um_skas_respawn_all_stubs() - re-create a stub child for every
 * mm that previously had one (id.pid == -1 sentinel).
 *
 * Returns 0 on success, -errno on first failure (subsequent mms are
 * left torn-down; caller decides whether to retry or fail the take).
 */
int um_skas_respawn_all_stubs(void);
```

Both walk `mm_list` (file-local list).  Implementation iterates with
`scoped_guard(spinlock_irqsave, &mm_list_lock)` (per
`init_new_context` line 99) but drops the lock around the actual
`kill/wait/start_userspace` calls because those block and we cannot
hold a spinlock across host syscalls.  Use the existing pattern from
`mm_sigchld_irq` (line 184–214): take the lock, snapshot the pids,
drop the lock, do the kill/wait, optionally re-acquire to update fields.

Concretely: build a local array of `struct mm_id *` pointers under
the lock, drop the lock, iterate and call `start_userspace_redo` on
each.  `mm_list` cannot shrink while we hold a reference — but at
template_pause time the system is quiet (we're in proc_write with
UML signals gated), so the lock-snapshot pattern suffices without
additional refcounting.

### 3.3 Updated `fork_on_resume_loop` body

Replacement skeleton (pseudo-diff vs current
`arch/um/kernel/template_pause.c` lines 218–273):

```c
static int fork_on_resume_loop(const char *named_point, int identity_fd,
                               struct um_template_identity *first_blob)
{
    struct um_template_identity blob = *first_blob;
    int ret, child_pid;

    ret = assert_fork_safety(named_point);
    if (ret)
        return ret;

    os_snapshot_block_iter_signals();

    for (;;) {
        /* (A) pre-fork: kill all stubs in the master so fork doesn't
         * alias their pids into the child.  Master will respawn after
         * the fork in the parent path; child respawns in its path.
         */
        um_skas_teardown_all_stubs();

        /* (B) fork */
        child_pid = os_template_pause_fork();
        if (child_pid < 0) {
            pr_err("template_pause: fork failed: %d\n", child_pid);
            /* Try to respawn so the master can continue serving the
             * already-running guest userspace if any. */
            (void)um_skas_respawn_all_stubs();
            os_snapshot_unblock_iter_signals();
            return child_pid;
        }
        if (child_pid == 0) {
            /* (D-child) — respawn child's own stubs in its own process
             * tree, THEN do the existing worker_init forget/rebuild.
             * Order matters: worker_init rebuilds sigio/timer state
             * targeting the child's gettid(), which must be valid by
             * then (it is — the child is the calling thread).  Stubs
             * are independent of sigio/timer, so we can do them first.
             */
            ret = um_skas_respawn_all_stubs();
            if (ret) {
                pr_err("template_pause: child stub respawn failed: %d\n",
                       ret);
                os_snapshot_unblock_iter_signals();
                return ret;
            }
            um_snapshot_worker_init();
            os_snapshot_unblock_iter_signals();
            *first_blob = blob;
            return 0;
        }

        /* (C-parent) — respawn master's own stubs.  Master keeps its
         * UML task list intact (fork is a no-op for the master's task
         * list — only the child's gets a CoW copy), so every mm in
         * mm_list still has a valid `id.stack` etc.; we just need
         * fresh stub children.
         */
        ret = um_skas_respawn_all_stubs();
        if (ret) {
            pr_err("template_pause: parent stub respawn failed: %d\n",
                   ret);
            os_snapshot_unblock_iter_signals();
            return ret;
        }

        /* report new child's host pid via memfd[260:264] */
        if (identity_fd >= 0) {
            int werr = os_template_pause_write_child_pid(identity_fd,
                sizeof(struct um_template_identity), child_pid);
            if (werr)
                pr_warn("template_pause: write child pid %d failed: %d\n",
                        child_pid, werr);
        }

        /* (E) next take's pause */
        ret = one_pause_cycle(named_point, identity_fd, &blob);
        if (ret) {
            os_snapshot_unblock_iter_signals();
            return ret;
        }
    }
}
```

Notes on this sequence:

  * **First-iteration vs subsequent-iteration**: handled uniformly.  At
    iteration 1 entry the system has live stubs from boot/init.sh;
    `um_skas_teardown_all_stubs()` reaps them.  After (C-parent)
    respawn the master is back to normal operation.  At iteration 2
    entry the master has live stubs again (from (C-parent) of iter 1);
    same teardown applies.  Idempotency falls out for free because
    teardown checks `id.pid > 0`.

  * **Why not lazy respawn in the child** (analogous to AFL forkserver's
    "no stubs at ready point" plus on-first-syscall spawn)?  Because
    AFL's lazy path relies on the child having NO stubs at fork time.
    Template-pause runs after init.sh has already executed several
    guest userspace tasks, so mms with stubs exist.  Lazy respawn
    would require teaching every `current_mm_id()` consumer to handle
    `id.pid == -1` by calling `start_userspace` first — a much bigger
    invasive change.  Eager respawn in the same code path that
    triggered the fork is local and contained.

  * **The kill-and-respawn cycle does not race with `mm_sigchld_irq`**:
    `os_snapshot_block_iter_signals()` is already active for the loop
    body (line 234 today, retained).  UML's SIGCHLD dispatch into
    `do_IRQ(SIGCHLD_IRQ, ...)` is gated behind `signals_enabled`, so
    incoming SIGCHLDs queue in `signals_pending` and are drained on
    unblock.  The drain at unblock-time will walk mm_list, find pids
    we've already marked -1 (or freshly respawned, with the new pids),
    and do its idempotent thing.

  * **Concurrent guest tasks**: the master may have other UML guest
    tasks (kworker/kthreads with mm, daemon-style guest userspace
    processes).  Their `vcpu_run` paths would race with our pre-fork
    teardown if signals weren't gated.  They are — UML kernel signal
    gate is closed across the entire loop body.  Any task that was
    mid-syscall when we entered the loop is parked in
    `wait_stub_done_seccomp`'s futex; we kill its stub, the
    `UM_USER_READ_ONCE(mm_idp->pid) < 0` check (process.c line 139)
    bails it via `fatal_sigsegv()` — **this is unacceptable for the
    master**, see §3.5 below.

### 3.4 SIGCHLD/IRQ interaction

After (C-parent), the master's stub children are fresh.  After (D-child),
the forked child's stub children are fresh and ARE the forked child's
own children (because `clone` was invoked from the forked child's thread).
SIGCHLD on a child stub dying lands on the right parent in each case.

`init_new_thread_signals` was called once at boot; the forked child
inherits the signal handler registrations.  No need to re-register
SIGCHLD.

`mm_sigchld_irq` is registered via `request_irq(SIGCHLD_IRQ, ...,
NULL)` at `init_child_tracking` (early_initcall).  The IRQ table is
CoW'd; the handler pointer is the same kernel-VA function in both halves.
Works.

### 3.5 The "concurrent guest task" exception — bail-via-fatal_sigsegv

The current `wait_stub_done_seccomp` panics any non-current `mm_id`
caller via `fatal_sigsegv()` (line 177–178) if it loses its stub
mid-wait.  In the template-pause use case this would fire if (a) a
non-template-pause guest task is mid-syscall at the moment we tear
down stubs, AND (b) its kernel thread is parked in
`wait_stub_done_seccomp` for that stub.

Mitigation: **the contract for template_pause armament is "do not run
other guest userspace tasks during the pause window."**  The Phase 1a
selftest already follows this (init.sh writes to /proc/um/template_pause
and that is the only task).  For Phase 2a the same contract is
documented:

  * `Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-next-
    sprint/09-fork-server-snapshot-restore.md` §3 Phase 2 — add a
    paragraph noting that only the calling task can have an in-flight
    syscall at SIGSTOP time.

  * `arch/um/kernel/template_pause.c::assert_fork_safety` — extend
    with a runtime check: walk mm_list (under mm_list_lock), and for
    each mm verify that `stub_data->futex == FUTEX_IN_CHILD` (i.e., the
    stub is running guest code and the kernel is not mid-syscall for
    it).  Refuse the fork if any mm is in mid-syscall state.

  * This is a real but tolerable constraint for the keystone use case:
    syzkaller spawns a single guest payload (the syz-executor), there
    is no daemon hanging around.  The bootstrap script writes to
    /proc/um/template_pause as its last act before becoming the taken
    instance.

### 3.6 KVM-backend interaction

Unchanged.  `assert_fork_safety` continues to refuse fork-on-resume
under `um_backend->kind == UM_BACKEND_KIND_KVM` (`template_pause.c`
line 189–193).  The KVM-aware path remains a future deliverable
(KVM_CREATE_VCPU per child, kvm_run mmap per child).  Phase 2a is
seccomp-only.

### 3.7 Signal-gating discipline

Existing `os_snapshot_block_iter_signals()` at loop entry remains
necessary.  Add a second invariant: between (A) teardown and (C-parent)
respawn, the master CANNOT enter `wait_stub_done_seccomp` for any mm
(its `mm_id->pid` is -1).  Any spurious syscall dispatch from a
non-current task in this window will crash.  Section 3.5 enforces the
"no concurrent tasks" contract at assert time.

`os_snapshot_unblock_iter_signals()` is called only on exit paths.
Pre-fork the entire loop body is gated; identical to today.

### 3.8 Bootstrap-script flexibility (Phase 1a value preserved)

Phase 1a's design point is that the bootstrap script writes to
/proc/um/template_pause AFTER setting up guest userspace state (umlctl
init.phases).  The fix preserves this — we still pause from inside
proc_write, with whatever guest userspace state the bootstrap built.
The teardown+respawn cycle invalidates any in-stub-process state (e.g.,
mmap'd regions in the stub address space) but the stub data
(`stub_data->syscall_data[]`) is the only stub-side state the kernel
relies on, and that's re-bootstrapped on every iteration anyway by
`set_stub_state`.

---

## 4. Step-by-step implementation plan

Six patches, each independently buildable, with checkpatch on every
commit, ordered to keep `tools/testing/selftests/um/template-pause-
smoke` green throughout.  Diffstat estimates are file-size estimates
(LoC including comments).

### Patch 1 — Factor `start_userspace_redo` out of `start_userspace`

**Files touched** (3): `arch/um/os-Linux/skas/process.c` (+50 LoC),
`arch/um/include/shared/os.h` or `arch/um/include/shared/skas.h` (+5),
`arch/um/include/shared/skas/mm_id.h` (no change; just confirm signature
lives somewhere consumable).

**What it does**: extracts the body of `start_userspace` past its
"alloc temp stack" prelude into a shared inner helper `__start_userspace_
clone_and_handshake`, then writes `start_userspace_redo` as
"kill+wait+zero+call inner".  `start_userspace` itself unchanged from
the call-site side — it still calls into the inner helper.

**Acceptance**: existing boot path still works (smoke template-pause).
No behavior change yet.

### Patch 2 — Add `um_skas_teardown_all_stubs` / `um_skas_respawn_all_stubs` in mmu.c

**Files touched** (2): `arch/um/kernel/skas/mmu.c` (+90), one of
`arch/um/include/shared/skas.h` or a new
`arch/um/include/asm/um-skas.h` (+10) for the prototypes.

**What it does**: iterates mm_list under the existing
`mm_list_lock`, snapshots `struct mm_id *` pointers to a small kmalloc'd
array, drops the lock, calls `start_userspace_redo` (or the kill side
only) on each.  Returns a count.

**Acceptance**: not wired in yet; testable via a debugfs poke that calls
the new helper and verifies (via /proc/N/status of the master) that all
stub children were re-spawned.  Add a one-shot selftest:
`tools/testing/selftests/um/skas-restubbing-smoke/`.

### Patch 3 — Wire `assert_fork_safety` to refuse mid-syscall mms

**Files touched** (1): `arch/um/kernel/template_pause.c` (+30).

**What it does**: extends `assert_fork_safety` to also walk mm_list
under mm_list_lock and refuse with -EBUSY if any mm has `stub_data->
futex == FUTEX_IN_KERN` AND `current_mm_id()` does NOT equal that mm
(i.e., that mm is mid-syscall for some OTHER task than the caller).

**Acceptance**: template-pause-smoke unchanged.  A new selftest
(`template-pause-busy-smoke/`) verifies the refusal fires.

### Patch 4 — Replace `fork_on_resume_loop` body with the teardown→fork→respawn sequence

**Files touched** (1): `arch/um/kernel/template_pause.c` (+60, -20).

**What it does**: the §3.3 skeleton.  Adds the teardown call, the
respawn calls in both parent and child paths, error handling for each.

**Acceptance**: `pool-spawn-smoke` selftest extended to drive 3 takes
sequentially against one master, all succeed.  Crash IP `0x68803bde`
does not recur (§5 prediction).

### Patch 5 — Multi-take pool-spawn smoke

**Files touched** (2): `tools/testing/selftests/um/pool-spawn-smoke/run.sh`
(+50), `tools/uml/uml-launcher/src/bin/umlctl/pool.rs` (small refactor
to allow `pool spawn --takes N`).

**What it does**: drives N consecutive takes (SIGCONT, read child pid
from memfd, send SIGCONT, repeat) and asserts all child pids are
distinct + reachable via mconsole.

**Acceptance**: the selftest passes; N=10 works.

### Patch 6 — Documentation refresh

**Files touched** (3): the design memo §3 Phase 2 (clarify the
"no concurrent tasks" contract), STATUS doc (mark Phase 2a as LANDED
with the new commit hash), this PHASE2A-DESIGN doc moved to a tracking
location or marked DONE.

**No code.**

Total estimated diff: +295 LoC across 6 patches.  The bulk is in
patches 1 and 2; patch 4 is the smallest and most "interesting"
commit — but only correct if 1+2+3 are landed first.

---

## 5. Falsifiable predictions

These are the checks the next session can run AGAINST THE CODE before
building, to validate the design.  Each is grounded in a specific code
path that should change observably.

  **P1.** After patch 4 (and only patch 4 — patches 1–3 don't touch the
  loop body), `pool-spawn-smoke --takes 2` should NOT panic with
  "Kernel tried to access user memory" at IP `0x68803bde`.  The
  crash IP for any new panic should be outside `STUB_START ..
  STUB_END` (i.e., not in stub-trap code).  Verifiable by inspecting
  the dmesg after a failing run; predicted to no longer match
  `0x6880xxxx`.

  **P2.** After patch 4, calling `um_skas_teardown_all_stubs()`
  in a paused master and then reading `/proc/<master_pid>/task/*` from
  the supervisor should show zero alive stub-child threads attached
  to the master.  Verifiable via `ls /proc/<master>/task` count
  before vs after the call.

  **P3.** After patch 4, the second `one_pause_cycle` ("raising SIGSTOP"
  log line #2) should be followed by a clean "resumed via SIGCONT"
  log line — proving the master made it past the previously-broken
  point.  Verifiable in the dmesg of a 2-take run.

  **P4.** The bad address pattern (`0x265d57`, `0x2670fc`, `0x26ac13`
  — all 16-bit-ish small numbers) is consistent with corrupted
  `unsigned short` fields in stub-data.h (`si_offset`, `mctx_offset`
  at lines 65–67 are `unsigned short`).  After patch 4, if a new
  crash occurs, its address pattern should NOT be 16-bit-ish — the
  stub-data fields are no longer being concurrently written.
  Verifiable in any future crash signature.

  **P5.** Pre-patch, `grep -c 'mm_id->pid =' arch/um/` finds
  initializations only in `init_new_context`, `start_userspace`, and
  `mm_sigchld_irq`.  After patch 1, `start_userspace_redo` is a fourth
  writer of `mm_id->pid`.  Verifiable post-patch by re-grepping.

Two of these (P1, P3) are verifiable only by running; the other three
(P2, P4, P5) are verifiable by reading code or by simple host-side
process introspection — within scope of this research-only session
extended to verification by the next session.

Within THIS session I've verified by reading:

  * P5: `grep` of the current tree shows exactly three writers of
    `mm_id->pid` — confirmed via the file reads in §1.7, §1.8, §1.9.

  * The mechanism behind P4: `struct stub_data::si_offset` and
    `mctx_offset` are `unsigned short` (`stub-data.h` line 66 + 67) —
    so their value range is 0..65535.  Adding a base sigstack address
    (which lives at a high address with low bits at zero modulo
    page size) and an unsigned-short offset gives an absolute address
    where the low 16 bits look small.  This matches the observed
    `0x2670fc` etc. (≈ 0x6700 in the low half) UNDER THE ASSUMPTION
    that the sigstack base in the corrupt run was around `0x260000`
    (a glibc / heap range).  Sufficient for "yes, this hypothesis fits
    the data."

---

## 6. Risk register

| Risk | Likelihood | Severity | Mitigation |
|------|------------|----------|------------|
| **R1** `wait4` in `start_userspace_redo`'s kill path hits the same crash mode as `os_snapshot_waitpid_status` (process.c 187–192) | medium | high | use raw `__NR_wait4` per the existing doc; if still buggy, use `__WNOHANG` poll loop with `os_snapshot_reap_zombies`-style drain plus a 100 ms timeout |
| **R2** A guest task other than the writer is mid-syscall when teardown fires; `wait_stub_done_seccomp` panics via `fatal_sigsegv` | low (no other tasks in keystone use case) | medium | §3.5 — `assert_fork_safety` walks mm_list, refuses if any mm has FUTEX_IN_KERN owned by another task |
| **R3** Per-take latency budget blown by N stub respawns | medium for N>=8 | low | budget: ~200 µs per respawn; for syzkaller's typical 2–4 mms, 0.4–0.8 ms — within 5 ms median.  If real workloads have >>4 mms, add a "freeze mm_list" pass that destroys all-but-one mm pre-fork, so we respawn only one stub |
| **R4** KASAN false positives from CoW'd shadow in M-fork1 | low | low | already exercised by the existing AFL forkserver; same code path; covered by `CONFIG_UM_FUZZ_HOOKS` MADV_DONTFORK skip (mem.c line 70) |
| **R5** Race between `mm_sigchld_irq` and `um_skas_teardown_all_stubs` (both want to walk mm_list, both want to update id.pid) | low | medium | UML signal gate is closed across the whole loop body; SIGCHLD queues in signals_pending and is drained on unblock — at which point the teardown is finished and any drain is a no-op |
| **R6** The forked child's `request_irq(SIGCHLD_IRQ, ...)` is shared with the master via fork's CoW; if M-fork1's SIGCHLD irq fires it'll walk M-fork1's mm_list (the CoW copy), which is correct | — | — | this is a property, not a risk; documented in §3.4 |
| **R7** `__GFP_ZERO` was the initial alloc, but post-respawn the stub_data page is NOT zeroed — leftover stub state could trip the new stub | high (without fix) | high | §3.1 step 3: zero the page explicitly inside `start_userspace_redo` |
| **R8** The supervisor reads memfd[260:264] before the master writes it in `fork_on_resume_loop` — race | already handled today | — | parent always writes child pid before re-pausing; supervisor only reads after seeing the next SIGSTOP via WUNTRACED |
| **R9** `start_userspace` itself fails on the second call (e.g., fd table full) | low | high | propagate -errno from `start_userspace_redo`; loop returns and master process exits; supervisor detects and reports |
| **R10** Patch 3's mid-syscall detector has a TOCTOU between the assert and the fork | medium | medium | UML signals are gated; the only writer of `stub_data->futex` between assert and fork is the master itself (no other task can run) — the TOCTOU window is zero in practice |
| **R11** UM_WORKER_PROCESS=y path (memo 28 E.3d.0): stub is owned by a per-mm worker process, not by master | unknown | medium | for v1 of this fix, refuse fork-on-resume when `mm->context.worker != NULL` (mirror `assert_fork_safety`); worker-process integration is a follow-on patch |

Recommendation to defer: R11 (UM_WORKER_PROCESS) gets explicit refusal in
`assert_fork_safety` and is filed as Phase 2b.  The syzkaller use case
does not need WORKER_PROCESS=y (memo 28's deliverable is independent).

---

## 7. Single-paragraph summary

**Root cause**: the previous session's "CLONE_VM CoW divergence"
explanation is mechanically wrong (CLONE_VM means shared mm_struct,
which means CoW splits propagate to both sides; furthermore stub_data is
mapped MAP_SHARED from the physmem memfd and is not subject to CoW at
all).  The actual failure is **stub-pid aliasing + concurrent drivers**:
when the master forks, the forked child M-fork1 inherits an UML task
list whose `current->mm->context.id.pid` fields point at the master's
stub children.  Both the master and M-fork1 then race to drive the
same stubs — concurrent writes to `stub_data->{futex, si_offset,
mctx_offset, syscall_data_len}` corrupt the `unsigned short` offset
fields, and the stub's next signal-handler-return dereferences a
garbage `unsigned short`-corrupted offset and SEGVs deep in stub
code (IP `0x68803bde`); UML's trap path interprets the stub-code IP
as a kernel-mode fault and panics.

**Most important design decision**: kill every stub child IN THE MASTER
PRE-FORK, then re-spawn fresh stubs IN BOTH halves POST-FORK.  This
removes the aliasing entirely — after the fork the master and M-fork1
each own their own private set of stub children with no shared host
process to race over.  The cost is ~200 µs per stub respawn (one
`clone(CLONE_VFORK|CLONE_VM|SIGCHLD)` + one socketpair + one
mmap+munmap of the trampoline stack); with the keystone use case's 2–4
mms typical, well within the 5 ms median take-latency target.

**Smallest patchable unit to start with**: **Patch 1** — extracting
`start_userspace_redo` out of `start_userspace` in
`arch/um/os-Linux/skas/process.c`.  It's a pure refactor with no
behavior change, the boot path keeps working unchanged, and it unblocks
Patches 2–4 each of which builds on the new helper.  Until Patch 1 is
in, the rest is paper.
