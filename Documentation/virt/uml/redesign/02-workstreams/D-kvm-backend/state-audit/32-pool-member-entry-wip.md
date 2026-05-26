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

    UPDATE 2026-05-22 (day 2 attempt — master-side stub_data
    reset): tried adding `um_skas_reset_stub_data_all()` to
    clear MAP_SHARED stub_data scratch fields in master between
    iterations (preserving pid/sock so the live stub child stays
    usable).  Both variants — full reset and "soft" reset
    preserving stub_data->futex — broke iter-1's child: init.sh
    didn't return from the /proc write, no MEMBER_DONE.

    Lesson: stub_data is FULLY load-bearing.  Even the
    syscall_data_len + signal + offsets are part of an
    end-to-end protocol the stub depends on.  Master CANNOT
    safely mutate stub_data while the stub is alive and
    handshake-active.  The reset path is removed; the per-
    iteration corruption is unavoidable without per-child mm.

    Confirmed direction: each pool member needs its OWN mm_id
    with its own MAP_SHARED stub_data page and its own stub
    child.  No middle-ground exists.  Per-member mm pre-
    allocation (Option 1 of two paths I proposed) is the only
    way forward.

    UPDATE 2026-05-22 (day-2 attempt 2 — early-fork +
    pool_member): tried combining um_template_pause=early-fork
    (which pauses BEFORE init.sh runs — mm_list is empty at
    that ready point, no stub aliasing) with the existing
    pool_member arm.

    Result: child fires POOL_ENTER, then kernel panic.
    child_entry_pool_member's userspace() call assumes
    current==init.sh's task with valid user pt_regs.  In early-
    fork context current is the late-initcall task (kernel-mode
    only, no user regs).  Calling userspace() on a kernel-mode
    task crashes.

    Architectural lesson: the child entry function is bound to
    LATE-fork semantics (drop into existing init.sh task).
    Early-fork would need a DIFFERENT child entry that
    reconstructs a clean kernel-side longjmp target and lets
    the initcall caller continue booting normally — the child
    would then go through standard kernel boot path, eventually
    spawning its own fresh init.sh task with fresh mm + stub
    via the normal start_userspace lazy path.

    That's a second child-entry variant (call it
    `child_entry_early_pool_member`) — a third arm separate from
    pivot_test and pool_member.  Approach is sound but is a
    new commit series.

    UPDATE 2026-05-22 (day-2 attempt 3 — early-fork +
    continue_child): added an experimental `continue_child` arm
    that makes the child take ordinary __NR_fork and return up the
    syscall stack normally (Memo 09 §2 original design).
    Combined with `um_template_pause=early-fork` to test if the
    v1 ceiling avoids early mode (no init.sh stub yet).

    Result: v1 ceiling FIRES even in early mode.  Panic IP =
    `um_template_pause_enter+0xf6/0x147` (same offset as
    state-audit/30 documented for late-fork).

    Diagnosis: `os_template_pause_signals_block_host()` is called
    AFTER SIGCONT, not before SIGSTOP.  Signals queued during the
    SIGSTOP window (SIGALRM, SIGCHLD, SIGIO) fire on master's
    kernel stack between SIGCONT and signal_block — UML's
    hard_handler runs, the handler frame pushes onto master's
    kernel stack, and corrupts the saved-RIP slot at
    um_template_pause_enter's `ret` epilogue.  Child inherits
    that corrupted stack via CoW, hits the corruption on return.

    So even in early mode, ANY return-up-the-syscall-stack path
    needs Path A protection.  The early-fork + continue_child arm
    is therefore moot — we'd still need a clean-stack pivot, just
    one that lands at a kernel boot continuation (not userspace()).

    Removed continue_child arm from the tree (it added a cmdline
    that didn't fix what it was designed to fix).  Empirical
    finding preserved here.

    Refined direction:
      - `child_entry_early_pool_member(void)` (new entry function):
        uses Path A primitive to land on a private stack with a
        clean RBP, then constructs a fresh jmp_buf representing
        "resume at do_one_initcall after template_pause_early_init"
        and longjmps to it.  Master saves the jmp_buf via
        UML_SETJMP before entering um_template_pause_enter.
      - Per-member mm naturally arises because each child's
        kernel boot creates its own init.sh task with its own
        new mm via init_new_context (fresh stub via lazy
        start_userspace).
      - Multi-iter sustained: master loops in fork_on_resume_loop
        (no return), each child gets to boot from late_initcall
        forward — N independent VM instances.

    Status: this is the clean architectural path.  Implementation
    spans several layers (jmp_buf save + new Path A entry + UML
    boot-resume validation) and is a 2-3 day commit series.

    UPDATE 2026-05-22 (day-2 attempt 4 — pre-SIGSTOP signal block):
    Reordered one_pause_cycle to call
    `os_template_pause_signals_block_host()` BEFORE the SIGSTOP/
    SIGCONT cycle instead of after.  Made the block idempotent so
    repeated calls don't overwrite saved_sigmask.

    This hardens the master-side signal-delivery window
    (legitimately a fix at the source) — pre-SIGSTOP block was
    committed in a1d6d0020ccc.  Both selftests still PASS.

    Then re-tested continue_child + early-fork to see if the v1
    ceiling was actually closed.  Result: still fails with same
    "Segfault with no mm" at um_template_pause_enter+0xf6.

    Disassembly finding: um_template_pause_enter+0xf6 is NOT the
    function epilogue's `ret` — it's a `mov %eax, %r12d`
    instruction MID-FUNCTION (before the +0x132 epilogue at
    `add $0x118, %rsp`).  That instruction has no memory access
    and cannot fault on its own.

    Implication: the v1 ceiling is NOT saved-RIP corruption from
    signal-handler frames.  It's a SIGSEGV INJECTION from
    somewhere else mid-function — possibly seccomp filter
    inheritance, ptrace stop, or a process-wide condition the
    forked child trips on its first kernel-mode syscall return.

    Specifically: the fork() returns to the CHILD at this
    instruction; the child has inherited seccomp filter from the
    parent's seccomp backend; if the next syscall trips a
    seccomp rule the child wasn't supposed to inherit, SIGSYS
    or SIGSEGV could fire here.

    This changes the diagnostic story for state-audit/30:
    Outcome 2's "Phase 2a panic" is NOT a saved-RIP epilogue
    issue.  It's a mid-function signal injection.  The Path A
    fix (avoid the return path entirely) still works because it
    sidesteps the entire post-fork kernel return path — but the
    Path A primitive's success doesn't IMPLY the saved-RIP
    hypothesis was correct.

    continue_child arm removed (still doesn't fix the underlying
    issue).  Pre-SIGSTOP block kept as legitimate hardening.

    UPDATE 2026-05-22 (day-2 attempt 5 — per-member stub allocation):
    Implemented `um_skas_disown_inherited()` in
    arch/um/kernel/skas/mmu.c: iterates the child's CoW'd mm_list,
    forgets master's stub_pid/sock (no kill), and swaps each
    id->stack to a FRESH `__get_free_pages` allocation.  Paired
    with `um_skas_respawn_all_stubs()` to spawn a per-child stub
    via start_userspace → clone(CLONE_VM) under the CHILD's host
    process context.

    Result: start_userspace's first clone fails — the child's
    new stub still corrupts.  Investigation:

      * phys_mapping(uml_to_phys(id->stack)) returns UML's
        global physmem_fd plus an offset.
      * physmem_fd is mapped MAP_SHARED across ALL forked UML
        kernels (master, every pool-member child, every stub).
      * `__get_free_pages` in the child returns a kernel VA, but
        that VA resolves to a physmem_fd offset that's still
        shared with every other UML kernel and stub.
      * "Fresh" child-side allocation provides VA isolation but
        NOT physical isolation.  The stub mmaps physmem_fd at
        that offset and sees the same physical bytes master sees.

    ARCHITECTURAL LIMIT: UML's stub-data isolation model is
    fundamentally based on each UML kernel having its OWN
    physmem_fd.  Forked UML kernels (master + pool members) all
    share master's physmem_fd via MAP_SHARED.  Per-member stub
    isolation requires per-member physmem_fd — a refactor of
    init_new_context / arch_um_load_physmem to allow per-pool-
    member memfd backing.

    `um_skas_disown_inherited()` is preserved in the tree as
    infrastructure for that future workstream — when per-member
    physmem lands, the helper is half the wiring already done.

    state-audit/32 step 19e refined: the answer is NOT "give
    child its own stub_data" alone — it's "give child its own
    physmem_fd, which gives it its own stub_data automatically."

    UPDATE 2026-05-22 (day-2 attempt 6 — focused per-mm memfd
    backing for stub_data):

    Realized the previous "needs physmem refactor" framing was
    overly broad.  Only the stub_data page needs per-pool-member
    physical isolation, not all of physmem.  Implemented:

      * `arch/um/os-Linux/skas/process.c` — added field
        `tramp_data->stub_data_fd_override`.  When >= 0,
        userspace_tramp uses it as init_data.stub_data_fd
        instead of phys_mapping(physmem_fd) — the stub mmaps the
        override fd at offset 0.  Also added fcntl(F_SETFD, 0)
        on stub_code_fd when it differs from stub_data_fd, so
        physmem_fd survives execveat in the override path.

      * `start_userspace_fresh(struct mm_id *id)` — memfd_create
        a per-mm stub_data backing, ftruncate to
        STUB_DATA_PAGES * PAGE_SIZE, mmap MAP_SHARED into the
        calling UML kernel's address space, set mm_id->stack to
        the new VA, clone the stub with stub_data_fd_override
        set to the memfd.

      * Wire-up in child_entry_pool_member: paired with
        um_skas_disown_inherited(), DIS_R=1 (mm_id disowned),
        SUF_ENTER, TR_OV=0xe (override fd 14) all fire.
        clone() returns successfully to parent (CLONE_VFORK
        unblocks).

    Failure: stub child fails to complete its post-execveat
    handshake.  wait_stub_done_seccomp times out / mm_id->pid
    transitions to -1 via SIGCHLD path.  Stub binary's mmap of
    stub_data from the memfd should succeed (memfd is ftruncated,
    CLOEXEC cleared, fd survives execveat) but evidently does
    not.  Further diagnosis needs stub-side instrumentation
    (raw-syscall write inside stub_exe.c before/after each mmap).

    Status: infrastructure committed (06968e3f5f27), NOT wired
    into child_entry_pool_member.  Next session's first action:
    instrument stub_exe.c with exit codes 11/12/13/14 + raw-
    syscall write to a debug fd to identify which mmap fails.

    UPDATE 2026-05-22 (day-2 attempt 7 — full diagnostic cycle):
    Re-wired disown+fresh in child_entry_pool_member with
    dump_stack diagnostic in do_exit + waitpid(WNOHANG) +
    pre/post print markers + SIGCHLD unblock + flush_tlb_mm.
    Identified the actual failure mechanism:

      * `start_userspace_fresh` clones the new stub successfully.
      * `wait_stub_done_seccomp` returns (stub completes initial
        handshake — its first __NR_exit traps via seccomp SIGSYS).
      * `userspace()` enters its dispatch loop.
      * Each iteration: seccomp_vcpu_run delivers init.sh's
        userspace state to the new stub.  Stub jumps to init.sh's
        RIP.  Init.sh's userspace tries to access ANY memory page
        — SIGSEGV (the stub only has stub_code + stub_data mapped).
      * Each SIGSEGV is handled by the page-fault path which
        eventually adds the page to the stub via mm_region_added.
      * Init.sh makes very slow progress (one page per
        round-trip) and doesn't reach POST_PAUSE within 45-second
        wait windows.

    THE REAL FIX: bulk-push init.sh's existing VMAs to the new
    stub immediately after start_userspace_fresh.  flush_tlb_mm
    only marks RANGES for the sync mechanism; um_tlb_sync acts on
    PRESENT PTEs but doesn't pre-walk VMAs.

    Concrete next step:
      void um_skas_push_all_vmas_to_stub(struct mm_struct *mm) {
          for_each_vma(vmi, mm)
              um_backend_dispatch(mm_region_added, mm, &region);
      }
    Then call from child_entry_pool_member after start_userspace_fresh.

    Wire-up gated again (single-iter PASS preserved) until that
    helper lands.  Infrastructure all stays in tree.

    UPDATE 2026-05-22 (day-2 attempt 8 — um_skas_force_resync_mm):
    Implemented and committed (85227a0552d9).  Walks all VMAs in
    init.sh's mm, marks every present PTE _PAGE_NEEDSYNC, then
    triggers um_tlb_sync to push them to the fresh stub via
    mm_region_added.

    Wire-up disown+fresh+resync in child_entry_pool_member.

    Single-iter pool-member-smoke: PASS (init.sh → MEMBER_DONE).

    Sustained-smoke (N=3): iter 1 PASS (full execution to
    MEMBER_DONE), iter 2 RUNS INIT.SH but segfaults inside
    libc.so.6 at offset 0xf7490 (BUG: zap_pid_ns_processes
    panics the iter 2 child kernel).

    Diagnostic trace shows iter 2's child:
      * receives valid current->mm at child_entry_pool_member
      * fresh stub clones successfully
      * MM_NONNULL_BEFORE_RESYNC fires
      * MM_PRE_US (mm) is still valid before userspace()
      * init.sh starts executing
      * Eventually segfaults at libc address — one specific
        page not properly mapped/populated

    iter 3+ has mm=NULL at child_entry — master's state was
    corrupted by iter 2's panic propagation.

    Next bounded step: identify what's at libc offset 0xf7490
    and why iter 2 segfaults there but iter 1 doesn't.  Possible
    causes:
      * pte_mkneedsync of an iter-1 child's pgtable CoW page
        leaked back to master somehow
      * iter 2's resync missing a specific VMA range
      * libc's TLS / GOT setup needs special handling

    Single-iter pool-member-smoke STAYS PASS.

    UPDATE 2026-05-22 (day-2 attempt 9 — state restoration):
    Identified that libc+0xf7490 is `hlt` inside `_exit@@GLIBC_2.2.5`
    — the "trap if exit_group syscall returned" defensive instruction
    after the exit_group syscall.  For it to execute, exit_group
    returned to userspace (didn't kill the process).

    Root-cause diagnostic chain:
      1. Kernel slab allocations (task_struct, signal_struct,
         mm_struct) live in UML's MAP_SHARED physmem_fd, NOT CoW'd
         across forked UML kernels.
      2. When iteration N's child runs init.sh's `exit 0`, its
         do_exit path mutates these fields (PF_EXITING set,
         signal->live decremented to 0, group_exit_code set, etc.)
      3. Those writes propagate back to master and to iteration
         N+1's child.
      4. Iter 2's do_exit sees signal->live = 0, decrements to -1,
         group_dead = false → skips is_global_init panic →
         find_child_reaper → zap_pid_ns_processes → BUG.

    Fix committed (aabfd4082413): reset task/signal/mm fields in
    child_entry_pool_member entry.  task->flags &= ~PF_EXITING, etc.
    signal->live = 1, mm refcounts = 2.

    Result: iter 2's do_exit panic shape now MATCHES iter 1's
    "Attempted to kill init" — kernel-state consistency achieved
    across iterations.

    REMAINING ARCHITECTURAL LIMIT:

    Iter 2's bash STILL segfaults at libc hlt before reaching
    MEMBER_DONE.  Userspace memory (bash's heap, stack, libc
    static state) ALSO lives in MAP_SHARED physmem_fd.  Iter 1's
    userspace writes (modifying bash's heap during execution)
    propagate to master and iter 2.

    To make sustained dispatch fully work, the iteration must
    EITHER:
      * use per-pool-member physmem_fd (wholesale UML refactor)
      * snapshot + restore bash's userspace pages between iters
        (large kernel addition: walk init.sh's mm VMAs and
        copy each page to a snapshot store before fork; restore
        post-iter-N before iter-N+1 forks)

    Either path is significantly larger than the in-scope Path A
    integration.  The work landed here (disown + fresh stub +
    force resync + kernel state reset) is the maximum that's
    achievable without per-member physmem isolation.

    Single-iter pool-member-smoke STAYS PASS.  Sustained-smoke
    iter 1 PASS, iter 2+ documented XFAIL on userspace state
    sharing.

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
