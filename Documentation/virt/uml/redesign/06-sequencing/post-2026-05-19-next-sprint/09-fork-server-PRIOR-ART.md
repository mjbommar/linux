# 09 — Fork-server prior-art audit (2026-05-20)

**Author:** research session 2026-05-20 (post-bisect-attempt)
**Scope:** answer the seven specific questions raised after the
previous session's seven-variant bisect failed; cite file:line for
every claim; recommend the next falsifiable experiment.

**TL;DR (paragraph form, expanded in §5):**

The previous session conflated two distinct hazards (stub-pid
aliasing → fixed; UML_LONGJMP-into-stale-jmp_buf → unfixed) and
then declared the second "architectural / multi-week."  It is not.
The AFL forkserver path that WORKS in tree
(`arch/um/kernel/snapshot.c`) avoids the second hazard via TWO load-
bearing invariants that template_pause violates: (a) the parent
**never returns up the syscall stack** — it lives forever in
`um_snapshot_forkserver_loop`'s blocking-`read()` on fd 198, while
template_pause's parent re-enters guest-kernel scheduling state
between iterations via `one_pause_cycle()` → unwind through
`interrupt_end()`; and (b) `um_snapshot_assert_ready()` enforces
**four** preconditions at ready-point entry, ALL of which
`assert_fork_safety()` skips — most critically `um_get_signals() ==
1`.  Without that check the `os_snapshot_block_iter_signals()` call
inside the loop body is a silent no-op, the entire D41 gating
contract collapses, and the master's SIGALRM-tick eventually
schedules into a stale jmp_buf.  The single concrete next step
(§5) is to (i) port the four AFL preconditions verbatim, then
(ii) restructure `fork_on_resume_loop` so the **parent** never
returns up the call stack between iterations — it must sit in a
blocking host primitive (matching AFL's `read(fd198)` shape) and
let the supervisor drive iteration timing.  Falsifiable prediction:
with both changes the master's `um_template_pause_enter+0xf0`
epilogue crash disappears because the epilogue is unreachable in
the happy path.

---

## 1. Inventory of in-tree precedent

### 1.1 What works

| Mechanism | File | What it actually does | Verified |
|-----------|------|------------------------|----------|
| AFL forkserver loop | `arch/um/kernel/snapshot.c:231–429` | Parent blocks in `os_snapshot_read_all(fd 198)`, forks on each cmd byte, worker exits or runs trivial guest code | Smoke selftest `tools/testing/selftests/um/snapshot-smoke/` passes; commit message of `b890c0a706e2` records "PASS state_version=1 ready=present" + "PASS pid=N status=0x0" |
| Worker `um_snapshot_worker_init()` | `arch/um/kernel/snapshot.c:493–615` | Disables `um_snapshot_enabled` static key; `os_sigio_worker_forget` + `os_timer_worker_forget`; `sched_worker_detach_other_tasks`; `os_sigio_worker_rebuild` + `os_timer_worker_rebuild` | C-09 commit 3d-c demonstrated forked worker runs `/bin/echo` and `/bin/true` |
| Strict ready-point assertions | `arch/um/kernel/snapshot.c:142–198` | Refuses entry from IRQ/softirq, with pending signals, on SMP, with UML signals already gated, or under KVM backend | D37 pull-forward #2 |
| UML-native signal gating | `arch/um/os-Linux/process.c:245–273` | `um_set_signals(0)` → `signals_enabled` TLS flag = 0; UML's `sig_handler` / `timer_alarm_handler` see this and queue into `signals_pending` instead of dispatching | D41 codifies this as the canonical primitive |
| `start_userspace_redo()` | `arch/um/os-Linux/skas/process.c:572–579` | `os_skas_reap_stub()` + `start_userspace()` — kill+wait4+zero stub_data then reclone | Memo 09 Phase 2a Patch 1 (`0993765a704c`) |
| Stub teardown / respawn helpers | `arch/um/kernel/skas/mmu.c:292–413` | `um_skas_teardown_all_stubs` / `um_skas_respawn_all_stubs` / `um_skas_forget_all_stubs` / `um_skas_other_mm_mid_syscall` | Memo 09 Phase 2a Patches 2 + 4 |

### 1.2 What is known-broken

| Mechanism | File | Failure mode | Reference |
|-----------|------|--------------|-----------|
| Blocking `wait4` in parent's reap path | `arch/um/os-Linux/process.c:169–205` (`os_snapshot_waitpid_status`); also `arch/um/os-Linux/skas/process.c:517–535` (`os_skas_reap_stub`'s wait4 loop) | Crashes parent with `Kernel mode signal 4` (SIGILL) or with `0x0` / `0x61093bc0` jmp IPs | `04-risks/signal-reentry-in-fork-window.md` lines 19–26 + 105–125; D41 attempt-matrix |
| `sched_worker_detach_other_tasks()` called PRE-FORK in the master | `kernel/sched/core.c:2266–2288` | Crashes the master immediately — detaches init.sh, kworkers, ksoftirqd from `rq->cfs_tasks`, then `__set_next_task_fair` trips on near-empty rq | Bisect note in `767cef61264d`'s commit message |
| Template-pause `fork_on_resume_loop` | `arch/um/kernel/template_pause.c:258–369` (current tree) | Master panics at `um_template_pause_enter+0xf0` (the function epilogue's `ret`) with corrupted saved-RIP after the second `one_pause_cycle`'s SIGSTOP/SIGCONT | `09-fork-server-STATUS.md` lines 219–251 |
| AFL parent reaping per-iter worker status synchronously | reverted in `f8bb690f91b8`; design ceiling captured in snapshot.c lines 366–397 | Same SIGILL hazard as above; v1 ceiling documented honestly as "status byte hard-coded to 0" | Four failed attempts logged in `04-risks/signal-reentry-in-fork-window.md` lines 105–125 |
| Worker pr_info immediately post-fork | observed in snapshot.c 3c/3d-c bring-up; led to comment in snapshot.c lines 482–487 ("Deliberately no pr_info here") | Trips vsnprintf via per-CPU/TLS lookups in the forked child | Commit `9d0dd8ed3181` bring-up |

The current template_pause.c child path violates the worker-pr_info
rule: lines 329 (`pr_err`) and 334 (`um_snapshot_worker_init` which
internally has pr_err on failure) can fire in the child.  Per
snapshot.c's experience these are an additional fork-inheritance
hazard if the rebuild fails.

---

## 2. Decisions-log archaeology — what prior rounds decided about fork hazards

| Decision | Date | Bottom line |
|----------|------|-------------|
| **D35** (`decisions-log.md:2360`) | 2026-03 | C-09 v1 scope is "cooperative AFL-style forkserver"; CRIU snapshot-to-disk explicitly deferred to v2.  v1 takes the ready-point invariant of "no userspace tasks, no stubs, runqueue has only kthreads + caller." |
| **D36** (`decisions-log.md:2546`) | 2026-03 | v2 snapshot file format is ELF64 + UML PT_NOTE.  Out of scope here; flagged because Memo 09 §3 Phase 3+ may want it. |
| **D37** (`decisions-log.md:2716`) | 2026-04 | Five v1 pull-forward items, of which #2 = ready-point preconditions (`signal_pending`, `num_online_cpus`, etc.) and #6 = KASAN MADV_DONTFORK skip when `CONFIG_UM_FUZZ_HOOKS` is on (`arch/um/os-Linux/mem.c:64–76`). |
| **D38** (`decisions-log.md:2853`) | 2026-04 | v2 has crash-consistent vs application-consistent capture modes; v1 is Mode B by construction. |
| **D39** (`decisions-log.md:3012`) | 2026-04 | C-09 commit 3 split into 3a/3b/3c/3d (worker-side reinit risk tranching). |
| **D41** (`decisions-log.md:3240`) | 2026-04 | **UML `signals_enabled` is THE canonical signal-gating primitive for any forkserver critical section.**  Raw `sigprocmask` is the wrong layer — its unblock fires queued signals atomically in the same hazardous window.  `signals_enabled` must be **1** at ready-point entry (enforced by `um_snapshot_assert_ready` line 161–164) so that the loop body's `block_iter_signals` call actually takes effect (otherwise it's a no-op). |
| **D42** (`decisions-log.md:3411`) | 2026-04 | `sched_worker_detach_other_tasks()` is necessary but not sufficient for worker-side rebuild.  v2 replaces with freezer-cgroup pre-fork barrier.  v1 = the narrow helper. |
| **D43** (`decisions-log.md:3637`) | 2026-04 | C-06 BPF JIT v1 blocked on three cross-subsystem touches.  Not directly relevant but cited because it's the same sprint's "blocked on cross-subsystem" pattern as D41/D42's freezer-cgroup deferral. |

The hazard memo `04-risks/signal-reentry-in-fork-window.md`
(2026-04-23) characterises the **SIGALRM-into-stale-jmp_buf**
failure mode in mechanical detail (lines 28–74) and lists the four
**Option A/B/C/D** fix shapes (lines 145–246).  Option **A** ("worker
reports its own status pre-exit, parent never blocking-waits in the
critical window") is the recommended medium-term fix.  Option **B**
(dedicated post-fork wait-thread) is rejected as adding more
pthread surface to a zone where pthreads are the bug.  Option
**C** (validate jmp_buf stack pointer in `switch_threads`) is
rejected as 4+ sessions and adds infrastructure that doesn't exist
today.  Option **D** (do nothing, v1 ceiling) is what is in tree
for AFL.

**The previous session never engaged with Option A.**  Their
PHASE2A-DESIGN.md §3 has no mention of the
signal-reentry-in-fork-window memo, no mention of `D41`'s
`signals_enabled == 1` entry contract, and no consideration of
having the master sit permanently in a blocking host primitive (the
AFL pattern).  Both omissions are the load-bearing gap captured in
§4 below.

---

## 3. The seven failure-variants — were they predicted by earlier decisions?

From `09-fork-server-STATUS.md` lines 220–250.  For each, the
prior-art record's verdict.

| # | Variant | Predicted broken by? | Why |
|---|---------|----------------------|-----|
| 1 | Teardown + respawn (PHASE2A-DESIGN §3) | Partially.  D41 + the signal-reentry memo predict that respawn's `start_userspace` → `wait_stub_done_seccomp` host blocking syscall WILL re-enter the SIGALRM-in-stale-jmp_buf hazard if signals_enabled isn't held at 0 from the right entry state. | Master's blocking futex in `wait_stub_done_seccomp` (`arch/um/os-Linux/skas/process.c:143–145`) is the same hazard class as the snapshot path's blocking wait4 (signal-reentry memo §"Call-site inventory" line 137). |
| 2 | No teardown | Yes.  PHASE2A-DESIGN §1.11 itself predicts this (stub-pid aliasing).  Bisect confirmed. | The stale-jmp_buf hazard is independent of stubs; deactivating teardown only addresses the master's stub aliasing, not the unwind problem. |
| 3 | Pre-fork `sched_worker_detach_other_tasks` | Yes.  `D42` line 3556–3560 explicitly states the helper "operates on this_rq() under rq_lock_irqsave" in the **worker's** copy; calling it in the **master** would detach init.sh / kworkers / ksoftirqd from the master's runqueue, with results not predicted by D42.  The bisect's "master crashes immediately" is the predicted outcome. | D42 line 3470 has the helper docstring: "called only from arch/um/kernel/snapshot.c's um_snapshot_worker_init(), under that worker's signals_enabled == 0 guard."  Pre-fork master use violates both halves. |
| 4 | Child-side detach + respawn (master keeps stubs) | Partial.  D41 predicts that the master, after fork, has live signal queues that drain when signals_enabled flips back to 1.  Without respawn the master is still exposed to the same SIGALRM-in-stale-jmp_buf hazard. | Master's stale jmp_buf hazard is orthogonal to stub aliasing. |
| 5 | Inline-asm syscalls everywhere | Bypasses **one** hazard (glibc cancellation-pipe in the forked child — see commit `c0a7ac3f905c`); does **not** bypass the SIGALRM-into-stale-jmp_buf hazard. | The signal-reentry memo line 96–103 explicitly says raw syscall doesn't help: the unblock step itself fires queued signals atomically. |
| 6 | `preempt_disable` across fork | Yes.  D41 §"Why the existing signals_enabled TLS guard doesn't help" predicts this won't help — `preempt_disable` is the wrong level (CFS preemption, not host signal delivery). | The same SIGALRM via `timer_alarm_handler` → `unblock_signals_trace()` → `do_IRQ` → `schedule()` path runs whether or not preempt_count is non-zero, because UML's `unblock_signals` calls `sig_handler_common(SIGIO, …)` directly (signal.c:399–410). |
| 7 | Early-pause via `late_initcall_sync` | Partial.  The "no userspace, no stubs" condition matches AFL's invariant.  But early-pause STILL crashes with `Segfault with no mm` — and that's predictable: at late_initcall_sync time `current == &init_task` (process.c:48 sets `cpu_tasks[0] = &init_task`); a SEGV in this path hits `trap.c:366–368` panic. | Early-pause was a structural test of "match AFL's ready-point invariant," but template_pause's loop body STILL violates D41's `signals_enabled==1`-at-entry contract.  Same root cause, different surface symptom. |

The bisect was directionally correct (it ruled out a lot of dead
ends) but the design memo never engaged with the four prior
mitigations (D41 entry assertions; Option A worker-side status; AFL
parent's "permanently blocked in `read(fd198)`" shape; the
signal-reentry memo's full call-site inventory).  The bisect ran
seven variants of the SAME mis-engineered scaffolding.

---

## 4. The seven questions — answered with file:line citations

### 4.1 What EXACTLY does `um_snapshot_worker_init()` do that we are NOT doing in `template_pause.c`?

In order, lines `arch/um/kernel/snapshot.c:533–615`:

1. **Line 548** — `static_branch_disable(&um_snapshot_enabled);` —
   flip the snapshot hot-path static key OFF in the worker.
   Template_pause has no equivalent because it doesn't gate
   anything via a static key.  Not applicable.
2. **Line 556** — `os_sigio_worker_forget();` — drop the parent's
   SIGIO helper-thread pthread_t + epoll fd (stale tids/fds in the
   worker).  Template_pause's child calls this via
   `um_snapshot_worker_init()` at line 334.
3. **Line 557** — `os_timer_worker_forget();` — drop the parent's
   per-CPU POSIX timers (their SIGEV_THREAD_ID tids don't exist in
   the worker).  Same — called via `um_snapshot_worker_init()`.
4. **Line 571** — `sched_worker_detach_other_tasks();` — detach all
   non-`current` CFS tasks from `rq->cfs_tasks`.  Same.
5. **Lines 586–592** — `os_sigio_worker_rebuild()` +
   `os_timer_worker_rebuild()` — recreate the SIGIO helper pthread
   and the POSIX timers in the worker's own address space.  Same.
6. **Explicit non-actions** (lines 519–526): no stub respawn here.
   "Seccomp stub children: no stubs exist at ready-point because no
   guest userspace task has run yet."  Template_pause violates this
   invariant by running AFTER init.sh has executed.

**What template_pause does that worker_init does NOT do**:
1. Calls `preempt_disable()` (`template_pause.c:322`) — added in
   commit `767cef61264d` as a defensive measure that the bisect
   found doesn't help (D41 predicts this).
2. Calls `sched_worker_detach_other_tasks()` BEFORE
   `um_snapshot_worker_init()`'s own call to the same helper
   (`template_pause.c:326`).  Idempotent (second call finds no
   tasks to detach), but the call site shows the previous session
   thought ordering mattered.
3. Calls `um_skas_respawn_all_stubs()` (`template_pause.c:327`) —
   the eager respawn that snapshot.c explicitly defers (the
   "first userspace syscall lazily spawns one via start_userspace"
   model, snapshot.c:520–522).
4. Re-enables signals via the return path (unwinding the
   `os_snapshot_block_iter_signals()` save) — snapshot.c's worker
   path keeps `signals_enabled == 0` because the worker is going
   to either `exit_group` or carefully manage its own re-arm.

**What worker_init does that template_pause SKIPS**:
- The "do not pr_info / pr_err in the worker" rule (snapshot.c
  lines 482–487, 607–614).  Template_pause's child has BOTH
  `pr_err` calls at lines 329 and (transitively) inside
  `um_snapshot_worker_init` if its sub-helpers fail.  Per
  snapshot.c the worker's per-CPU/TLS state is unsafe for
  vsnprintf.

### 4.2 At what specific point does the AFL forkserver fork relative to UML's task scheduler state?

`um_snapshot_ready()` is invoked from `arch/um/kernel/snapshot.c:
628–646` (the debugfs trigger) after the user echoes a name into
`/sys/kernel/debug/um/snapshot_ready`.  In the fuzz profile the
caller is the bootstrap script run by init, **with init.sh as the
calling task**.  So at fork time:

- `current` = init.sh's task_struct
- `mm_list` MAY have init.sh's mm
- The runqueue has init.sh (running) + any kthreads (ksoftirqd,
  kworker, etc.)

This is identical to template_pause's late-pause case in every
material respect.  The "v1 ceiling" applies — `um_snapshot_assert_
ready` checks `signal_pending(current)` (snapshot.c:151) which
implicitly assumes "no pending init-time signals," and `num_online
_cpus() > 1` (snapshot.c:156) which refuses SMP.

The EARLY-pause variant the previous session tried
(`template_pause.c:495–504`, `late_initcall_sync`) does match the
"no stubs, no userspace tasks" invariant.  But it ALSO crashes,
because the D41 contract violation (no `signals_enabled == 1` entry
assertion) is independent of the stub state — see §4.3.

### 4.3 Does template_pause's `assert_fork_safety` check the SAME conditions as `um_snapshot_assert_ready`?

**No.** Side-by-side:

| Check | snapshot.c | template_pause.c | Comment |
|-------|------------|------------------|---------|
| Not in IRQ/softirq context | line 146 (`in_hardirq() \|\| in_softirq()`) | **missing** | Cheap. |
| No pending non-SIGCHLD signals (`signal_pending(current)`) | line 151 | **missing** | Critical — see below. |
| `num_online_cpus() == 1` | line 156 | **missing** | Template_pause smoke runs `ncpus=1` (selftest line 101) but the kernel assertion is what enforces it. |
| `um_get_signals() == 1` at entry | line 161 | **missing** | Load-bearing per D41.  If signals_enabled is already 0 at entry, `os_snapshot_block_iter_signals()` at line 274 is a SILENT NO-OP and the entire loop body's signal-gating contract collapses. |
| KVM backend refusal | line 191 | line 213 | Present in both. |
| Mid-syscall mm refusal | (none) | line 228 | Template_pause adds this defensively. |

**The missing `um_get_signals() == 1` check is load-bearing.**  D41
documents this contract in `decisions-log.md:3349–3356`:

> `um_snapshot_assert_ready()` gains a check that `signals_enabled
> == 1` at ready-point entry.  This documents that the ready-point
> contract expects a fully operational UML signal state before we
> start quiescing.  If a future caller enters `um_snapshot_ready()`
> with signals already gated, a WARN_ONCE fires and the ready-point
> is refused, preventing a subtle re-entry where our `block_iter`
> call is a no-op and the critical section is already "open" from
> someone else's perspective.

In the early-pause variant in particular, `late_initcall_sync` runs
while the kernel's signal subsystem is still in flux.  At that
point `signals_enabled` may legitimately be 0 (it gets set to 1 by
`start_kernel_proc` via `block_signals_trace` — see
`arch/um/kernel/skas/process.c:25`).  Without the entry check we
silently no-op the entire loop body's gating.

### 4.4 What does `sched_worker_detach_other_tasks()` actually do, and why does it CRASH the master pre-fork?

Source: `kernel/sched/core.c:2266–2288`.  Pseudocode:

```c
rq = this_rq();
rq_lock_irqsave(rq, &rf);
update_rq_clock(rq);
list_for_each_entry_safe(se, tmp, &rq->cfs_tasks, group_node) {
    p = task_of(se);
    if (p == current) continue;
    if (!task_on_rq_queued(p)) continue;
    deactivate_task(rq, p, DEQUEUE_NOCLOCK);
}
rq_unlock_irqrestore(rq, &rf);
```

`deactivate_task` (`kernel/sched/core.c:2211` per D42's
cross-reference) calls `__dequeue_task` + clears p->on_rq.  Task
remains alive (no `do_exit`), just unable to be scheduled.

**Why it works in the AFL worker**: per D42 lines 3470–3489, the
contract is "called in the WORKER, which is a forked-child UML
process whose `rq->cfs_tasks` references parent task_structs whose
`thread.switch_buf` jmp_bufs point at parent host-thread state."
Detaching them stops `__set_next_task_fair` from picking them.

**Why it CRASHES the master pre-fork** (bisect result, commit
`767cef61264d`): in the master, the runqueue contains
**legitimately scheduling-needed kthreads** (ksoftirqd, kworker,
migration, RCU softirq tail).  Detaching them prevents the master
from doing routine kernel work.  When the next softirq raises (e.g.
the SIGALRM tick), the only candidate is `current` (init.sh).  But
init.sh is mid-syscall (in proc_write), and `__set_next_task_fair`
WALKS `rq->cfs_tasks` looking for a sched_entity to dequeue — with
the list now containing only init.sh's stale entry (or an
inconsistent state if the helper raced with a concurrent enqueue),
the walk dereferences a freed or torn-down entry and panics.

The crash is **expected** given D42's contract; the previous
session called the helper pre-fork in violation of its docstring.

### 4.5 Where is the pipe in `anon_pipe_write`?

The previous session reported the CHILD blocks in `anon_pipe_write`
immediately after fork (STATUS lines 178–184).  Commit
`c0a7ac3f905c`'s message captures the root cause:

> Modern glibc's syscall() / kill() / pwrite() wrappers route
> through `__syscall_cancel`, which has been observed to write to
> an internal libc cancellation pipe whose reader pthread does NOT
> exist in a forked child (raw `__NR_fork` only duplicates the
> calling thread).  The child blocks forever in anon_pipe_write to
> that internal pipe before reaching any of our code.

So the pipe is **glibc's internal cancellation pipe** (in
`__syscall_cancel`).  Not a UML pipe; not visible in the source
tree.  The commit converted three template_pause helpers
(`os_template_pause_fork`, `os_template_pause_stop_self`,
`os_template_pause_child_exit`) to inline-asm `syscall`
instructions to bypass it.

**But the fix is incomplete.**  The child path still calls:

- `um_skas_respawn_all_stubs()` → `start_userspace_redo()` →
  `start_userspace()` (`arch/um/os-Linux/skas/process.c:377`).
- `start_userspace` calls **glibc** `socketpair()` (line 410),
  **glibc** `clone()` (line 427), and `wait_stub_done_seccomp`
  which uses `syscall(__NR_futex, ...)` (line 143, glibc
  syscall()).

Each of these can route through `__syscall_cancel` → the dead
cancellation pipe in the forked child.  The previous session
verified the `os_template_pause_*` helpers are inline-asm; they did
NOT verify the same for `start_userspace` and its dependencies.
Per the c0a7ac3f commit message itself: "the v1-ceiling secondary
hazard fires" — that hazard is partially the same cancellation-pipe
issue, just one call-frame further down.

`grep -n 'syscall(__NR_\|CATCH_EINTR' arch/um/os-Linux/skas/
process.c` shows:
- line 118 `CATCH_EINTR(syscall(__NR_sendmsg, ...))` — glibc
- line 124 `CATCH_EINTR(syscall(__NR_futex, ...))` — glibc
- line 143 `syscall(__NR_futex, ..., FUTEX_WAIT, ...)` — glibc
- line 518 `syscall(__NR_wait4, mm_id->pid, ...)` — glibc

All four are reachable from the forked child's path via
`um_skas_respawn_all_stubs`.  The child should NEVER call any of
these — its respawn path should use raw inline-asm syscalls (or
the child should skip respawn entirely and lazily spawn on first
guest userspace syscall, matching snapshot.c's
worker_init contract at lines 519–522).

### 4.6 What in UML's host-process state corrupts a saved-RIP after fork(2)?

The master's crash IP at `um_template_pause_enter+0xf0` is the
function epilogue's `ret`.  `ret` pops 8 bytes from `%rsp` into
`%rip`.  The popped value is small (0x4, 0x2d6b62, 0x2e3e34) — these
look like:

- 0x4 → NULL function call (popped 0x0000000000000004).  Pattern:
  the stack slot was overwritten with a small offset value.
- 0x2d6b62 / 0x2e3e34 → ~3 MB into a low VA range.  This range is
  the **guest-userspace VA** seen by UML's SKAS stub (STUB_START =
  0x68800000 is much higher, but guest-userspace's text segment
  typically starts at 0x00400000 in static-link convention; with
  ASLR enabled, the guest's executable maps somewhere in the
  0x200000–0x600000 range).

What is the mechanism for the stack slot to acquire these values?

**Hypothesis 1: a `longjmp` whose target jmp_buf has a guest-
userspace IP.**  UML's task scheduler uses `switch_threads(jmp_buf
*me, jmp_buf *you)` (`arch/um/os-Linux/skas/process.c:621–627`),
which does `UML_SETJMP(me)` then `UML_LONGJMP(you, 1)`.  The libc
`longjmp` restores `%rsp` + `%rip` from the jmp_buf.  If the master
schedules into a task whose jmp_buf has stale guest-userspace IP +
%rsp values, libc `longjmp` writes those values to the CPU.
**THIS MATCHES.** Per `04-risks/signal-reentry-in-fork-window.md`
lines 28–74 the mechanism is:

  1. Some host signal (SIGALRM, queued during the SIGSTOP/SIGCONT
     window from the second `one_pause_cycle`) arrives.
  2. With `signals_enabled` ineffectively gated (D41 entry
     contract violation per §4.3), `unblock_signals()` runs from
     within `sig_handler_common` (signal.c:65) when ANY non-IRQ
     signal arrives.
  3. `unblock_signals` drains `signals_pending`, including a
     deferred SIGALRM → `timer_real_alarm_handler` → `do_IRQ` →
     scheduler → `schedule()` → `switch_threads` → `UML_LONGJMP`
     into some target task's `thread.switch_buf`.
  4. The target task is a CoW'd init.sh whose `thread.switch_buf`
     jmp_buf's `JB_IP` was captured by `setjmp` at its last
     context-switch-out — which was during guest userspace
     execution, with `JB_IP` pointing at guest userspace code.
  5. `longjmp` jumps to a guest-userspace IP that has no valid
     mapping in the master (the stub child that mediated the
     guest IP is dead — torn down pre-fork).
  6. Host SIGSEGV.  UML's `relay_signal` sees `is_user == false`
     and `address < TASK_SIZE`, panics at `trap.c:372` or 396.

  The stack-trace shown (`um_template_pause_enter+0xf0`) is just
  the LAST KNOWN kernel-mode stack frame UML's panic walker
  printed — not where the fault occurred.  The actual fault was
  the IP popped from longjmp.

  Verification (file:line):
  - `arch/um/include/shared/longjmp.h:11–13` —
    `UML_LONGJMP(buf, val)` is `longjmp(*buf, val)`, the libc
    longjmp.
  - `arch/um/os-Linux/skas/process.c:625–626` — `switch_threads`
    calls `UML_LONGJMP(you, 1)`.
  - `arch/um/kernel/process.c:75–85` — `__switch_to` is invoked
    from the scheduler; it calls `um_backend_dispatch(context_
    switch, from, to)` which routes to
    `arch/um/backend/seccomp/thread.c:17–20`'s
    `seccomp_context_switch` which calls `switch_threads`.
  - `arch/um/os-Linux/signal.c:65` — `sig_handler_common`
    unconditionally calls `unblock_signals_trace()` for any
    non-IRQ signal.
  - `arch/um/os-Linux/signal.c:409–410` — `unblock_signals`
    fires queued SIGALRM via `timer_real_alarm_handler(NULL)`.

**Hypothesis 2: sigaltstack stack-frame overwrite of the kernel
stack.**  `set_sigstack(cpu_irqstacks[0], THREAD_SIZE)`
(`arch/um/kernel/skas/process.c:36`) registers a 16 KB alt-stack
in .bss.  After fork, the child inherits this registration (the
.bss is CoW'd).  When a host signal arrives in EITHER process, the
host kernel writes the signal frame to `cpu_irqstacks[0]`.  But:
- The alt-stack is a SEPARATE address from the kernel C stack
  (the THREAD_SIZE area pointed at by `current->stack`); they
  don't overlap.
- The kernel return path (`um_template_pause_enter`'s frame)
  lives on the kernel C stack, not the alt-stack.
- So sigaltstack writes can't corrupt `um_template_pause_enter`'s
  saved RIP.
Hypothesis 2 REFUTED.

**Hypothesis 3: KASAN shadow corruption.**  Per
`arch/um/os-Linux/mem.c:64–76`, under `CONFIG_UM_FUZZ_HOOKS` the
KASAN shadow is CoW'd into the child (no MADV_DONTFORK).  KASAN
shadow writes are local to the writer's mm; no aliasing.  KASAN
slab-OOB was the SYMPTOM in 3d-c (per snapshot.c lines 562–569)
but that was a worker-side issue, not a master-side stack-slot
overwrite.
Hypothesis 3 REFUTED for the master's epilogue crash.

**Hypothesis 4: ftrace return-thunk shadow stack.**  UML doesn't
enable HAVE_FUNCTION_GRAPH_RET_ADDR_PTR or kernel CET; the
`return-thunk` referred to is the per-task graph-tracer shadow
stack.  `grep -rn 'shadow_stack\|return_thunk' arch/um/` returns
nothing (verified above).
Hypothesis 4 REFUTED.

**Answer to Q6**: the corruption is **not** of a stack slot.
`longjmp` into a stale jmp_buf restores `%rip` + `%rsp` directly
from registers in the jmp_buf, without touching any stack slot.
The "saved-RIP" the previous session inferred from `ret` popping a
small value is actually the `JB_IP` of a CoW'd task's jmp_buf.  The
unwind shows `um_template_pause_enter+0xf0` because the panic
walker scanned the kernel stack looking for the most recent
kernel-mode frame and found `um_template_pause_enter`'s — but the
actual faulting IP is the value `longjmp` loaded into `%rip`, NOT
the `ret`-popped value.

### 4.7 Recent UML fork-fix commits since 2024

`git log --all --oneline -i --grep='fork.*fix\|fork.*hazard\|fork.*broken\|cow.*broken'` returns 16 candidates; the UML-relevant ones are:

| Commit | Date | Relevance |
|--------|------|-----------|
| `c0a7ac3f905c` | 2026-05-20 | Previous session's inline-asm fix for template_pause helpers' cancellation pipe.  Partial — `start_userspace` still uses glibc syscall (§4.5). |
| `9402746dba54` | 2026-05-20 | Adds `um_skas_forget_all_stubs` (mark id.pid=-1 without killing).  Predecessor evidence that the previous session knew of the "child shouldn't kill master's stubs" pattern. |
| `851cd7b7eba0` | 2026-05-20 | Early-fork mode (`um_template_pause=early-fork`).  Bisect final entry; master still crashes. |
| `b890c0a706e2` | 2026-05-20 | STATUS doc; locks in the "v1 ceiling architectural" framing. |
| `f8bb690f91b8` | 2026-04 | Revert of the AFL forkserver poll-waitpid fix attempt + revert of `3dca0c2ae50b`; documents v1 ceiling honestly. |
| `b2e391348e80` | 2026-04 | UML-native signal gating switchover for snapshot.c; the right primitive per D41. |
| `498066d13937` | 2026-04 | `um: snapshot: assert signals_enabled == 1 at ready-point entry (D41)`.  **This is the commit that landed the D41 entry assertion in `um_snapshot_assert_ready` — the assertion that template_pause's `assert_fork_safety` is missing (§4.3).** |
| `f969325d3a8d` | 2026-04 | The `sched_worker_detach_other_tasks` upstream helper landing. |
| `9d0dd8ed3181` | 2026-04 | Worker returns from `um_snapshot_ready` and runs trivial guest code (the 3d-c demo). |
| `c49a1061b81d` | 2026-04 | Worker rebuild infra (SIGIO + POSIX timer rebuild). |
| `8f5e8b2159ea` | 2026-04 | Worker forget step. |
| `2bf287b64b16` | 2026-04 | Multi-iteration forkserver loop (the AFL parent permanently inside `read(fd198)` shape). |

**Most relevant to the current debugging**: `498066d13937` — the
`signals_enabled == 1` entry assertion that `assert_fork_safety`
SKIPS.  Porting just this single check would have surfaced (with a
WARN_ONCE) whether the early-pause case is entering with
signals_enabled at 0, and would have given the previous session
a concrete error message to chase rather than the generic "Segfault
with no mm."

---

## 5. What to try next — concrete files + falsifiable prediction

### 5.1 The single most important thing the prior session missed

**The AFL forkserver parent NEVER returns up the syscall stack
between iterations** — it lives forever inside the blocking
`os_snapshot_read_all(UM_FORKSERVER_CTL_FD, &cmd, sizeof(cmd))` at
`arch/um/kernel/snapshot.c:306`.  That `read()` is a host blocking
syscall; with `signals_enabled == 0` UML's IRQ dispatch defers any
queued SIGALRM/SIGCHLD into `signals_pending`; the parent's
scheduler never runs because the parent is not in a UML kernel
critical section that would call `schedule()`.  This works **only
because** the parent's host main thread is permanently parked in
the read.

Template_pause's parent, by contrast, does its iteration via
`one_pause_cycle()` (template_pause.c:179–198), which uses
`os_template_pause_stop_self()` (host `kill(self, SIGSTOP)`) as the
parking primitive.  SIGSTOP is the right host primitive for "wait
for supervisor SIGCONT," but between SIGCONT and the next SIGSTOP
the parent **runs ordinary kernel code** — reading the identity
blob (`read_identity_blob`), writing the child pid
(`os_template_pause_write_child_pid`), iterating the loop, calling
`os_snapshot_block_iter_signals` (idempotent if already gated),
calling `um_skas_teardown_all_stubs` (blocking wait4), calling
`os_template_pause_fork` (raw fork), calling
`um_skas_respawn_all_stubs` (blocking futex via `wait_stub_done_
seccomp`), and so on.  EVERY one of those points is a host-context
window where SIGALRM (queued during the previous SIGSTOP) is
delivered, and any synchronous fault unblocks signals
synchronously per signal.c:65.

The PHASE2A-DESIGN memo says "Parent never re-enters UML kernel
scheduling between forks" (template_pause.c:247–250).  That claim
is **false**: every iteration of the loop body unwinds back through
`os_template_pause_stop_self` and reenters the loop, and every
helper call (`um_skas_teardown_all_stubs`, `um_skas_respawn_all_
stubs`) is a UML kernel call.

### 5.2 Concrete next step (files + pseudocode)

**Two patches, ordered**:

**Patch A** — port the AFL `um_snapshot_assert_ready` entry
preconditions into template_pause's `assert_fork_safety`.

File: `arch/um/kernel/template_pause.c` (function
`assert_fork_safety` at line 211).

Pseudocode (add at the top of the function):

```c
static int assert_fork_safety(const char *named_point)
{
    /* Port from arch/um/kernel/snapshot.c:142–198 (D41 +
     * D37 pull-forward #2 contract).  Without these the
     * os_snapshot_block_iter_signals() at line 274 is a
     * silent no-op when signals_enabled is already 0,
     * collapsing the entire signal-gating contract.
     */
    if (WARN_ONCE(in_hardirq() || in_softirq(), ...))
        return -EBUSY;
    if (WARN_ONCE(signal_pending(current), ...))
        return -EBUSY;
    if (WARN_ONCE(num_online_cpus() > 1, ...))
        return -EBUSY;
    if (WARN_ONCE(um_get_signals() != 1, ...))
        return -EBUSY;

    /* existing KVM + mid-syscall checks ... */
}
```

This will MAKE THE EARLY-PAUSE FAILURE LOUDER (because at late_init
call_sync time `signals_enabled` may legitimately be 0; the
existing failure mode of "Segfault with no mm" will become "WARN:
signals_enabled is 0 at ready-point entry, refused with -EBUSY").
That's a STRICT IMPROVEMENT — the WARN message is actionable; the
panic is not.

**Patch B** — restructure `fork_on_resume_loop` so the parent
NEVER returns up the syscall stack between iterations.  Match
AFL's "parent permanently blocked in a host syscall" shape.

File: `arch/um/kernel/template_pause.c` (function
`fork_on_resume_loop` at line 258).

The current loop body's PARENT path returns from the
`os_template_pause_stop_self` (kill+SIGSTOP) call when the
supervisor SIGCONTs the master.  Between SIGCONT and the next
SIGSTOP it does many things in host-context, each a SIGALRM
exposure window.

The fix is to add a SECOND blocking primitive — a `read()` from a
pipe whose other end is held by the supervisor — that the parent
sits in EXCEPT when actively doing teardown/fork/respawn work.  The
pipe doesn't carry payload; the supervisor just `write(1 byte)`
each time it wants a new take.  Pseudocode:

```c
/* New: a host pipe whose reader stays in this loop forever. */
int wake_fd = os_template_pause_open_wake_pipe();  /* new helper */

for (;;) {
    /* Park in a host blocking read.  signals_enabled is 0;
     * SIGALRM/SIGCHLD queue.  This is the AFL pattern
     * (snapshot.c:306).
     */
    char cmd;
    n = os_template_pause_blocking_read(wake_fd, &cmd, 1);
    if (n <= 0) break;

    /* When the supervisor pokes us, do exactly one take.
     * Order is teardown → fork → respawn → write child pid → loop.
     * Same as today, MINUS the "raise another SIGSTOP" step —
     * the supervisor's next write does the wake.
     */
    n = um_skas_teardown_all_stubs();
    if (n < 0) return n;
    child_pid = os_template_pause_fork();
    if (child_pid == 0) {
        /* child path: respawn, worker_init, return up the
         * stack.  Unchanged from today.
         */
    }
    /* parent path: respawn + write child pid */
    n = um_skas_respawn_all_stubs();
    /* ... handle errors ... */
    os_template_pause_write_child_pid(...);
    /* Loop back to read(wake_fd).  No SIGSTOP. */
}
```

The new wake-pipe primitive replaces SIGSTOP as the parent's
parking mechanism.  SIGSTOP is still useful for the FIRST pause
(Phase 1a's invariant: "master is the taken instance"), but
once the master has done its first take it sits in `read(wake_fd)`
permanently, and the supervisor's `write(wake_fd, 1)` byte
triggers each subsequent take.

This precisely matches the AFL forkserver protocol shape with the
control channel inverted: AFL has the FUZZER write commands to the
parent's read; here the SUPERVISOR writes wake bytes.

**Estimated diff size**: ~80 lines (a new os-Linux helper for the
pipe wake, plus the loop restructuring).  No new in-tree
cross-subsystem touches.  Drops the entire SIGALRM-into-stale-jmp_buf
hazard class as a side effect.

### 5.3 Falsifiable predictions

1. **Patch A alone** will surface a WARN_ONCE on the early-pause
   path saying "signals_enabled is 0 at ready-point entry."
   Verifiable: build with `CONFIG_UM_TEMPLATE_PAUSE_FORK=y`, boot
   with `um_template_pause=early-fork`, dmesg shows the WARN
   instead of the panic.  If the WARN does NOT appear, then
   signals_enabled was 1 at entry — and the current early-pause
   crash is a different bug class than I've claimed (the SIGALRM
   reentry mechanism in §4.6).  Either outcome is informative.

2. **Patch A + Patch B together** will make the master survive >=10
   sequential takes against `tools/testing/selftests/um/pool-spawn-
   smoke/` (extended with `--takes 10`).  Verifiable: pool-spawn-
   smoke's exit code becomes 0 for a 10-take run; dmesg shows 10
   "torn down N stub(s) pre-fork" lines and 10 "fork returned
   pid=N" lines, with no "Kernel mode fault" / "Kernel tried to
   access user memory" / "Segfault with no mm" lines.

3. If Patch A + Patch B still crashes with the same `um_template_
   pause_enter+0xf0` symptom, then the SIGALRM-into-stale-jmp_buf
   hazard is NOT the dominant failure — and we should look at the
   glibc cancellation-pipe issue in `start_userspace`'s respawn
   path (§4.5).  Patch C would convert
   `arch/um/os-Linux/skas/process.c`'s glibc syscalls inside
   `start_userspace` + `wait_stub_done_seccomp` + `os_skas_reap_
   stub` to inline-asm.  That's another ~50 lines.

4. If Patches A + B + C all fail to make the master survive, THEN
   the "multi-week scheduler rework" claim in
   `09-fork-server-STATUS.md` lines 252–272 is justified.  But not
   before — the previous session jumped that conclusion without
   doing A, B, or C.

5. **Negative control**: Patch A alone, applied to a build where
   signals_enabled IS 1 at template_pause entry (the late-pause
   case via `/proc/um/template_pause`), should produce no behavior
   change (the assertion passes silently).  This confirms the
   assertion is a no-op in the working case and only fires when
   the contract is violated — same as `um_snapshot_assert_ready`
   today.

### 5.4 What the previous session got right (worth keeping)

- The structural Patches 1+2+4 from PHASE2A-DESIGN — the
  `start_userspace_redo` extraction, mm_list-walking helpers, and
  wire-in to the loop — are all reusable.
- The inline-asm conversion in `arch/um/os-Linux/template_pause.c`
  is correct and necessary; commit `c0a7ac3f905c` should NOT be
  reverted.
- The selftest harness (`tools/testing/selftests/um/template-
  pause-fork-smoke/`) is good — its SKIP-vs-FAIL discipline
  correctly distinguishes "we have structural progress but the v1
  ceiling fires" from "we regressed."
- `um_skas_forget_all_stubs()` (commit `9402746dba54`) is the
  right helper for the CHILD path post-fork — the child shouldn't
  kill the master's stubs (the master IS still using them); it
  should just disclaim them from its CoW'd mm_list.

The error was in the design (PHASE2A-DESIGN.md), not in the
implementation.  The implementation is correct for the design as
specified; the design didn't capture the four AFL invariants.

---

## 6. Cross-references

- `Documentation/virt/uml/redesign/04-risks/decisions-log.md` —
  D35, D36, D37, D38, D39, D41, D42, D43.
- `Documentation/virt/uml/redesign/04-risks/signal-reentry-in-fork-
  window.md` — the load-bearing forensic memo the previous
  session never engaged with.
- `Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-
  gaps/09-snapshot-forkserver.md` — the v1 design memo for the
  AFL forkserver; lines 595–615 explicitly say "v1 ceiling: short
  non-blocking guest programs only."
- `arch/um/kernel/snapshot.c:142–198` (`um_snapshot_assert_ready`),
  `:231–429` (`um_snapshot_forkserver_loop`), `:493–615`
  (`um_snapshot_worker_init`).
- `arch/um/kernel/template_pause.c:211–235` (`assert_fork_safety`,
  the missing-checks site), `:258–369` (`fork_on_resume_loop`, the
  unwind-between-iterations site).
- `arch/um/os-Linux/skas/process.c:82–180` (`wait_stub_done_
  seccomp`), `:377–477` (`start_userspace`), `:504–559`
  (`os_skas_reap_stub`), `:572–579` (`start_userspace_redo`),
  `:621–627` (`switch_threads`).
- `arch/um/os-Linux/signal.c:65` (`sig_handler_common`
  unconditional `unblock_signals_trace` for non-IRQ signals — the
  load-bearing line for the SIGALRM-via-fault reentry hazard).
- `arch/um/os-Linux/signal.c:409–410` (`unblock_signals` synchronously
  fires queued SIGALRM via `timer_real_alarm_handler`).
- `arch/um/include/shared/longjmp.h:11–13` (`UML_LONGJMP` = libc
  `longjmp`).
- `kernel/sched/core.c:2266–2288` (`sched_worker_detach_other_tasks`
  — worker-side use only per D42).
- Commits: `498066d13937` (the D41 entry-assertion landing for
  snapshot.c; the model for the missing template_pause check),
  `b2e391348e80` (UML-native signal gating switchover),
  `f8bb690f91b8` (the wait4-in-parent revert that documented
  the v1 ceiling honestly), `c0a7ac3f905c` (template_pause helper
  inline-asm conversion).
