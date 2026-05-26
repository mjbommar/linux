# Path A kernel integration — v1 ceiling crossed end-to-end

**Date:** 2026-05-21
**Status:** PASS — 20/20 PIVOT_OK iterations, 0 v1-ceiling panics.

---

## 1. What landed

Three pieces, all squarely on the production code path (no
`CONFIG_*_DIAG` gating):

  * `arch/um/os-Linux/template_pause.c` — new host-side helper
    `os_template_pause_fork_clone_to(entry)`.  Same private-stack
    `__NR_clone` as `os_template_pause_fork_clone()`, but the child's
    inline-asm branch ends with `xorq %rbp,%rbp; jmpq *%[entry]`
    instead of `__NR_exit_group(0)`.  The Path A stack-pivot
    primitive (validated host-side in
    `tools/testing/selftests/um/rt-sigreturn-isolation/`) applied to
    the post-clone child path.

  * `arch/um/kernel/template_pause.c` — new `child_entry_pivot_test`
    function and `um_template_pause_pivot_test=1` cmdline.  When
    armed, master uses `os_template_pause_fork_clone_to(
    child_entry_pivot_test)`; the child runs raw-syscall
    `write(1, "PIVOT_OK\n", 9)` then `exit_group(0)`.  Master skips
    the post-fork SIGKILL (child self-exits cleanly).

  * `arch/um/include/shared/os.h` — public typedef + extern for the
    new helper.

  * `tools/testing/selftests/um/template-pause-pivot-smoke/` — new
    selftest that drives 20 SIGCONT iterations and verifies:
    - PIVOT_OK count >= 5 (sustained operation)
    - no "Kernel tried to access user memory" panic
    - no `um_template_pause_enter+0xf*` IP in the log

  * Wired into `tools/testing/selftests/um/Makefile`'s TARGETS list.

## 2. What this proves

The 2026-05-20 v1 ceiling — kernel panic at
`um_template_pause_enter+0xf6/0x147` (function epilogue's `ret`),
addr=ip=0x6061c910 in guest-userspace VA — does NOT fire on the
new path.

Confirmed by `state-audit/30-path-c-v1-ceiling-confirmed.md`: the
post-fork M-fork child CANNOT return up the syscall stack without
panic.  Confirmed today by this integration: the child CAN execute
kernel C code arbitrarily, as long as it does so on a private stack
without touching the corrupted saved-RIP slots of the parent's
kernel stack.

Numbers (UML on Linux 7.0.0-15-generic, x86_64):

```
PIVOT_OK count   : 20
v1 ceiling panic : False
SIGCONT sent     : 20
PASS
```

## 3. What this does NOT yet do

`child_entry_pivot_test` is a leaf function — it does syscalls and
exits.  A real pool-member entry needs to:

  1. `preempt_enable()` — master held it pre-fork.
  2. Restore signal mask (unblock the host signals master had
     blocked).
  3. `um_template_identity_apply(&blob)` — install MAC/IP/hostname
     identity from the daemon's memfd.
  4. `um_skas_respawn_all_stubs()` — rebuild SKAS stub state
     (currently master tore them down pre-fork).
  5. Drive the guest scheduler / userspace resume so the child's
     `init=` process (init.sh) continues from where master's
     SIGSTOP froze it.

Step 5 is the open work.  The child's userspace task already
exists post-fork (CoW from master); its kernel-mode entry in
`um_template_pause_enter` has been bypassed by the stack-pivot, so
the normal return-to-userspace dance via the syscall return path
isn't available.

Two known options for Step 5:

  * **Option A — sigaltstack + sigreturn.**  Build a `pt_regs`
    representing init.sh at the post-`write(/proc/...)` IP, push
    a synthetic rt_sigframe, `__NR_rt_sigreturn`.  CRIU's pattern.
    This is what the `tools/testing/selftests/um/rt-sigreturn-
    isolation/` test originally tried — the bare primitive worked,
    but reconstructing kernel-side pt_regs for a UML task in a
    kernel context (no userspace TLS) is fragile.

  * **Option B — re-enter the scheduler from `child_entry_pivot_test`
    and let UML's `userspace()` / `interrupt_end()` drive the resume.**
    Calls into the UML scheduler from a clean stack.  Requires
    figuring out which UML kernel task struct represents the post-
    fork child's init.sh — the master and child both believe they
    are PID 1.

Both options require Step 18 (port AFL preconditions into
`assert_fork_safety`) first to make the assumptions explicit.

## 4. Verdict on the 2026-05-21 three-paths plan

  * Path A primitive — PASS, host-side selftest landed
    (`rt-sigreturn-isolation`).
  * Path C v1-ceiling check — Outcome 2, panic confirmed
    (state-audit/30).
  * Path A integration — PASS, kernel selftest landed
    (this memo).

Per the plan's §4.4 decision tree, this triggers:
> "Outcome 2 (Phase 2a panic) → integrate A into 1.3 build-out
> (4-7 days)"

The 1.3 build-out begins.  Next concrete steps:

  * Task #18: port AFL forkserver preconditions into
    `assert_fork_safety` (~50 LoC).  Independently useful as
    a regression sentinel and a prerequisite for the pool-member
    entry above.

  * Task #19 continuation: implement the real
    `template_pause_child_pool_entry()` per §3 above.  Land it as
    an additional cmdline mode (`um_template_pause_pool_member`)
    that keeps the diagnostic `pivot_test` mode alongside as a
    minimal regression sentinel.

## 5. Cross-references

  * `state-audit/30-path-c-v1-ceiling-confirmed.md` — the Outcome 2
    result this work answers.
  * `06-sequencing/post-2026-05-21-three-test-paths.md` — the plan.
  * `06-sequencing/post-2026-05-21-pool-architecture-comparison.md`
    §1.7 (Path A) + §1.3 (pool-member model).
  * `tools/testing/selftests/um/rt-sigreturn-isolation/` — the
    host-side primitive validation.
  * `tools/testing/selftests/um/template-pause-pivot-smoke/` — the
    kernel integration validation landed today.
