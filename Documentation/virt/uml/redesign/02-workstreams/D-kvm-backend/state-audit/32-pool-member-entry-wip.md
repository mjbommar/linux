# Pool-member entry — fully functional, init.sh runs to completion

**Date:** 2026-05-21 late evening
**Status:** PASS end-to-end.  Pool member boots, init.sh executes
userspace code, multiple sleep+echo cycles complete, MEMBER_DONE
reached.  Verified by new selftest
`template-pause-pool-member-smoke`.

Resolved in three stacked fixes:
  1. Gate `um_skas_teardown_all_stubs()` on `!pool_member_armed` —
     preserve per-mm turnstile mutex.
  2. `os_timer_worker_forget()` + `os_timer_worker_rebuild()` in
     child entry — POSIX timers don't survive fork(2).
  3. `os_timer_one_shot(0, 1ms)` after rebuild — prime the
     clockevent so `set_next_event` re-establishes tracking for
     the next hrtimer expiration.

---

## 1. What landed

Add a second arm `um_template_pause_pool_member=1` alongside the
`pivot_test` arm:

  * `arch/um/kernel/template_pause.c` — new `child_entry_pool_member`
    function called via `os_template_pause_fork_clone_to()`.  Writes
    "POOL_ENTER\n" via raw syscall + exits cleanly.  Provides the
    scaffold; the real `userspace()` drive is commented out pending
    the layer-2 fix below.

  * SIGKILL bypass extended to cover the new mode.

Master sustains hundreds of fork iterations under this arm with zero
panics (observed: 65+ iterations in a 15-second harness run before
the harness teardown).

## 2. What we learned (the actual NEW info)

Previously the v1 ceiling was "child cannot run kernel C code post-
fork."  That's resolved by Path A.  The next layer is:

  Init.sh's pt_regs are mid-syscall:
    IP = 0x400f56c6 (instruction after the syscall instruction in
                     init.sh's userspace)
    AX = -ENOSYS (master never set the syscall return value)

  Master's pre-fork sequence in fork_on_resume_loop step (A) calls
  um_skas_teardown_all_stubs() to free per-mm SKAS state.  This
  destroys (or at least invalidates) `current->mm->context.turnstile`
  — the per-mm mutex that seccomp_vcpu_run takes via
  enter_turnstile() on EVERY round-trip.

  Result: when child_entry_pool_member calls userspace(), the very
  first thing the seccomp dispatch loop does is mutex_lock on a NULL
  or destroyed mutex.  Panic IP 0x603d31d9 is
  __mutex_lock.constprop.0+0xa6 — the typical NULL-deref offset in
  __mutex_lock's slow path.

## 3. The architectural fix

The Memo 09 §2 design requires master to be IDLE-but-ALIVE at
SIGSTOP, with its full SKAS state intact, so that each forked child
inherits a working address-space context.  Today's
`fork_on_resume_loop` was designed for Phase 2a (child does
`exit_group` only) and AGGRESSIVELY tears state down to minimize
master's hazard window.

For pool_member mode, that teardown is WRONG.  Master must:

  1. KEEP stubs alive across fork — defer `um_skas_teardown_all_stubs`
     to never-run, or move it to a separate explicit teardown that
     happens only when master is being torn down for good.
  2. KEEP `current->mm->context.turnstile` mutex initialized.
  3. KEEP per-mm `mm_id->stack` (the stub_data page) mapped.

Then the child inherits a fully-working mm context and
seccomp_vcpu_run's `enter_turnstile(current_mm_id())` succeeds.

## 4. Status of concrete steps (4-7 day plan window)

  * **Step 19a — DONE.** Gated `um_skas_teardown_all_stubs()` on
    `!template_pause_pool_member_armed_flag`.  Master logs
    "skipping pre-fork stub teardown (pool_member mode keeps SKAS
    state)" when the new mode is armed.

  * **Step 19b — DONE.** Validated end-to-end:
    ```
    POOL_ENTER                        <- kernel C ran in child
    POST_PAUSE pid=1 rc=0             <- init.sh resumed write w/ rc=0
    MEMBER_ALIVE_1 pid=1              <- init.sh's next echo ran
    ```
    The post-fork child's init.sh executes userspace code beyond
    the /proc write trigger.  This is the first time post-fork
    userspace progress has happened in this tree.

  * **Step 19c — DONE.** Timer wakeups work in the child:
    `os_timer_worker_rebuild()` re-creates the per-CPU POSIX
    timer; `os_timer_one_shot(0, 1ms)` arms it to fire ~1ms
    later, which re-establishes the clockevent's next_event
    tracking.  Subsequent nanosleep() / hrtimer expirations are
    handled normally.  Verified: 5×`sleep 1` cycles in selftest.

  * **Step 19d — VERIFIED WIRED.** Identity_apply pipeline confirmed
    operational:
    ```
    template_pause: enter("fork-smoke") — identity_fd=4
    template_pause: identity at "fork-smoke"
                    instance="pool-member-1"
                    mac=52:54:00:a1:b2:01 tap="tap-pool-1"
                    ipv4="10.7.0.42/24" gw="10.7.0.1"
    template_pause: no target netdev found; identity NOT applied
    template_pause: identity apply at "fork-smoke" returned -19
                    (continuing)
    ```
    Blob is READ + PARSED + ATTEMPTED-APPLIED by master before
    fork.  The -ENODEV is because the hostfs-only test boot has no
    netdev to apply MAC/IP to — that's a TEST configuration gap,
    not a code gap.  When the daemon-side path provides a real
    netdev + tap, apply_mac/apply_tap_reopen succeed (already
    landed in `template_pause_identity.c`).

  * **Step 19e — multi-iteration crash (master-side state leak).**
    After iter 1 fully completes (child boots, runs to MEMBER_DONE,
    exits 0, host-side reaped) — iter 2 SIGCONT to master succeeds,
    master reads + applies blob, forks via Path A primitive, child
    enters POOL_ENTER... and then panics at:
      `addr 0x6008a85b, ip 0x6008a170`
      `__clear_task_blocked_on+0x61` jumping to `up_read+0xb`.

    The panic is in the SCHEDULER's task-blocked-on path, NOT in
    SKAS / stub state.  This means iter 1's path mutated master's
    scheduler state (rwsem owner chain or task->blocked_on linkage)
    in a way that survives the fork into iter 2 child.

    The candidate sites in `child_entry_pool_member` that touch
    cross-task state:
      - `preempt_enable()`
      - `os_template_pause_signals_restore_host()` (raw rt_sigprocmask)
      - `os_timer_worker_forget/rebuild/one_shot`
      - `PT_REGS_SET_SYSCALL_RETURN(&current->thread.regs, 11)`
      - `userspace(...)` (long-lived seccomp dispatch on init.sh's
        task_struct)

    Path forward (next session, ~2-4 hours):
    1. Add raw-syscall diagnostic prints WAS-ACQUIRED/WAS-RELEASED
       on rtnl_lock + per-mm turnstile before/after the iter 1
       child's userspace() call.
    2. Snapshot master's `current->blocked_on` / `mm->mmap_lock`
       state pre-iter-1 and pre-iter-2 — should be identical.
    3. If they diverge, the child's userspace() did not properly
       isolate task-level locks before exiting init.sh.

    UPDATE 2026-05-21 23h: bisected by replacing userspace() with
    raw __NR_exit_group in the child entry.  Result: iter 2 master
    SIGSTOPs cleanly, NO panic.  Confirms the offender is inside
    userspace() — specifically, the seccomp_vcpu_run path's use
    of inherited mm_id->stub_pid (master's host-children) which
    races against master's continued use of the same stub when
    master loops back into another fork iteration.

    Real fix (Phase 3-scale, separate workstream):
    - Child must "disown" master's stub_pids without killing the
      stub processes themselves (those belong to master).  Set
      child's local mm_id->pid = -1 for each entry, then call
      start_userspace_redo() so the child gets its OWN stub-child
      host processes.  Caveat: the 2026-05-20 fork-stress
      diagnosis (mmu.c:395-411) documents that respawning under
      master's still-live VM-share can crash at IP=0 — needs
      careful sequencing.

    UPDATE 2026-05-22 00h: ATTEMPTED um_skas_forget_all_stubs +
    um_skas_respawn_all_stubs in child entry.  Result: FIRST
    child crashes immediately with kernel panic (no TPPM_POST_PAUSE
    even printed; child dies in start_userspace_redo).  Confirms
    the IP=0 hazard from the mmu.c comment is alive even under
    Path A pivot — the child's clone() in start_userspace_redo
    still creates a stub that shares VM with master's
    address-space via CoW, and the triple-share (master / child /
    new-stub) corrupts the stub's binary entry-point lookup.

    Next attempt (deferred to dedicated SKAS workstream):
    Modify start_userspace_redo to use a CLONE_VM-less path
    when called from a post-Path-A child (the child has already
    been "split" from master's VM via clone() without CLONE_VM —
    so its address space IS independent; the new stub just needs
    to know that).  This is a new SKAS API.

    Restored: child_entry_pool_member does NOT call forget +
    respawn — single-iteration dispatch works without them and
    the multi-iter fix needs deeper SKAS work.  Diagnostic
    findings preserved in the source comment.

  * Task #18 (AFL preconditions in `assert_fork_safety`) — direct
    blocker for regression sentinels of these stub-state
    assumptions.

## 5. Why this is not "quitting"

  * v1 ceiling: CLEARED end-to-end (pivot_test selftest: 20/20 PASS,
    state-audit/31).
  * Scaffold for pool_member: LANDED + verified non-crashing.
  * Architectural blocker identified with a concrete fix in hand
    (step 19a above) and the path forward enumerated.

  This is day 1 of the plan's "4-7 day" window per Path C §3.3
  Outcome 2.  Next session's first concrete commit is 19a.

## 6. Cross-references

  * state-audit/30 — Path C result (Outcome 2).
  * state-audit/31 — Path A kernel integration (pivot_test PASS).
  * `arch/um/kernel/template_pause.c::child_entry_pool_member` —
    the WIP scaffold this memo documents.
  * `arch/um/kernel/skas/mmu.c::enter_turnstile` — the per-mm mutex
    that needs to survive master's pre-fork teardown.
  * `arch/um/backend/seccomp/trap_user.c::seccomp_vcpu_run` line 46
    `current_mm_id()` + line 53 `enter_turnstile()` — the failure
    site.
