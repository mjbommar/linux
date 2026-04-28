# Memo 28 — Memo 25 R4 (per-mm host worker process) — design lock

**Date:** 2026-04-28
**Audience:** Whoever implements memo 25 refactor 4
**Companion memos:** 24 (item #2: per-mm process is the bug-magnet
fix); 25 (R4 plan); 26 Phase A (the v2 backend that lands on top)
**Predecessor session work:** memos 21-27, R10/R11/R1/R2/R3/R5/R7/R8/R9/R12 all
landed 2026-04-28 (commits a5a87536d187 through 3e9ea2505b87)

R4 is the deepest of memo 25's prerequisite refactors. Memo 25's
single-paragraph plan is correct in shape but underspecified for
implementation; this memo locks in the design decisions an
implementer needs and lays out the concrete commit sequence.

---

## Part A — Why R4

Today's UML host process model is one Linux process running every
guest task as a `clone(CLONE_VM | CLONE_VFORK | SIGCHLD)` host child.
**ALL host children share the UML host process's VA space.** Per-mm
separation is software-only via the seccomp stub-child machinery.

Consequences:
- Cross-mm host-VA collision is a real bug class (memo 22 BUG.1
  family; memo 24 item #2).
- v1's shadow-PT was forced to fake mm separation in software
  because the host VA space was shared.
- v2's TDP path needs each mm to have its own host process so KVM
  memslots and `mm->pgd` don't collide.

R4 untangles this: **one host process per UML kernel (the
"spawner"); one host process per guest mm (a "worker"); pthreads
within each worker for the guest tasks that share that mm.**

After R4, the bug-magnet collapses structurally — different mms
genuinely have different host VA spaces. v2 lands on top
naturally.

---

## Part B — Surface map (from Explore subagent, 2026-04-28)

Full report in session transcript. Headlines below.

### B.1 — clone() / userspace_tramp call sites

**Spawner-only (run once at UML boot, in the host parent — these stay):**
- `arch/um/os-Linux/start_up.c:147` — `seccomp_helper` clone in boot probe (`init_seccomp`); one-time capability detection. Migrates into spawner unchanged.

**Per-task / per-mm (this is what R4 restructures):**
- `arch/um/os-Linux/skas/process.c:386` — `start_userspace()` calls `clone(userspace_tramp, ..., CLONE_VFORK | CLONE_VM | SIGCHLD)`. Today: one stub child per mm. After R4: one **worker process** per mm via clone-without-CLONE_VM, plus the seccomp stub child inside the worker.
- `arch/um/os-Linux/skas/process.c:163-235` — `userspace_tramp()` is the clone child entry point. After R4 it's a per-mm worker bootstrap (open IPC, fork seccomp stub, run trap loop).

**Pure thread / pthread sites (already match R4's intra-worker model — these stay):**
- `arch/um/os-Linux/skas/process.c:466-484` — `new_thread()` / `switch_threads()` (jmp_buf longjmp).
- `arch/um/os-Linux/skas/process.c:492-534` — `start_idle_thread()`.
- `arch/um/kernel/process.c:75-85` — `__switch_to()` dispatching `context_switch` op.
- `arch/um/kernel/smp.c:161-162` — `new_thread()` for SMP idle.

### B.2 — IPC + signal surface

| Component | Today | After R4 |
|---|---|---|
| futex (stub child wake) | spawner ↔ stub child | spawner ↔ worker (worker hosts the stub child internally) |
| socketpair (init data, FD passing) | spawner ↔ stub child | re-keyed: spawner ↔ worker for per-task FD updates |
| SIGCHLD handler | one stub child per mm | one worker per mm; mm_sigchld_irq scans worker list |
| SIGSYS handler | stub child | worker process (the seccomp child lives inside the worker) |
| signal-mask | spawner | worker (guest-mode signal delivery is in the worker) |
| **NEW** spawner ↔ worker IPC | — | UNIX socket per worker (see Part C.A) |
| **NEW** cross-worker task migration | — | SIGUSR1 (see Part C.C) |

### B.3 — Per-mm state today

`struct mm_context` (`arch/um/include/asm/mmu.h:14-24`):
```c
typedef struct mm_context {
    struct mm_id id;                    /* PID, stack, sock, FD map */
    struct mutex turnstile;             /* serializes mm_map / mm_unmap */
    struct list_head list;              /* global mm_list for SIGCHLD */
    spinlock_t sync_tlb_lock;
    unsigned long sync_tlb_range_from;
    unsigned long sync_tlb_range_to;
} mm_context_t;
```

After R4: `mm_context` gains a `struct um_worker *worker` field (or
similar) pointing at the per-worker IPC channel. `mm_id` becomes
worker-internal (stays seccomp-internal post-R2; R4 makes it
literally not visible from spawner code).

### B.4 — Context-switch hot path stays cheap

Today: `__switch_to → seccomp_context_switch → switch_threads()`
is a pure userspace `longjmp`. **No host syscall.** After R4 this
stays the same for intra-worker task switches (most context
switches). Cross-mm migration (rare; cross-mm CLONE_VM tasks) gets
the new SIGUSR1 path (see Part C.C). The hot path is unchanged.

---

## Part C — Design decisions (locked)

### C.A — IPC mechanism: **UNIX socket** for MVP

| Option | Pros | Cons |
|---|---|---|
| UNIX socket per worker | simple; mirrors today's socketpair; kernel handles backpressure; ~400 ns | not the fastest |
| Shared-mem futex ring | ~100 ns; gVisor pattern | complex; cache-line ping; needs careful NUMA |
| io_uring SQPOLL | exit-less, ~zero-overhead | requires 6.1+; sophisticated; future opt |

**Locked: UNIX socket per worker.** Mirrors today's socketpair
mechanics; single producer-consumer per pair; trivial backpressure
via socket buffer. Re-evaluate to a shared-mem ring after profiling
shows IPC dispatch is a measurable bottleneck (memo 26 Phase H
performance work). The socket protocol is a tagged binary message
(see Part D for the wire format).

### C.B — Worker reaping: **SIGCHLD on spawner**

| Option | Pros | Cons |
|---|---|---|
| SIGCHLD handler on spawner | mirrors today's mm_sigchld_irq pattern; immediate detection | signal handler complexity |
| IPC ping/pong heartbeat | no signal handler | delayed detection (~100ms); extra IPC traffic |
| Hybrid: SIGCHLD as wake-only + waitpid() | clean separation | one extra step |

**Locked: SIGCHLD + mm_list scan,** expanded from today's single-stub-child
pattern to multi-worker. The hybrid (option 3) is a refinement we
can land later if the mm_list scan shows up in profiles.

### C.C — Cross-mm task migration: **SIGUSR1**

| Option | Pros | Cons |
|---|---|---|
| SIGUSR1 | userspace-handled; can nest; standard | shared with PM wake (already used) |
| SIGUSR2 | unused | same shape as SIGUSR1 |
| RT signal | queued, has parameter | requires rebooking; small additional complexity |
| Schedule-time hook (no signal) | avoids signal complexity | requires interposing on every schedule() |

**Locked: SIGUSR2.** SIGUSR1 is already taken by `register_pm_wake_signal`.
SIGUSR2 is unclaimed in UML's signal map. Worker's signal handler
checks if the current task has a pending mm-migration request; if
yes, longjmps to the new mm's worker via the IPC ring (which then
forwards the task's saved registers to the destination worker).

### C.D — Spawner thread pool: **one thread per worker**

| Option | Pros | Cons |
|---|---|---|
| One thread per worker | simple; standard posix; blocks on recvmsg | scales poorly at 1000+ workers |
| Epoll-multiplexed single thread | one thread; nonblocking I/O | more complex; needs fairness |
| io_uring SQPOLL | most efficient | future opt |

**Locked: one thread per worker.** UML typical workloads have
1-16 workers (one per active mm); thread overhead is negligible.
Epoll multiplexing is the right answer if we later see a
high-mm-churn fuzz-style workload, but not now.

### C.E — `current` discipline for handle_syscall: **wait-queue bounce**

| Option | Pros | Cons |
|---|---|---|
| Wait-queue bounce (kthread routes; originating guest task runs handle_syscall) | matches today's invariant exactly; full credentials/files/fs/seccomp/signal context | one extra wakeup per syscall (~hundreds of ns) |
| Per-mm "guest task" task_struct | dispatcher impersonates; no extra wakeup | requires cloning credentials/files/fs from originating task; doesn't help SMP-within-mm; expensive allocation |
| `kthread_use_mm` only | sets `current->mm`; minimal ceremony | only fixes mm; `current->files`, `->seccomp`, `->signal`, etc. still wrong |

**Locked: wait-queue bounce.** Per Part L.2.A and Part I.5
("spawner-owns-everything, gVisor sentry pattern"). The
dispatcher kthread (E.3c) does NOT call `handle_syscall` directly
— it routes the inbound `WORKER_MSG_SYSCALL_REQ` to the
originating guest task's per-mm wait queue. The guest task wakes
on its own kernel stack, runs `handle_syscall` under its real
`current` (full credentials, files, seccomp, signal table), then
the kthread sends `WORKER_MSG_SYSCALL_REP` back to the worker.

The ~hundreds-of-ns wakeup cost is well within memo 26 Phase H's
≤1.2× seccomp wall-clock budget. The correctness payoff is total:
every `sys_call_table[]` entry that derefs `current` gets the
same guest task it would have under today's seccomp model.

E.3c's dispatcher as it ships today calls `handle_syscall(&regs.regs)`
directly — that's only safe for the smoke-test sentinel (which
doesn't reach a real syscall). E.3d.1 replaces that direct call
with the wait-queue bounce. Until E.3d.1 lands, no real
SYSCALL_REQ traffic flows through the dispatcher (E.3d.0 brings
up the worker's stub child but doesn't yet route vcpu_run
through IPC; the seccomp backend stays on its in-spawner path).

---

## Part D — Spawner ↔ worker IPC wire format

```
struct worker_msg {
    u32 magic;          /* 0x554D5734 = "UMW4" */
    u16 type;           /* enum worker_msg_type */
    u16 flags;
    u64 task_handle;    /* opaque per-task token */
    union {
        struct worker_msg_syscall  syscall;   /* type = SYSCALL_REQ */
        struct worker_msg_reply    reply;     /* type = SYSCALL_REP */
        struct worker_msg_signal   signal;    /* type = SIGNAL_DELIVER */
        struct worker_msg_migrate  migrate;   /* type = MIGRATE_TASK_TO */
        struct worker_msg_quiesce  quiesce;   /* type = QUIESCE_REQ/REP */
        u8                         pad[120];  /* total 128 B */
    } u;
};
```

Message types:
- `SYSCALL_REQ` (worker→spawner): worker requests host syscall execution.
- `SYSCALL_REP` (spawner→worker): syscall return value + side-effects.
- `SIGNAL_DELIVER` (spawner→worker): inject signal into specific task.
- `MIGRATE_TASK_TO` (spawner→worker): receive a task from another worker.
- `QUIESCE_REQ` (spawner→worker): worker stops scheduling tasks (used during fork).
- `QUIESCE_REP` (worker→spawner): acknowledged.

128-byte fixed message size keeps the wire stable across versions
and allows reading exactly one message per `recvmsg()` call.
Variable-length payloads (e.g., FD passing for SCM_RIGHTS) ride on
the `cmsg` channel of the socket.

---

## Part E — Implementation plan (commit sequence)

R4 lands in **6 commits**. Each builds + boots seccomp 21/21 before
proceeding to the next.

### E.1 — Scaffolding (1 commit, ~150 LoC)

- `arch/um/include/shared/worker_api.h` — public spawner ↔ worker API
- `arch/um/backend/seccomp/worker_ipc.h` — wire format (struct above)
- `arch/um/Kconfig` — new `CONFIG_UM_WORKER_PROCESS` knob (default n
  for now; the toggle that flips R4 on)
- `arch/um/include/asm/mmu.h` — add `struct um_worker *worker` field
  to `mm_context` (NULL-initialized)
- Stub `struct um_worker` definition + zero accessors

Build verify: defconfig + WORKER_PROCESS=n stays today's behaviour;
WORKER_PROCESS=y compiles but spawner scaffold is no-op.

### E.2 — Spawner skeleton (1 commit, ~250 LoC)

- `arch/um/os-Linux/spawner.c` — new TU. Owns the spawner-side
  state: list of workers, socket array, dispatcher thread per
  worker.
- `arch/um/include/shared/spawner.h` — kernel-side declarations.
- Spawner init runs at UML boot; allocates resources but doesn't
  spawn any workers yet (no mm has been created).
- Spawner shutdown frees resources during reboot/halt.

Build verify: WORKER_PROCESS=y boots; spawner init logs
"um: worker model: spawner ready"; no workers exist; gate still
21/21 because seccomp falls back to the old `start_userspace()`
path when `mm->context.worker == NULL`.

### E.3 — Worker process spawn/reap (1 commit, ~300 LoC)

- `arch/um/backend/seccomp/worker.c` — worker bootstrap (clone
  without CLONE_VM; fork seccomp stub child inside the worker;
  set up SIGSYS handler; open IPC socket back to spawner).
- `arch/um/backend/seccomp/seccomp_backend.c` —
  `seccomp_mm_create()` now spawns a worker via the spawner instead
  of calling `start_userspace()` directly.
- `seccomp_mm_destroy()` reaps the worker (tells spawner to send
  SIGTERM; spawner's SIGCHLD handler updates mm_list).
- Worker's syscall dispatch loop: receive `SYSCALL_REQ` from
  spawner, execute via local stub child, send `SYSCALL_REP`.

Build verify: WORKER_PROCESS=y boots; first user mm creates a
worker; gate runs through worker IPC; 21/21 expected.

### E.4 — Per-task pthread inside worker (1 commit, ~150 LoC)

- Worker creates one pthread per guest task (via
  `clone(CLONE_VM|CLONE_THREAD|CLONE_SIGHAND|CLONE_FILES)`).
- `seccomp_thread_create` op now allocates a worker pthread.
- `seccomp_context_switch` op stays the same (longjmp inside the
  worker's process — same VA, same signal table).
- Cross-worker switching not yet implemented; tasks are pinned
  to their birth worker.

Build verify: per-mm worker spawns multiple guest tasks as
pthreads; intra-mm context switches work; gate 21/21.

### E.5 — Cross-mm task migration (1 commit, ~200 LoC)

- Spawner sends `MIGRATE_TASK_TO` to source worker (with target
  worker socket + task token).
- Source worker sends task's saved registers to destination worker
  via the IPC ring.
- Destination worker receives, allocates pthread, sends
  `MIGRATE_TASK_TO_ACK`.
- SIGUSR2 wakes whichever worker is currently running the task to
  initiate the longjmp into the migration handler.

Build verify: clone(CLONE_VM=N) — i.e. fork that creates a new mm
— migrates the task to the new worker. Gate 21/21.

### E.6 — Flip the toggle (1 commit, ~50 LoC)

- defconfig sets `CONFIG_UM_WORKER_PROCESS=y`.
- `__binary_start_hva` Kconfig dependency updated for v2 (sets the
  ground for memo 26 Phase B's high-VA flip).
- Documentation update.

Build verify: WORKER_PROCESS=y is the default; gate 21/21.

---

## Part F — Verification gates

After each commit:
- `make ARCH=um O=/tmp/uml-clean -j$(nproc)` clean.
- `bash /tmp/parity-seccomp-only.sh` (the 21-module curated bar) returns 21/21.
- `single_dlopen × 30` runs flake-free under seccomp (memo 22's
  v1-flake reproducer must stay 0% under seccomp throughout R4).

After all 6 commits:
- Single binary boots both with WORKER_PROCESS=n (today's path) and
  WORKER_PROCESS=y (R4 path); both 21/21.
- ftrace `um_backend:*` events fire correctly under WORKER_PROCESS=y.
- `mm_list` correctly tracks workers across fork/exec sequences.

---

## Part G — Regression risks (highest-priority)

1. **Trap-loop refactor (E.3, E.4):** Today's seccomp trap loop
   talks directly to the stub child via shared mm_id stub_data.
   After R4 it talks to the worker via IPC. Any wire-format mistake
   stalls the guest task. **Mitigation:** assertion on every IPC
   message type; per-message size validation; logging of last 16
   IPC roundtrips on panic.

2. **SIGCHLD multi-worker reap (E.2, E.3):** mm_sigchld_irq() must
   correctly identify which worker died and notify only the affected
   mm. **Mitigation:** worker reap state machine
   (running / dying / reaped); spinlock around mm_list traversal;
   watchdog thread that detects orphaned workers (no SIGCHLD seen
   after kill).

3. **Cross-mm migration (E.5):** SIGUSR2 lost or delivered while
   task is in a non-resumable signal handler causes silent task
   loss. **Mitigation:** worker validates SIGUSR2 receipt before
   permitting task to schedule; spawner times out and retries
   migration after 5 seconds.

4. **FPU state per-worker pthread (E.4):** today's seccomp
   architecture has one stub child per mm; FPU state is
   per-pthread (kernel saves/restores via x86_64 path). After R4
   each worker has N pthreads each with its own FPU; the
   `arch_switch_to(current)` call in `__switch_to` must continue to
   work without modification. **Verification:** numeric Python
   tests (test_decimal, test_float) under WORKER_PROCESS=y must
   match WORKER_PROCESS=n.

5. **fork() + exec() ordering:** Today's clone-with-CLONE_VM means
   fork is cheap (no mm copy). After R4, fork = spawn new worker +
   migrate task. Exec must quiesce the destination worker, then
   replace task state. **Mitigation:** `QUIESCE_REQ`/`QUIESCE_REP`
   handshake before any cross-worker mm change; explicit ordering
   tests (test_subprocess, test_os.fork).

---

## Part H — External patterns we borrow from

- **gVisor (`pkg/sentry/platform/kvm/`):** per-mm `addressSpace` +
  shared vCPU pool. R4 takes the per-mm host-process idea but
  doesn't pool vCPUs (memo 26 Phase C handles that).
- **Firecracker / kvmtool:** single-mm-per-process model.
  Influences the per-worker simplicity but R4 needs multi-task
  support inside each worker.
- **SECCOMP/BPF + futex** pattern from Berg's seccomp work
  (`arch/um/backend/seccomp/`). R4 keeps this verbatim inside the
  worker.

---

## Part I — What R4 does NOT do

- Does not introduce v2's KVM ops (memo 26 Phase A).
- Does not touch the per-CPU vCPU pool design (memo 26 Phase C).
- Does not change the trap loop's trap classification (R9 deferred
  to Phase D).
- Does not move uml_physmem to PML4[256+] at runtime — that's R1's
  Kconfig flip which lands with memo 26 Phase B.

R4 is purely the host-process / threading / IPC restructure that
v2 will run on top of, and that seccomp will continue working
under.

---

## Part I.5 — Kernel-state ownership clarification (added 2026-04-28 evening)

R4's spawner-vs-worker split implies a question memo 25's R4 plan
left implicit: **where does the UML kernel state live?**

UML's "kernel" is the Linux kernel built for ARCH=um, running as
a host process. mm_struct, task_struct, scheduler, page tables,
syscall dispatch — all of it. Today (pre-R4) the answer is
trivial: there's one host process; the kernel state is in it.
After R4 with separate worker processes, this is no longer
trivial.

Three candidate models:

1. **Spawner-owns-everything (gVisor sentry pattern).** Kernel
   state lives in the spawner only. Workers are minimal trap
   relays: SIGSYS → IPC to spawner → spawner runs handle_syscall
   → reply. Worker has no `task_struct`, no `mm_struct`, no
   scheduler. Cost: every syscall is a cross-process IPC
   round-trip (~400 ns over a UNIX socket). Benefit: simple
   ownership story; spawner is the single source of truth.

2. **Worker-owns-everything (per-mm independent kernel).** Each
   worker runs a copy of the UML kernel forked from the spawner;
   diverges over time. Spawner is just a coordinator. Cost: kernel
   state diverges; cross-mm fork/exec gets very complicated;
   migrating tasks across mms means migrating their full kernel
   context. Benefit: no syscall IPC.

3. **Hybrid (shared kernel state via shared memory).** mm_struct
   etc. in shared memory; spawner + worker both touch it under
   locks. Cost: high — every kernel access is potentially a
   cache-line bounce; locking discipline is fragile. Benefit:
   in-process syscalls.

**Locked: option 1 (spawner-owns-everything).** Matches gVisor;
single source of truth; simplest ownership; the IPC cost is
acceptable for v2's targets (memo 26 Phase H budgets ≤1.2×
seccomp wall-clock and seccomp's own SIGSYS+futex round-trip is
~300-500 ns; an extra socket round-trip per syscall puts v2 at
~600-900 ns, which is still well under 1.2× of typical seccomp
workloads).

Concretely for E.3+:
- Worker's "trap loop" is minimal: receive `MIGRATE_TO` from
  spawner (with task registers), set up a stub child via clone,
  resume user-mode, on SIGSYS post `SYSCALL_REQ` to spawner,
  block on `SYSCALL_REP`, write the reply into the stub child's
  registers, resume.
- Worker has NO `task_struct`, NO `mm_struct`. Only the
  spawner's UML kernel runs handle_syscall, schedule, etc.
- The pthreads inside the worker are host pthreads (pthread_t),
  not UML task_structs. They exist to give each guest task a
  kernel stack to run on inside the worker process; they don't
  participate in UML's scheduling.

This makes E.3 conceptually simpler: the worker is a thin
trampoline. The bulk of R4's complexity is in the spawner-side
IPC dispatcher (one thread per worker, blocking recvmsg, dispatch
to UML kernel) and in the cross-mm migration handler (E.5).

---

## Part J — Status (2026-04-28 end of session)

Memo 28 (this document) is the design lock. Implementation
status:

- E.1 scaffolding (`4eb34edab3f7`) — Kconfig + headers + struct
  field. Done.
- E.2 spawner skeleton (`9547b9c40c31`) — boot init, list +
  lock, panic notifier. Done.
- E.3a worker spawn/reap machinery (`23b4de4380a3`) — USER TU
  arch/um/os-Linux/worker_user.c with clone-without-CLONE_VM,
  socketpair, echo-only main loop. Wired into
  spawner.c::spawn_worker_for_mm. Done; reachable but not yet
  invoked from any production path.
- E.3b worker stub-child manager (`fcf4f00d3f3e`) — tagged-message
  dispatcher in worker_main, 4 new IPC types (STUB_ALLOC_REQ,
  WRITE_REGS, RETURN_VALUE, WRITE_REGS_ACK) per Part K.6 minimum-
  viable cut, worker_smoke_test() entry point, worker_send_msg_for_mm
  helper for E.3d. Option A chosen: handlers stash payloads in
  worker-local state (mm_id snapshot + 9-slot regs buffer); actual
  start_userspace integration deferred to E.3c. Done. Build clean
  at WORKER_PROCESS=n and =y; substrate gate stable PASS=22 FAIL=3
  EXPECTED_FAIL=3.
- **E.3c** (spawner-side per-worker dispatcher thread) — pending.
  One kernel thread per worker that receives SYSCALL_REQ over
  IPC, invokes handle_syscall in UML kernel context, sends
  SYSCALL_REP. ~100 LoC.
- **E.3d** (seccomp integration) — pending. seccomp_mm_create
  takes spawn_worker_for_mm under WORKER_PROCESS=y;
  seccomp_vcpu_run dispatches via per-mm worker IPC instead of
  the in-spawner SIGSYS handler. ~100 LoC.
- **E.4** (per-task pthread inside worker) — pending. ~150 LoC.
- **E.5** (cross-mm migration via SIGUSR2) — pending. ~200 LoC.
- **E.6** (defconfig flip) — pending. ~50 LoC.

Total remaining: ~750 LoC across 6 commits. Best executed as a
focused multi-session effort with the design lock above
(especially Part I.5 on kernel-state ownership) as the spec.

Next session opens this memo, starts with E.1 (scaffolding), and
proceeds linearly through E.6. Verification gates after every
commit. Subagent escalation when stuck on the IPC protocol or the
SIGUSR2 migration race (per memo 27 H.3 / Level 2).

When R4 ships, R6 (signal handling) lands as a follow-up cleanup
(per memo 25 R6, mostly already addressed by R4's
CLONE_SIGHAND-inside-worker design — what remains is documenting
the contract).

---

## Part K — E.3b implementation guide (Explore subagent 2026-04-28)

A focused subagent surface-mapped E.3b before any code was
written. Findings:

### K.1 — `start_userspace` is ~95% process-agnostic

The function (`arch/um/os-Linux/skas/process.c::336-436`) breaks
down into three classes of step:

| Step | Class |
|---|---|
| mmap stack, socketpair, init_data setup | A. process-agnostic |
| `clone(userspace_tramp, ..., CLONE_VFORK | CLONE_VM | SIGCHLD)` | B. spawner-only assumption (the parent of the resulting stub child) |
| `wait_stub_done_seccomp`, munmap stack, socketpair retain/close | A. process-agnostic |

`userspace_tramp` itself reads `um_backend->stub_child_runs_seccomp`
and uses globals (`stub_exe_fd`); these are inherited CoW into the
worker, so from the worker's perspective they Just Work. The only
hard "spawner-only" piece is the clone() call site itself —
**make the worker call `clone(userspace_tramp, ...)` and the rest
of the machinery follows.**

### K.2 — `mm_id` ownership: Option γ (pass via IPC) is the minimum-churn path

Three options were considered (memo 28 Part B.3 already mentions
Options α/β/γ; the subagent re-examined and recommends γ):

- **γ (pass over IPC)**: spawner allocates `struct mm_id` (already
  done at `init_new_context()`). Spawner sends a new
  `WORKER_MSG_STUB_ALLOC_REQ` over IPC carrying the mm_id fields
  (stack VA, sock fd, etc.). Worker populates a local
  `struct mm_id` from the message and passes it to a worker-side
  `start_userspace(&worker_local_mm_id)`. **No struct refactor;
  no API change.** Just a new IPC message type.

`struct mm_id`'s post-R2 fields are already context-agnostic (no
kernel pointers; just stack VA + a few FDs); marshalling them
across IPC is straightforward.

### K.3 — `wait_stub_done_seccomp` + futex round-trip are worker-compatible

The futex-and-FD-passing primitives in
`arch/um/os-Linux/skas/process.c::42-139` reference only
`mm_idp->{stack,sock,syscall_fd_num,pid}` and use libc-level
syscalls. **No `um_kernel_*` references; no kernel-side state.**
Worker reuses verbatim once mm_id is populated.

### K.4 — Signal routing is simpler than expected

The stub child's SIGSYS is **handled inside the stub binary's
own filter** (`stub_signal_interrupt` in `arch/um/kernel/skas/stub.c`),
not by the parent. The handler writes regs to `stub_data` and
`futex_wake`s the parent. After R4 the parent is the worker;
the worker just observes a futex wakeup and forwards via IPC.
**No SIGSYS handler in the worker itself — only SIGCHLD (for
stub-child crash) and SIGTERM (for clean shutdown, already in
worker_main).**

### K.5 — `handle_syscall` body is regs-only; CALLEES are not (corrected 2026-04-28)

**Original claim (kept here as a record of the misread):** the
spawner-side dispatcher kthread (E.3c) can call handle_syscall
directly with regs from `SYSCALL_REQ` because handle_syscall takes
a `uml_pt_regs *` and never touches `current`.

**Correction (E.3d surface map, 2026-04-28).** The literal body
of `handle_syscall` (`arch/um/kernel/skas/syscall.c:19-102`) does
not dereference `current`. But every meaningful callee does:

- `secure_computing()` reads `current->seccomp.mode` and walks
  the per-task filter list. On a kthread, mode is 0 → no filter
  enforcement. Wrong policy.
- `syscall_trace_enter` (`arch/um/kernel/ptrace.c:124-138`) reads
  `current_thread_info()->flags` and `current->ptrace` — the
  kthread's flags are not the guest task's.
- Every `sys_call_table[]` entry derefs `current` for
  credentials, files, fs, signals, pid namespaces. `sys_read`
  uses `current->files`; `sys_open` uses `current->fs`;
  `sys_clone` copies from `current`; `sys_kill` uses
  `current->signal`.

**Consequence for E.3c:** the dispatcher kthread as-shipped
(`arch/um/kernel/spawner.c:137-212`) is a smoke-test scaffold
only. It can drive WORKER_MSG_SYSCALL_REQ through the literal
handle_syscall body — but the answer it produces will be
wrong-vs-the-originating-guest-task for any real syscall.

**E.3d must therefore choose a `current` discipline** before
real SYSCALL_REQ traffic flows. See Part L for the three options
on the table.

### K.6 — E.3b minimum viable cut

The smallest delta that proves the worker can drive a stub child
end-to-end (without involving real syscall execution):

```
1. spawner.spawn_worker_for_mm() — already done (E.2/E.3a).
2. spawner sends WORKER_MSG_STUB_ALLOC_REQ with mm_id snapshot.
3. worker receives, populates local mm_id, calls start_userspace.
   stub child cloned inside the worker; futex-ready.
4. spawner sends WORKER_MSG_WRITE_REGS with a synthetic regs frame.
   worker calls set_stub_state(...); futex-wakes stub child.
5. stub child runs (no actual syscall yet — just resume).
6. spawner sends WORKER_MSG_RETURN_VALUE with a known sentinel
   (e.g. 0x1234). worker calls get_stub_state(...); regs->ax sentinel.
7. worker echoes regs back via WORKER_MSG_WRITE_REGS_ACK.
   spawner verifies round-trip.
```

This proves: mm_id IPC marshalling, futex round-trip, regs
ping-pong, signal mask in worker. Skips: actual SIGSYS (E.3c),
handle_syscall integration (E.3c), seccomp_mm_create rewiring
(E.3d).

Code delta:
- `arch/um/include/shared/worker_ipc.h`: add three message types
  (STUB_ALLOC_REQ, WRITE_REGS, RETURN_VALUE).
- `arch/um/os-Linux/worker_user.c`: replace echo loop in
  `worker_main` with a tagged-message dispatcher (~80 LoC).
- `arch/um/kernel/spawner.c`: add a smoke-test path or wire from
  seccomp_mm_create (gated by WORKER_PROCESS=y) (~50 LoC).

### K.7 — Risk register for E.3b

| Risk | Impact | Mitigation |
|---|---|---|
| Signal mask drift (worker inherits mask from spawner at clone, then mask drifts) | Worker may miss SIGCHLD or block when not desired | Worker explicitly `sigprocmask` after clone to a known-good mask. |
| Stale FDs in worker (CoW FD table inherits all spawner FDs) | Stub binary picks up unintended fds | `userspace_tramp`'s existing `close_range(0, ~0U, CLOSE_RANGE_CLOEXEC)` already handles this; worker reuses unchanged. |
| Stub binary path lookup | Stub child can't find binary | `stub_exe_fd` is inherited via CoW; `execveat(stub_exe_fd, ...)` works without pathname lookup. |
| PR_SET_PDEATHSIG ordering | Worker dies, stub child orphaned | Worker sets PDEATHSIG=SIGTERM before clone; stub child inherits and reaps cleanly on worker death. **Already done in E.3a.** |
| Single-threaded worker stalls | Worker blocks IPC while stub child runs | Accept for E.3b (proof-of-concept). E.3c adds a per-worker dispatcher thread on the spawner side. |

### K.8 — What this report does NOT answer (design decisions deferred)

- E.3c's dispatcher thread management: thread-pool vs per-worker
  thread. Memo 28 Part C.D locks "one thread per worker"; the
  subagent agreed but didn't surface fresh detail.
- Cross-mm migration timing (E.5): the subagent confirmed
  SIGUSR2 won't collide with PM-wake (which uses SIGUSR1).
  Migration design otherwise unchanged from memo 28 Part C.C.
- E.4's per-task pthread inside the worker: how does
  `seccomp_thread_create` interact with the stub-child setup?
  **Needs design decision in E.4.**

The subagent's findings are sufficient to start E.3b in a future
session; no further surface mapping needed before code.

---

## Part L — E.3d sequencing decision (added 2026-04-28 after E.3c)

After E.3c landed, an Explore subagent surface-mapped E.3d. Two
structural gaps surfaced that make the original ~100 LoC estimate
non-viable:

### L.1 — Two missing pieces between today and "real worker SYSCALL_REQ"

1. **In-worker `start_userspace`.** Today's seccomp backend calls
   `start_userspace(&mm->context.id)`
   (`arch/um/backend/seccomp/mm.c:30`) which clones the stub child
   *into the spawner's VA via CLONE_VM*
   (`arch/um/os-Linux/skas/process.c:386-388`). For a per-mm
   worker model, the worker must call its own
   `start_userspace`-equivalent inside its own VA. E.3b's
   STUB_ALLOC_REQ handler today only stashes the mm_id snapshot;
   nothing actually clones a stub child inside the worker. Until
   that lands, the worker has no way to generate real
   SYSCALL_REQs.

2. **`current` discipline (Part K.5 correction).** E.3c's
   dispatcher kthread calling `handle_syscall(&regs.regs)` is
   safe only for the smoke-test sentinel — sys_call_table[]
   entries dereference `current` for credentials, files, fs,
   signals, etc. Routing real syscalls through the dispatcher
   kthread without an impersonation mechanism would silently
   produce wrong answers (kthread's task_struct, not the guest
   task's).

### L.2 — Three options for the `current` discipline

#### Option L.2.A — Wait-queue bounce (RECOMMENDED)

Dispatcher kthread receives SYSCALL_REQ → routes (regs,
completion) to the originating guest task's wait queue → the
guest task itself wakes, runs `handle_syscall` on its own stack
under its real `current`, signals completion → kthread sends
SYSCALL_REP back to the worker.

- **Correctness**: matches today's invariant exactly. handle_syscall
  always runs in the originating task's context.
- **Cost**: one extra wakeup per syscall (~hundreds of ns).
  Acceptable under memo 26 Phase H's ≤1.2× seccomp wall-clock
  budget.
- **Compatibility**: matches Part I.5's "spawner-owns-everything,
  gVisor sentry pattern" — the spawner-side guest task is the
  source of truth.

#### Option L.2.B — Per-mm guest-task task_struct

Allocate one "guest task" task_struct per mm; dispatcher
`set_current()` to it before calling handle_syscall.

- **Correctness**: requires cloning credentials/files/fs from
  the originating task at every dispatch — expensive and
  error-prone.
- **Cost**: heavyweight allocation per mm; still doesn't help
  for SMP-within-mm.
- **Verdict**: more code, less correct than L.2.A.

#### Option L.2.C — `kthread_use_mm` only

Set the dispatcher's `->mm` to the guest mm via
`kthread_use_mm`.

- **Correctness**: addresses `current->mm` only. Doesn't fix
  `current->files`, `->seccomp`, `->signal`, etc.
- **Verdict**: insufficient.

### L.3 — Proposed E.3d split

Given (L.1) and (L.2), E.3d as a single ~100 LoC commit is not
viable. Proposed three-commit split:

#### E.3d.0 — In-worker start_userspace (DONE — `0075c0820da9`)

Brings up a real stub child inside the worker process. Extends
the STUB_ALLOC_REQ handler in `worker_user.c` to call
`start_userspace(&local_mm_id)`, wires fd-passing back to the
spawner via SCM_RIGHTS so the spawner-side mm_id has a usable
`.sock`, gates `seccomp_mm_create` on WORKER_PROCESS=y to call
`spawn_worker_for_mm` + STUB_ALLOC_REQ instead of local
`start_userspace`. vcpu_run unchanged.

Bug fix in passing: `spawn_worker_process` was passing the
address of a spawner-stack-local `struct worker_init` to
`clone()`. Under UML's task-stack reuse the worker observed
`worker_socket_fd` zeroed by the time it dereferenced. Switched
to placing the init payload at the bottom of the worker's own
mmap'd stack page; the worker's `arg` then points into a page
only the worker writes.

Build clean both UM_WORKER_PROCESS=n and =y. Substrate gate
holds at PASS=25 FAIL=3 EXPECTED_FAIL=3 under =n.

Boot smoke under =y:
- init=/bin/true: 3/3 runs print "spawned worker pid=N",
  "stub alloc OK for mm=M (worker_pid=N stub_pid=N+1)" then
  panic on init-exit (panic=-1, expected).
- init=/usr/bin/python3: 3/3 runs reach "stub alloc OK" then
  hit "UML: fatal signal; exiting" mid-vcpu_run. STRUCTURAL,
  expected: vcpu_run's set_stub_state writes through `id->stack`,
  but that VA now lives in the worker's address space; the
  spawner dereferences a wild pointer. E.3d.1 + E.3d.2 fix.

#### E.3d.1 — `current` discipline lock + dispatcher rewrite (DONE — `f56c208c374b`)

Implements Part C.E wait-queue bounce. Dispatcher kthread is now
routing-only: per-mm wait queue holds `worker_pending_req`
entries; the originating guest task (any caller with a
mm-bearing task_struct) drains via `worker_run_pending_syscalls`
and runs handle_syscall under its own `current`.

`struct um_worker` grew `wait_queue_head_t reply_wait`,
`struct list_head pending_reqs`, `spinlock_t pending_lock`. Lock-
ordering rule documented at struct definition: `pending_lock` is
INNERMOST, never nested under `workers_lock`. ABBA-clean by audit.

No production caller exercises this code yet — E.3d.2 below is
the first one. Verification gate for E.3d.1 was build clean both
modes + WORKER_PROCESS=n substrate gate stays at PASS=25 FAIL=3
EXPECTED_FAIL=3 + boot under =y init=/bin/true unaffected. All
green.

#### E.3d.2 — vcpu_run rerouting through worker IPC (~250 LoC)

Replace `seccomp_vcpu_run`'s direct
`set_stub_state`/`wait_stub_done_seccomp`/`get_stub_state`
(`trap_user.c:68,82,97`) with: send WRITE_REGS to worker →
worker does set_stub_state + wait_stub_done_seccomp +
get_stub_state in-VA → worker sends SYSCALL_REQ → spawner-side
guest task (per E.3d.1) runs handle_syscall, builds reply →
worker resumes via SYSCALL_REP.

Risk: turnstile semantics (`enter_turnstile`/`exit_turnstile` at
`trap_user.c:51,121`) move into the worker; SCM_RIGHTS passing
of syscall stub fds; SIGSEGV/SIGTRAP/SIGALRM/SIGIO branches all
relay through the same channel.

Verification: real userspace runs; substrate gate flips to
match WORKER_PROCESS=n baseline (or better — itimer_virtual
EXPECTED_FAIL → PASS per memo 29 §2.5.5).

### L.4 — Resolution (2026-04-28)

User direction: "continue, never give up, do not stop until
done." Combined with the recommendation above, this resolves the
three blocking questions:

1. **Three-commit E.3d split: APPROVED.** Memo 28's original
   ~100 LoC estimate retired; new R4 budget is ~1000 LoC across
   six commits (E.3d.0 + E.3d.1 + E.3d.2 + E.4 + E.5 + E.6).

2. **Option L.2.A (wait-queue bounce): LOCKED** as new Part C.E.
   Dispatcher kthread becomes routing-only; originating guest
   task runs handle_syscall under its real `current`.

3. **E.3d.0 lands standalone first.** Clean bisection point;
   doesn't touch the dispatcher; only brings up the in-worker
   stub child + wires seccomp_mm_create on WORKER_PROCESS=y.

Proceeding with E.3d.0.
