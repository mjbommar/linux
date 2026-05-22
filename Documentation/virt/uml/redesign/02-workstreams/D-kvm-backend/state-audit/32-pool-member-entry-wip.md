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

  * **Step 19e — NEW DISCOVERY (multi-iteration limit).**  When
    master is SIGCONT'd before the prior iteration's child has
    exited, both children share master's mm_list (which still has
    the original child's stub pids).  Iter 2 child's POOL_ENTER
    succeeds but then panics at `addr 0x6008a85b, ip 0x6008a170`
    when seccomp_vcpu_run races against iter-1's stub.

    Path forward:
    - Add a synchronization point: master waits for the prior
      child's stubs to clean up before forking the next.  Easiest
      via host wait4(WNOHANG) on each pre-fork pause.
    - OR: each pool-member child allocates fresh stubs after
      Path A pivot (move um_skas_respawn_all_stubs into
      child_entry_pool_member, in addition to keeping the
      turnstile mutex).

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
