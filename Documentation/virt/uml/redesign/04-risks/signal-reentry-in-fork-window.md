# Hazard: SIGALRM reentry into stale `jmp_buf` during the fork-parent non-kernel-exec window

**Date:** 2026-04-23
**Status:** characterized (no fix landed)
**Forensic scope:** Finding #1 from the 2026-04-23 review
(four failed `wait4` variants in the snapshot forkserver
parent-side reap path). This memo exists so that the post-Q1
push Phase II Lift #4d author (the `start_userspace` clone/
wait split) knows what they are avoiding before they start
editing.
**Companion to:** `06-sequencing/post-q1-push.md`
§"Phase I — Lift #5",
`Documentation/virt/uml/snapshot.rst` §"v1 ceiling: exit-
status semantics",
`04-risks/decisions-log.md` D59 (broader `using_seccomp`
layer-1 leak context; the four failed attempts are also
referenced there).

## One-sentence hazard

**Any `wait4`-family host syscall performed by the UML
parent thread after `fork()` and before returning to the
UML trap loop runs inside a window where a fired `SIGALRM`
will `longjmp` into a `jmp_buf` captured pre-fork — whose
saved stack pointer references a stack frame that no longer
exists — and the CPU decodes garbage, delivering SIGILL.**

## Mechanism (one CPU step at a time)

1. UML parent executes `fork()`. Child PID returned. Parent
   still owns its pre-fork call stack in VA — nothing about
   `fork()` itself invalidates it.
2. Parent enters `wait4(pid, &status, 0, NULL)` (or any
   variant — see §"Failed variants" below). Control is now
   in libc / host kernel; UML kernel is NOT executing.
3. Parent's `signals_enabled` TLS flag
   (`arch/um/os-Linux/signal.c:94`) is `1` at this point in
   the snapshot-forkserver flow — the forkserver path
   explicitly re-enables signals after the fork so the next
   AFL iteration can run (D41 landed this invariant in
   `um: snapshot: assert signals_enabled == 1 at ready-
   point entry`).
4. Host timer `SIGALRM` fires (~every 1–10 ms, depending on
   `CONFIG_HZ` and the idle-decision path).
5. Host kernel delivers the signal → UML's
   `hard_handler` in `arch/um/os-Linux/signal.c:228` →
   `handlers[SIGALRM]` = `timer_alarm_handler`
   (line 155).
6. `timer_alarm_handler` sees `signals_enabled == 1` so it
   does NOT defer. It calls `block_signals_trace()` and
   then `timer_real_alarm_handler` → `timer_handler`
   (kernel-side).
7. `timer_handler` → `do_IRQ` → scheduler → ultimately
   `schedule()` → `switch_to` → `switch_threads`
   (`arch/um/os-Linux/skas/process.c`).
8. `switch_threads` performs `setjmp(prev_jb)` + `longjmp
   (next_jb, …)`. The `next_jb` it targets is the jmp_buf
   of whatever UML task the scheduler chose — typically the
   task that called `um_snapshot_ready` and whose stack
   frame was captured **before the fork**.
9. After fork, the UML parent's effective stack image is the
   same VA range as pre-fork — BUT the parent has executed
   arbitrary code (libc fork prologue, host-syscall
   trampoline, wait4 setup) that wrote transiently to lower
   stack slots. Those slots no longer contain what the
   pre-fork `setjmp` recorded.
10. `longjmp` restores `%rsp` + `%rip` + callee-saved
    registers from the stale jmp_buf. Execution resumes at a
    `%rip` that points into code that was valid pre-fork but
    whose surrounding stack frame no longer holds the
    register spill slots / return address the code expects.
11. The CPU reads garbage, decodes it as an instruction,
    and delivers SIGILL — which UML's signal handler
    catches and panics with `Kernel mode signal 4`.

## Why the existing `signals_enabled` TLS guard doesn't help

The TLS flag was designed to defer signals during UML kernel
critical sections. It works perfectly there: the handler
observes `!enabled`, sets a bit in `signals_pending`, and
returns — no reentry. The deferred signal replays on the
next `um_set_signals_trace(1)` call.

The hazard here is orthogonal: the parent-post-fork-wait
window is **not** a UML kernel critical section. It is a
host-userspace window in which signals are legitimately
enabled (the guest-kernel-side `signals_enabled = 1`
invariant is correct and needed for the next AFL iteration
to dispatch). The `signals_enabled` flag is the wrong
primitive to protect this window; it has the wrong
semantics. Setting it to `0` for the wait + restoring on
return would defer SIGALRM correctly, but the *restore step*
itself runs in the same hazardous window — queued signals
fire on restore, re-entering the hazard.

## Why host-level `sigprocmask(SIG_BLOCK, {SIGALRM})` doesn't help

Identical argument: the unblock step (`SIG_UNBLOCK`) fires
the queued SIGALRM inline, inside the same non-kernel-exec
window. The host kernel delivers queued signals atomically
on unblock; there is no safe point between "unblock" and
"we're back in UML kernel context" where we can re-establish
jmp_buf validity.

## Failed variants, all reproduce `Kernel mode signal 4`

Four attempts in the snapshot-forkserver parent-side reap
path (commits `3dca0c2ae50b` + predecessors, reverted in
`f8bb690f91b8`). Each was run against the smoke driver for
many iterations; each crashed the parent with SIGILL in UML
kernel context the first time SIGALRM landed inside the
`wait4` window.

| # | Variant | Why it still crashes |
|---|---------|----------------------|
| A | Bare blocking `wait4(pid, &status, 0, NULL)` | Parent sleeps in the host kernel; SIGALRM wakes it with EINTR and the return path drops back through the UML signal handler inside the stale-jmp_buf window. |
| B | `wait4(pid, &status, WNOHANG, NULL)` spin-poll | Between two non-blocking polls, the parent re-enters host userspace — exactly the window SIGALRM expects to find it in. |
| C | WNOHANG + `clock_nanosleep(tiny)` between polls | `clock_nanosleep` IS a host-syscall non-kernel-exec window; SIGALRM fires during the sleep rather than during the poll, same outcome. |
| D | WNOHANG + `sched_yield` between polls | Same as C — `sched_yield` is a yield in host context, host scheduler, UML kernel is not running. |
| E | Any of the above + host-level `sigprocmask(SIG_BLOCK, {SIGALRM})` bracketing the whole wait | Queued signals fire atomically on `SIG_UNBLOCK`, which is itself inside the non-kernel-exec window. |

Variant E is the tempting "obvious" fix and the reason we
spent four attempts on this. It fails for a subtle reason
captured above: the unblock step is not atomic with respect
to "we are safely back in UML kernel context."

## Call-site inventory — where else this hazard class lurks

These are the paths in the current tree that, under the
wrong conditions, could reproduce the same crash. Phase II
Lift #4d (per D59) splits `start_userspace` into per-backend
impls and touches several of these; it must not pattern its
new code after the snapshot-forkserver attempt.

| Site | File:line | Current safety |
|------|-----------|----------------|
| `start_userspace` stub-child `waitpid` (ptrace path) | `arch/um/os-Linux/skas/process.c:488–496` | Safe today: runs during early boot when `signals_enabled == 0` and no SIGALRM timer has been armed yet. Do NOT move to a post-boot call site without understanding this memo first. |
| `wait_stub_done(int pid)` body | `arch/um/os-Linux/skas/process.c:117` | Safe today: always called inside `block_signals_trace` / `um_set_signals_trace` window from the ptrace trap-loop body (`arch/um/backend/ptrace/trap_user.c`). The trap-loop's own signal gate is what keeps SIGALRM from reentering, not anything in `wait_stub_done`. |
| `do_syscall_stub` ptrace branch `wait_stub_done` call | `arch/um/os-Linux/skas/mem.c:114` | Safe today: called during flush from inside the trap-loop, same signal-gate inheritance as wait_stub_done. |
| Snapshot-forkserver parent-side `wait4` (the Finding #1 victim) | previously `arch/um/os-Linux/process.c::os_snapshot_poll_waitpid_status`, reverted in `f8bb690f91b8` | Not safe. v1 ceiling: status hard-coded to 0. |
| `wait4(-1, WNOHANG)` zombie drain at the TOP of each forkserver iteration | `arch/um/kernel/snapshot.c` drain point | Safe today: runs AT THE TOP of the next iteration, AFTER the parent has returned to UML kernel context once. The first `wait4` of the *next* iteration is inside the UML trap-loop body (= signals-disabled-or-deferred-correctly window). This is why the v1 ceiling is "drain zombies at next iter top" rather than "wait for this iter's worker inline." |

## The shape of a real fix — four options, ranked

### Option A (recommended) — worker reports its own status pre-exit

**Mechanism:** the worker, just before calling `exit(code)`,
writes a synthetic exit status to the status_fd. The parent
never reaps the specific worker synchronously; it reads the
pre-exit status on the status_fd, then moves on. Zombies
drain non-synchronously on the next iteration top (today's
pattern).

**Pros:**
- Sidesteps the hazard entirely — parent never calls a
  `wait4`-family syscall during a window where it matters.
- No UML infrastructure changes required.
- Backward-compatible at the protocol level: the wire
  format's `status` field is already there; we are just
  changing who writes it.

**Cons:**
- The worker has to catch its own crashes (segv, oops,
  panic) and report before exit — i.e. the worker needs an
  atexit / signal-handler that writes the status_fd. This
  is not trivial: the AFL model assumes coverage-map-based
  crash detection, and a kernel panic inside a fuzz-target
  path may not cleanly unwind back to a userspace atexit.
  Status of 0 for "normal exit" + status from crash-signal-
  handler for the common crash classes covers 95%.

**Effort:** ~1 session for the happy-path + crash-signal
handler. Shipping-capable after smoke validation.

### Option B — dedicated wait-thread with post-fork-captured jmp_buf

**Mechanism:** spawn a host-level helper thread (pthread, or
an `O_CLOEXEC` raw-clone) AFTER the fork. That thread
captures its own `setjmp` in the post-fork stack, does the
`wait4`, reports status, exits. The UML parent thread never
touches `wait4` in the hazard window.

**Pros:**
- Gives real exit status (waitpid works).
- No worker-side changes; protocol stays exactly as-written.

**Cons:**
- Adds a pthread / thread-local-storage surface to the UML
  host-abstraction layer that isn't there today. Cross-
  cutting risk: SMP + TLS + signal masking interactions are
  where Finding #1 already lives; we are adding more threads
  to that zone, not fewer.
- UML's `os_early_checks` + `start_up.c` carefully avoids
  host pthreads today; this breaks that invariant.
- Requires audit that the new thread's host-syscall mask
  inherits the right SIGALRM blocking from its creator.

**Effort:** 2–3 sessions including the cross-cutting risk
audit.

### Option C — UML infrastructure fix for the signal/schedule interaction

**Mechanism:** rework `switch_threads` to validate the
target `jmp_buf`'s stack pointer against the current
stack's live range before executing `longjmp`. If stale,
defer the schedule to the next safe-exec point.

**Pros:**
- Fixes the hazard class at the root; every other site in
  the call-site inventory gets safer too.
- Would also improve Phase II Lift #4d's safety margin.

**Cons:**
- Touches `arch/um/os-Linux/skas/process.c`'s lowest-level
  context-switch primitive. Any bug here is a
  "UML kernel panic during schedule" with an even nastier
  debugging story than Finding #1.
- "Validate jmp_buf stack" requires maintaining a live-
  stack-range table per UML task, which doesn't exist
  today. Significant plumbing.

**Effort:** 4+ sessions minimum. Full-blown design memo +
decisions-log entry required first.

### Option D — do nothing different, document v1 ceiling, revisit post-D workstream

**Mechanism:** accept the current v1 ceiling (status=0,
zombies drain at next-iter top). Revisit when workstream D
lands because KVM backend's `run_userspace` may force-
redesign enough of the signal/schedule path that a Fix-C
-like solution becomes possible as a side effect.

**Pros:**
- Zero new engineering risk.
- AFL's coverage-map-based crash detection works today
  despite the status=0 ceiling (syzkaller's `pkg/vm/uml`
  binding per C-08 uses its own out-of-band crash signal).

**Cons:**
- Consumers that want synchronous worker status (outside
  AFL) stay blocked.
- The hazard class remains a footgun for Phase II Lift #4d
  and any future refactor that wants to do post-fork
  `wait4` in a non-trap-loop context.

**Effort:** 0. This memo IS the deliverable.

## Recommendation

**Short term (Phase I / this memo):** Option D. The memo
records the characterized hazard so future work doesn't
retread the four failed variants.

**Medium term (post Phase II Lift #4d, when the per-backend
extraction has surfaced which call sites actually matter):**
Option A (worker reports its own status pre-exit). Cheapest
real fix, self-contained to the snapshot-forkserver code,
keeps the wire format.

**Long term (concurrent with Phase III D-workstream
`run_userspace` realization):** Option C may become natural
as the KVM backend forces a signal/schedule redesign. If
Option A has landed by then, Option C becomes a nice-to-have
that removes the whole hazard class rather than routing
around it.

## What this memo is NOT

- Not a decisions-log entry. This is a forensic hazard-class
  writeup that the next implementer reads BEFORE touching
  adjacent code. `04-risks/decisions-log.md` D59 records the
  accompanying decision ("catalog the 12 `using_seccomp`
  branches and defer to Phase II"); this file characterizes
  the hazard that makes Phase II Lift #4d dangerous.
- Not a fix. No code changes accompany this memo.
- Not an AGENT-PROMPT §"stop and ask" item. The hazard is
  already characterized; the recommendation is explicit.

## Cross-references

- `Documentation/virt/uml/snapshot.rst` §"v1 ceiling: exit-
  status semantics" — user-visible version of the hazard
  (shorter; no call-site inventory).
- `arch/um/os-Linux/signal.c:155` (`timer_alarm_handler`)
  and `arch/um/os-Linux/skas/process.c::switch_threads` —
  the mechanism's two halves.
- `04-risks/decisions-log.md` D41 (signals_enabled == 1
  invariant at ready-point entry), D59 (using_seccomp
  residue catalog, which references Finding #1 attempts).
- `06-sequencing/post-q1-push.md` §"Phase I — Lift #5"
  (this file is that lift's deliverable) and §"Phase II
  Lift #4d" (the Phase II item this file unblocks).
- Commits: `3dca0c2ae50b` (fourth failed attempt that
  motivated this memo), `f8bb690f91b8` (the revert-and-
  document commit).
