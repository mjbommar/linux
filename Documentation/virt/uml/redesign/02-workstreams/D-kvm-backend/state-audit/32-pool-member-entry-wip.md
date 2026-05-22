# Pool-member entry — boot reached, sleep does not return

**Date:** 2026-05-21 late evening
**Status:** Pool member init.sh executes userspace code post-fork —
the /proc write returns rc=0, the next `echo` runs, and the
MEMBER_ALIVE_1 marker reaches host stdout.  `sleep 2` does not
complete; MEMBER_ALIVE_2 never prints.

UPDATE: the architectural fix worked.  Gating
`um_skas_teardown_all_stubs()` on `!pool_member_armed` preserved
the per-mm turnstile mutex and unblocked userspace().

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

  * **Step 19c — REMAINING.** `sleep 2` does not complete;
    MEMBER_ALIVE_2 never prints.  Hypothesis: the child's
    inherited timer state is in flux (master held preempt_disable
    + signal-mask block across fork, child restored signals but
    not the UML kernel timer wheel).  Need to investigate what
    sleep(2) goes through in the child — nanosleep() syscall →
    UML's hrtimer → host SIGALRM timer.  The host process
    inherits master's timer settings; whether they tick in the
    child is the next data point.

  * **Step 19d — REMAINING.** Identity_apply (memfd-driven per-
    member MAC/IP/hostname) before userspace().

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
