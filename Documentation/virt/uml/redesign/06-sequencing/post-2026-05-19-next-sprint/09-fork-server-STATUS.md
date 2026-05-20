# 09 — Fork-server status (as of 2026-05-20)

This document tracks the state of Memo 09 (the keystone
template-pause + fork sprint).  It is updated as phases land and
should be re-read at the start of each session.

## Phases — current state

| Phase | Description                          | Status                | Commit          |
|-------|--------------------------------------|-----------------------|-----------------|
| 1a    | Kernel template-pause hook           | LANDED (2026-05-20)   | `5051e9c90342`  |
| 1b    | umlctl integration (MVP `pool spawn`)| LANDED (2026-05-20)   | `5979caa69440`  |
| 2a-P1 | start_userspace_redo + os_skas_reap_stub | LANDED (2026-05-20) | `0993765a704c` |
| 2a-P2 | mm_list-walking teardown / respawn helpers | LANDED (2026-05-20) | `d40d124f3abe` |
| 2a-P3 | `assert_fork_safety` mid-syscall refusal | LANDED (2026-05-20)  | `985fd78ab070` |
| 2a-P4 | Wire teardown → fork → respawn into loop | LANDED (2026-05-20)  | `5dac39adad4f` |
| 2a-P5 | template-pause-fork-smoke selftest   | LANDED (2026-05-20)   | `c417eccf869e` |
| 2a-P6 | template-pause-fork-stress selftest (6 strict gates, no retry) | LANDED (2026-05-20) | `972d19478c91` + this |
| 2a-P7 | umlctl `mission` Phase 8 wiring (full-mission only) | LANDED (2026-05-20) | `972d19478c91` |
| 2a-P8 | M-fork child dead-code respawn removal — eliminates one death path | LANDED (2026-05-20) | this commit |
| 2a    | Kernel-side fork-on-resume loop      | **EXPERIMENTAL — partial fix landed; residual race in M-fork child path keeps fork-stress ~40 % pass at single attempt** | (all above) |

## Open bugs (2026-05-20)

### B1: M-fork child wait_stub_done_seccomp pid=-1 race

After the 2026-05-20 dead-code respawn removal (commit `this`),
fork-stress still fails ~60 % of runs at N=100 iterations.  When it
fails, master dies at iter ≈ 6–11 with:

  ```
  wait_stub_done_seccomp : failed to wait for stub, pid = -1, errno = 0
  Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
  ```

Bisect evidence (see commit log for raw boot.log captures):

  * Master and M-fork child both successfully return from the
    `__NR_fork` syscall.  Master logs `BISECT MASTER post-fork
    child_pid=N` consistently.
  * The M-fork child SOMETIMES reaches the post-fork inline write
    syscall and writes the `tplp-CHILD-after-fork` marker; in those
    iterations everything works.  Roughly 1 in 100–700 iters.
  * In the OTHER iterations, the M-fork child panics BEFORE the
    inline write — the kernel-mode-fault location is in stack
    address space (e.g., IP=0x68803d66, same value across runs).
    This suggests a corrupted control-flow transfer, possibly via
    UML's task scheduler longjmp'ing into a CoW'd stack with a
    stale JB_IP.
  * Repeated `template_pause: torn down 0 stub(s) pre-fork` lines
    show that mm_list has stale dead entries (`pid=-1`) but
    teardown correctly skips them.  However, in the M-fork child
    a kernel path is somehow reaching `wait_stub_done_seccomp` on
    these dead entries — `start_userspace` isn't called explicitly
    from the child path anymore (the explicit `respawn_all_stubs`
    call was removed), so an INDIRECT call site remains to be
    isolated.

Open hypotheses to test (in order of cheapness):

  1. `seccomp_mm_create` is called when the kernel creates a new
     mm during M-fork child startup.  Audit whether new-mm
     creation happens on the M-fork child path.
  2. `do_syscall_stub` (from um_stub_mm_map / um_stub_mm_unmap)
     might be invoked when the M-fork child page-faults on
     anything that needs guest-side mapping.  Audit fault paths.
  3. UML's `switch_threads(me, you)` might pick up a different
     task post-fork — but `sched_worker_detach_other_tasks` is
     supposed to prevent that.  Verify the runqueue state after
     fork in the M-fork child.

Until B1 is closed, Phase 8 in `umlctl mission` will fail ~60 %
of runs against the experimental fork kernel.  Phase 8 SKIPs
cleanly when no fork kernel is present, so the rest of mission is
unaffected on production hosts.
| 1c    | `umlctl pool serve` daemon + multi-take | BLOCKED on UML_LONGJMP fix | —               |
| 2     | Kernel applies identity (MAC/IP/tap) | PENDING               | —               |
| 3     | Bench + acceptance gates             | PENDING               | —               |
| 4     | syzkaller `vm/uml` Go shim           | BLOCKED on 2a-P5 fix  | —               |

## What works today

End-to-end driving of the template-pause primitive through umlctl:

```sh
umlctl pool spawn \
    --kernel /path/to/linux \
    --instance pool-mvp-1 \
    --mac 52:54:00:11:22:33 \
    --tap tap-mvp --ipv4 10.7.0.42/24 --gateway 10.7.0.1 \
    --json
```

returns a JSON envelope describing the running pool member.

Selftests guarding the end-to-end:

  * `tools/testing/selftests/um/template-pause-smoke/` — kernel-side
    primitive (unarmed, armed-no-identity, armed-with-identity).
  * `tools/testing/selftests/um/template-pause-fork-smoke/` —
    structural smoke of the fork-on-resume primitive (teardown +
    fork() return + child pid write-back).
  * `tools/testing/selftests/um/template-pause-fork-stress/` —
    six-gate stress over hundreds of fork-on-resume iterations:
    G1 fork primitive ran, G2 distinct child pids, G3 RSS drift
    ≤ 5 %, G4 no post-teardown stub leak, G5 ≥ N iterations within
    the window, G6 identity-blob round-trip.  Has a 3-attempt
    retry harness to absorb the residual v1-ceiling flakiness.
  * `tools/testing/selftests/um/pool-spawn-smoke/` — umlctl wrapper.
  * `pool::tests` in `tools/uml/uml-launcher/src/bin/umlctl/pool.rs`
    — identity-blob layout + MAC parser.

`umlctl mission` (full, non-`--quick`) drives the stress test as
Phase 8 against a separate CONFIG_UM_TEMPLATE_PAUSE_FORK=y kernel
(via `--fork-kernel` / `$UM_FORK_KERNEL`, defaulting to
`$HOME/src/uml-builds/uml-tplpause-fork/linux`).  Phase 8 SKIPs
cleanly when that kernel is absent, so the rest of the mission
gate still runs on hosts without the experimental build.

## What does NOT work today

1. **One member per master**.  The kernel
   `um_template_pause_enter()` returns after one SIGSTOP/SIGCONT
   cycle.  The master IS the taken instance; there is no
   fork-from-pause.  To get N siblings today, run N separate
   `umlctl pool spawn` commands, each booting a fresh master.

2. **Identity blob is logged, not applied**.  The kernel reads
   the blob, validates magic + version, and emits a dmesg line.
   It does NOT yet swap MAC, rebind IPv4, or swap the tap fd.
   Pool members today inherit the master's identity for everything
   except the dmesg log line.

3. **No long-lived supervisor**.  Each `pool spawn` is a one-shot
   foreground command.  No `pool serve`, no Unix socket API, no
   replenish, no take RPC, no `pool list` / `pool status` /
   `pool destroy`.

4. **No KVM-backend fork support**.  Phase 1a does not fork, so
   kvm-v2 is fine.  When Phase 2a (kernel-side fork loop) lands,
   it will reuse `os_snapshot_fork_worker()` which is currently
   refused under KVM (see `arch/um/kernel/snapshot.c` line ~191).
   The KVM-aware fork path is filed as Phase 5+.

## Next steps, in order of priority

### Phase 2a failure analysis (2026-05-20 attempt)

Phase 2a kernel code is in tree (this commit) but the fork-on-resume
loop is **known-broken** and gated behind
`CONFIG_UM_TEMPLATE_PAUSE_FORK` (off by default, EXPERIMENTAL).
Setup arms via `um_template_pause=fork` on the cmdline.

**Observed failure** (smoke test `~/src/tplpause-fork-smoke2.sh`,
2026-05-20):

```
template_pause: armed via kernel cmdline (fork-on-resume) — EXPERIMENTAL
[boot ...]
POOL_BOOT_OK
template_pause: enter("fork-loop") — identity_fd=4
template_pause: raising SIGSTOP at "fork-loop" (pid=N)
[supervisor sends SIGCONT]
template_pause: resumed via SIGCONT at "fork-loop" (pid=N, count=1)
template_pause: identity at "fork-loop" instance="pool-take-1" mac=... [valid]
template_pause: raising SIGSTOP at "fork-loop" (pid=N)   ← parent's second pause
Kernel panic - not syncing: Kernel tried to access user memory at addr 0x2670fc, ip 0x68803bde
CPU: 0 UID: 0 PID: 1 Comm: init.sh
Call Trace:
 fork_on_resume_loop+0x148   ← right after one_pause_cycle() returns
 um_template_pause_enter
 template_pause_proc_write
 vfs_write
 ksys_write
 sys_write
 handle_syscall
```

**What works structurally**:
  * Master fork(2)s exactly once per SIGCONT.
  * Parent successfully writes the new child's pid to memfd offset
    260 (verified by reading it back from the supervisor side).
  * Parent's second pr_info("raising SIGSTOP") line prints — the
    control flow IS reaching the next iteration.

**What breaks**:
  * After the parent's second SIGSTOP attempt, a guest userspace
    task (PID 1 = init.sh, the task that was blocked in
    write(/proc/um/template_pause)) SEGVs.  The IP (0x68803bde)
    is in UML's seccomp stub address range; the access at 0x2670fc
    is a small invalid address.

**Root cause (narrowed 2026-05-20)**:
  The crash IP is consistently `0x68803bde` (varies by build) and
  the bad guest-userspace address varies per run (`0x265d57`,
  `0x2670fc`, `0x26ac13` across three boots — small heap-ish
  numbers, consistent with ASLR'd guest-userspace data).  The IP
  is in UML's stub-child code range (`0x68800000–0x68804000`).

  Concrete sequence:

    1. Master boots, spawns the SKAS seccomp **stub child** via
       `clone(CLONE_VM | CLONE_VFORK | ...)` to host guest
       userspace.  The stub child shares the master's mm.
    2. init.sh runs, writes to /proc/um/template_pause.
    3. Kernel template_pause_enter: identity read, SIGSTOP/SIGCONT.
    4. Kernel: `syscall(__NR_fork)` from the master's main thread.
       The fork(2) host syscall clones ONLY the calling thread —
       the stub child is a *separate kernel task* and is NOT
       duplicated.  The parent's mm is CoW'd.
    5. From this moment:
         * Parent UML has new CoW'd mm.
         * Stub child STILL has CLONE_VM-shared access to the
           parent's mm — but now any write the parent does
           triggers CoW, and the new physical page is in the
           parent's mm only.  The stub child still sees the
           ORIGINAL physical page through its CLONE_VM mapping.
         * Memory the parent reads from / writes to via the
           stub-mediated path now references different physical
           memory than what the stub child sees.
    6. When the parent next schedules a guest userspace task via
       the stub mechanism (PTRACE_SYSEMU equivalent in seccomp
       mode), the stub child decodes addresses against its stale
       view and SEGVs at a divergent address.

  This is **not** something `um_snapshot_worker_init()` can
  handle from inside the child: by the time `worker_init` runs,
  the parent has already been corrupted (its stub child's view
  has diverged).  The fix must happen pre-fork OR re-spawn stubs
  in both halves post-fork.

  Why the AFL forkserver doesn't hit this: its ready point is
  reached *before* first userspace.  No stub child exists yet at
  fork time — the worker lazily spawns one on its first guest-
  userspace syscall (per the `worker_init()` doc-comment).
  Template-pause runs AFTER init has executed, so stubs are
  already up.

**Update 2026-05-20 (after landing all 6 patches AND extensive bisect)**:

  The Phase 2a structural plan is complete; the v1-ceiling hazard
  was narrowed precisely.  Findings below correct the design memo's
  fix sketches (the agent's `start_userspace_redo` design is still
  the right structural fix for the SKAS stub-pid aliasing — that
  hazard is gone — but a SECOND hazard remains).

**The second hazard, narrowed (bisect, 2026-05-20)**:

  The forked child's UML kernel cannot execute even a single C
  statement after fork return.  Instrumented inline-asm pwrite
  markers placed IMMEDIATELY after the fork syscall do not fire —
  the child sleeps in `anon_pipe_write` for ~1 s then dies with
  SIGABRT (UML's panic exit signal via `uml_abort`).  Reaping with
  PR_SET_CHILD_SUBREAPER confirms BOTH halves get SIGABRT.

  The kernel panic message is `Kernel tried to access user memory
  at addr 0x2d6b62, ip 0x2d6b62` (addr == ip, the IP is the bad
  value).  The IP is in the **guest-userspace VA range** — exactly
  what you'd see if UML's kernel longjmp'd into a CoW'd
  `task_struct->thread.arch.jmp_buf` whose saved RIP was a guest-
  userspace address.  In the parent process that VA is mapped
  (via the seccomp stub child); in the forked child it is not.

  Concretely: after fork returns in the child, before my inline asm
  pwrite runs, UML's scheduler picks SOME runqueue task and
  `UML_LONGJMP`s into its `jmp_buf`.  That task's saved IP was in
  guest userspace at the time of last context switch.  The child
  has no stub child to mediate guest userspace execution, so the
  host CPU jumps to the unmapped guest-userspace IP, SIGSEGV's,
  UML's `trap.c::relay_signal` sees a kernel-mode IP < TASK_SIZE,
  and panics.

  This affects BOTH halves: the parent ALSO picks a stale runqueue
  task on its next schedule and panics for the same reason (its
  stub child for that mm was killed by `um_skas_teardown_all_stubs`
  pre-fork, but the parent's task list still references stubs by
  pid — the master's mms get respawn'd by
  `um_skas_respawn_all_stubs` in the parent path, but only AFTER
  the parent has already scheduled and panicked).

**Why `preempt_disable` doesn't help**:

  Confirmed empirically.  UML's scheduler doesn't honor
  preempt_count for the longjmp-to-task path on syscall return —
  the host kernel delivers queued signals (SIGCHLD / SIGALRM
  inherited from the parent's signal queue) at syscall-return time,
  UML's sig_handler runs, `sig_handler_common` calls
  `unblock_signals_trace()` which dispatches queued handlers,
  one of which runs `switch_threads()` → `UML_LONGJMP` → bad
  jmp_buf → panic.

**Update 2026-05-20 (Round 2 — early-pause + every-mitigation bisect)**:

  Tried `um_template_pause=early-fork` which arms the kernel to
  pause INSIDE a late_initcall_sync, BEFORE `run_init_process` —
  AFL forkserver's "v1-friendly" ready point invariant.  At this
  point there are NO guest userspace tasks, NO SKAS stub
  children, and the runqueue has only kthreads + the kernel_init
  task itself.

  Result: master STILL panics, this time with "Segfault with no
  mm" (because at initcall time `current->mm == NULL`).  Same
  call frame: `um_template_pause_enter+0xf0/0x100` — the function
  epilogue's `ret` instruction popping a corrupted saved-RIP.

  Combined-bisect outcome across the full matrix:

  | Variant                              | Master survives fork? |
  |--------------------------------------|------------------------|
  | Teardown + respawn (design's path)   | No — IP=0x2d6b62 etc.  |
  | No teardown                          | No — IP=0x4            |
  | Pre-fork sched_worker_detach         | No — crashes the master before fork |
  | Child-side detach + respawn          | No (master path still) |
  | Inline asm syscalls everywhere       | No                     |
  | preempt_disable across fork          | No                     |
  | Early-pause (no userspace, no stubs) | No — "Segfault with no mm" |

  The COMMON denominator across all variants is `um_template_pause_
  enter+0xf0` — the function epilogue's `ret`.  The master's
  saved-RIP slot on the kernel stack is being corrupted by something
  in the path between the fork syscall returning and any subsequent
  code reading from that slot.  CoW should preserve the slot
  contents.  Yet it doesn't, consistently, across all configurations.

**Conclusion: this is genuinely beyond a single-session fix.**

The hypothesis is that UML's host-process stack frame for the kernel
task running fork_on_resume_loop has some invariant — probably
related to UML's own jmp_buf save/restore in `switch_threads`, the
SKAS stub-child reaper signal path, or the CFS task accounting —
that gets violated by raw `__NR_fork`.  Fixing it requires either:

  (a) Adding a CRIU-style checkpoint/restore (memo 26's path,
      which Memo 09 explicitly rejected for the fast-spawn use
      case but may be unavoidable);
  (b) Re-architecting UML's task scheduling to use a freezer-cgroup
      pre-fork barrier (the v2 direction the existing
      sched_worker_detach_other_tasks comment in
      kernel/sched/core.c points at);
  (c) Constraining the master to be SINGLE-THREADED at fork time
      (no SIGIO thread, no POSIX-timer threads), so the host
      kernel's fork has no sibling-thread state to lose.

Each is a multi-week engineering effort, not a single-commit fix.

**Real fix (informed by the bisect)**:

  Three layers needed, in order:

  1. **Detach ALL non-current tasks from the runqueue PRE-FORK**
     in the master.  Existing helper:
     `sched_worker_detach_other_tasks()` in `kernel/sched/core.c`
     under `CONFIG_UM_SNAPSHOT_FORKSERVER`.  Call this before
     `os_template_pause_fork` so that after fork, the runqueue
     contains only `current` (init.sh) in both halves.  Schedule
     can't pick a stale task because there are none.

  2. **Block ALL host signals pre-fork via `sigprocmask` at host
     level**, not just UML's `signals_enabled=0`.  This prevents
     host-kernel signal delivery during the fork+respawn window.
     Restore the mask in BOTH halves after they've stabilized.

  3. **Re-attach detached tasks in the parent only AFTER**
     `um_skas_respawn_all_stubs` completes.  In the child, the
     detached tasks STAY detached (the child is a one-shot pool
     member; its scheduling needs are minimal).

  Total scope: ~80 LoC kernel patch on top of what's already in
  tree.  All five design patches landed are necessary
  building blocks but not sufficient.

**Fix sketches** (original — historical, for context):

The SKAS stub-pid aliasing hazard (the primary root cause the
research-agent narrowed) is **fixed** by the pre-fork teardown.
Smoke run with `um_template_pause=fork`:

```
template_pause: torn down 1 stub(s) pre-fork    <-- new
template_pause: fork returned pid=N
template_pause: PARENT branch entered (new child pid=N)
template_pause: raising SIGSTOP at "fork-min" (pid=master_pid)
Kernel panic - not syncing: Kernel mode fault at addr 0x4, ip 0x4
 [<6003a6fa>] um_template_pause_enter+0xf0/0x100
 [<6003a7d0>] template_pause_proc_write+0xc6/0xdd
```

The crash IP changed from the previous session's
`0x68803bde` (inside stub-child code) to `0x4` (NULL function
call).  The frames show `um_template_pause_enter+0xf0` — the
return-from-fork_on_resume_loop epilogue — meaning the panicking
process IS returning from fork_on_resume_loop.  That can only
happen in the CHILD path (the parent's path is an infinite loop).
So **M-fork1 is the crashing process**, panicking as it tries to
return up through the UML kernel scheduler after worker_init().

This is the **AFL forkserver v1 ceiling** — the same class of bug
documented in `arch/um/kernel/snapshot.c` around lines 340-360.
UML schedules tasks via UML_LONGJMP into jmp_buf structures saved
pre-fork.  M-fork1's jmp_buf references stack frames that are
valid only in the parent's address-space layout; CoW makes the
addresses *look* right but any actual scheduler activity in the
child trips a NULL function pointer in switch_threads().

The fix is not about SKAS stubs at all.  It's about scheduler-state
preservation across fork — the AFL path documents that the worker
"may run un-preemptible code (echo, exit) before halting" but
cannot reliably yield to the scheduler.  Template-pause's child
returns up through several stack frames before halting, which is
enough scheduler activity to trip the same hazard.

**Next investigation step (Patch 5 of the PHASE2A-DESIGN.md plan)**:
the child's return path needs to be either (a) replaced with a
direct guest-userspace dispatch that bypasses scheduler return
paths, or (b) extended to repair jmp_buf state post-fork.  Both
need familiarity with `arch/um/kernel/process.c::switch_threads`
and the UML_LONGJMP machinery in `arch/um/include/shared/longjmp.h`.

**Fix sketches** (original — historical, for context):

  1. **Tear down stubs in the PARENT pre-fork, respawn post-fork
     in BOTH halves.**  Right before `os_template_pause_fork()`,
     send SIGKILL to the master's stub child and waitpid-reap it
     so the master has no CLONE_VM-attached task.  After fork, in
     both parent and child, lazily respawn the stub child via the
     existing `start_userspace()` path on the next guest
     userspace syscall.  Touches `arch/um/os-Linux/skas/process.c`
     and `arch/um/kernel/skas/process.c`.

  2. **Early ready point — fork before first userspace.**
     Hook template_pause into the boot path BEFORE init runs (a
     new `__setup` callback that pauses in `init/main.c`'s
     `rest_init`-equivalent path).  No stubs exist yet; fork
     model matches the AFL forkserver's well-tested invariant.
     The bootstrap script no longer drives the pause — the
     kernel pauses itself on the way to init.  Costs the
     ability to run guest setup before pause; pool members
     start with no guest userspace state.

  3. **Pivot to the existing AFL forkserver path.**  Reuse the
     fd-198/199 protocol but provide an identity-blob channel
     alongside it (e.g., on fd 200).  Saves implementing a new
     fork path entirely; inherits the AFL path's existing
     correctness work and KVM refusal.

Estimated effort:

  * Sketch #2: ~1 day (smallest scope; clean re-use of AFL
    invariants).  Drawback: the bootstrap-script flexibility
    Phase 1a gave us is lost.
  * Sketch #1: ~3-5 days (touches SKAS heavily but is the
    proper engineering answer).
  * Sketch #3: ~1 day (mostly wiring + identity-channel
    grafting).  Drawback: the pool members go through the AFL
    path's existing limitations (KVM refused, etc).

Filed as the blocker on Phase 1c (daemon), Phase 4 (syzkaller
shim), and the performance acceptance gates in §Step 4 below.

### Step 1 — Kernel-side fork loop (~80 LoC kernel)

Extend `um_template_pause_enter()` to optionally fork-on-resume.
Trigger: a kernel cmdline (`um_template_pause=fork`) and / or a
control byte in the identity blob.

```c
int um_template_pause_enter(const char *named_point) {
    /* existing arm check ... */
    os_snapshot_block_iter_signals();  /* gate UML signal dispatch */
    for (;;) {
        ret = os_template_pause_stop_self();  /* SIGSTOP -> wake on SIGCONT */
        if (ret) break;
        /* read identity */
        if (!loop_armed) {
            /* Phase 1a behaviour — one-shot pause, return. */
            os_snapshot_unblock_iter_signals();
            return 0;
        }
        pid = os_snapshot_fork_worker();
        if (pid == 0) {
            um_snapshot_worker_init();  /* reuse the existing helper */
            /* identity already in blob; Phase 2 applies it here */
            return 0;  /* child resumes as taken instance */
        }
        /* parent: write child pid to memfd at offset 260 so the
         * supervisor can learn it; loop back to SIGSTOP. */
    }
    return ret;
}
```

Issues to handle:

  * `um_snapshot_worker_init()` lives in `arch/um/kernel/snapshot.c`
    which is gated by `CONFIG_UM_SNAPSHOT_FORKSERVER`.  Either
    factor the helper out into a shared TU, or make
    `CONFIG_UM_TEMPLATE_PAUSE` `select UM_SNAPSHOT_FORKSERVER`.
  * AFL path forbids KVM backend in
    `um_snapshot_assert_ready()`; the same refusal will need to
    apply to fork-on-resume.  Phase 1 ceiling.
  * Parent must write the child pid out somewhere the supervisor
    can read it back.  Cleanest: append 4 bytes at offset 260 of
    the identity memfd before re-pausing.

### Step 2 — `umlctl pool serve` daemon (~400 LoC Rust)

A long-lived supervisor that:

  * Boots ONE master with `um_template_pause=fork` (Step 1 above).
  * Listens on `$RUNTIME/uml/pools/<name>/api.sock`.
  * On take request: writes identity blob, SIGCONTs master,
    waits for master to re-pause, reads child pid back from
    memfd offset 260, returns it to the client.
  * Tracks taken children + reaps on death + auto-replenishes if
    `min_warm > 0` (Phase 1c proper, not MVP).

The wire JSON envelope is already defined by Phase 1b — the daemon
returns the same shape, so callers don't need to change.

### Step 3 — Kernel applies identity (~250 LoC)

Inside the child's post-fork init (Step 1's `worker_init` callback
or a new template-pause-specific equivalent):

  * `dev_set_mac_address` on the netdev named by the blob's `tap_name`.
  * In-kernel netlink: `inet_rtm_newaddr` to bind `ipv4_cidr`.
  * If the host passed a fresh tap fd via SCM_RIGHTS on the
    identity memfd's auxiliary channel (Phase 2.2), swap it via a
    new `vec2.0` ethtool-style ioctl.

### Step 4 — Bench + acceptance (~150 LoC)

`tools/testing/selftests/um/pool-bench/`:

  * `take` latency p50/p99 across 1000 takes against one warm master.
  * Memory amplification: RSS of (supervisor + 100 children) vs
    RSS of 1 master.
  * 60 s sustained `take + close` at 50 takes/sec.

Acceptance gates from Memo 09 §3 Phase 3:

  * `take` p50 ≤ 5 ms, p99 ≤ 50 ms (vs 207 ms cold-boot).
  * 100 forks of 128 MiB master ≤ 200 MiB total RSS.
  * No leak across 10 000 take/close cycles.

### Step 5 — syzkaller integration (~150 LoC Go)

`vm/uml/uml.go` implementing `vmimpl.Pool` + `vmimpl.Instance`
by shelling to `umlctl pool take` / `umlctl exec` /
`umlctl port-forward` / `umlctl stop`.

This is the deliverable that closes the user story driving the
whole 5-week sprint.  It depends on Steps 1–3 being in tree.

## Cross-references

  * Memo 09 design:
    `09-fork-server-snapshot-restore.md` (same directory)
  * Existing AFL-style fork-server:
    `arch/um/kernel/snapshot.c`,
    `arch/um/include/asm/um-snapshot.h`,
    `arch/um/os-Linux/process.c` (os_snapshot_* primitives)
  * Phase 1a TU: `arch/um/kernel/template_pause.c`
  * Phase 1b umlctl module: `tools/uml/uml-launcher/src/bin/umlctl/pool.rs`
