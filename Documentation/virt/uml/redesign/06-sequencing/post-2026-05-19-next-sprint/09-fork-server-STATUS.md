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
| 2a-P8 | Drop child-path respawn + drop SUCCESS-marker pwrite64 | LANDED (2026-05-20) | `5a65fdfe8010` |
| 2a-P9 | Pre-SIGKILL M-fork child; tighten gates to G7 (zero panics) + G8 (side-channel verify) | LANDED (2026-05-20) | `870c368726fd` |
| 2a-P10 | Root-cause B1 — captured M-fork child fault state via custom SIGSEGV handler | LANDED (2026-05-20) | `9c6c4948dc70` |
| 2a-P11 | **Fix B1**: __NR_clone with private MAP_PRIVATE child stack, default ON | LANDED (2026-05-20) | `d057cf28f482` |
| 2a    | Kernel-side fork-on-resume loop      | **PRODUCTION**: fork-stress 100/100 PASS at N=100 strict gates, 10/10 at N=1000, 10/10 under stress-ng full-load.  ZERO panics across 120 runs. Bug B1 ROOT-CAUSED + FIXED via private-stack clone(). | (all above) |

## Test reliability (2026-05-20, post-P9)

```
N=100  / SECS=10:   100/100 PASS  (idle host)
N=1000 / SECS=30:   10/10  PASS
N=5000 / SECS=160:  1/1    PASS   (master sustains 5714 iters)
N=100  + stress-ng --cpu $(nproc)/2:  10/10 PASS  (half-load)
N=100  + stress-ng --cpu $(nproc):    10/10 PASS  (full-load)
```

All gates strict: G1 master alive thru N iters, G2 distinct pids,
G3 RSS drift ≤ 5 % with ≥ 6 valid samples, G4 zero post-teardown
orphans, G5 ≥ N iters, G5b median ≤ 50 ms, G6 100 % blob byte-
match, G7 zero kernel panics, G8 ≥ 5 % /proc capture rate.

## Bug B1 — ROOT CAUSED AND FIXED (2026-05-20)

After the P9 SIGKILL workaround (committed first as a stopgap) and
P10 diagnostic capture, the underlying race was fully isolated.
P11 ships the actual fix: __NR_clone with a private MAP_PRIVATE
child stack.  Fork-stress now passes 100/100 strict gates without
any SIGKILL hack — master can let the child run and the child
terminates cleanly via inline __NR_exit_group(0) on its private
stack.

The SIGKILL fork()-based path is retained as a fallback (boot
with `um_template_pause_private_stack=0` to enable it) for
regression testing.

## ROOT CAUSE (isolated via MFC diagnostic mode, validated via P11)

UML's physmem is mapped `MAP_SHARED` (see arch/um/os-Linux/process.c
::os_map_memory: `flags = MAP_SHARED | MAP_FIXED | MAP_POPULATE`).
UML kernel allocations — including kernel stacks via the buddy
allocator → page allocator → physmem — live in this shared
mapping.  Master and M-fork child therefore share the same
PHYSICAL pages for their kernel stacks even though they're
separate host processes.

After fork:
  1. Master's `ret` in os_template_pause_fork_inner pops the
     CORRECT return address from stack (CoW behavior is fine for
     the immediate post-syscall path before master writes).
  2. Master continues parent path, calling printk, write_child_pid,
     and one_pause_cycle.  Each call PUSHES to the (shared) stack.
  3. M-fork child, concurrently, is at its own `ret` in inner.
     M-fork child reads the SAME stack slot — but by now master
     has overwritten it (because MAP_SHARED, not CoW).
  4. The popped value is whatever master most recently pushed:
     typically a callee-saved register (R15) holding the address
     of a static flag like `template_pause_mfc_diag_armed_flag`
     (0x6072c02c) or other data pointer.
  5. M-fork child jumps to that address, which is in .bss (not
     executable), and SIGSEGVs.
  6. UML's `sig_handler_common` sets `regs->is_user = 0`
     (default), reads the user-range fault IP, calls segv() which
     panics via `arch/um/kernel/trap.c::segv()` line 372
     ("Kernel tried to access user memory at addr 0xX, ip 0xX").

The 2-byte-per-iter address walk we observed earlier is master's
R15 (or similar reg) holding successively-different stack addresses
each iteration — the iteration counter accumulating in the
allocator changes which slot master writes.

Diagnostic mode (compiled in but disarmed): boot with
`um_template_pause_mfc_diag=1` to skip the SIGKILL and install
`mfc_diag_segv_handler` for SIGSEGV/SIGBUS/SIGILL/SIGFPE in master
pre-fork.  M-fork child inherits the handler via fork's sigaction
table inheritance.  Faults are captured as `MFC_FAULT sig=N
addr=0xX rip=0xX cr2=0xX err=0xX rsp=0xX rbp=0xX r15=0xX
stack[0..5]=...` lines in the boot log.

## Production contract (Phase 2a)

`um_template_pause=fork` arms fork-on-resume.  Default behaviour
(no extra cmdline flag needed):

  * Master uses `__NR_clone` with a private MAP_PRIVATE child
    stack via `os_template_pause_fork_clone()`.
  * The child runs on the private stack, does inline
    `__NR_exit_group(0)`, terminates cleanly.
  * No SIGKILL needed.  No panics.

Phase 2 (identity re-plumbing) builds on this: the child can now
safely execute inline-asm syscalls (dev_set_mac_address,
inet_rtm_newaddr, tap fd swap via SCM_RIGHTS, etc.) without
re-triggering B1, because its stack is private.

## Phase 2 implementation notes

Current `os_template_pause_fork_clone()` leaks the 8 KiB private
stack VMA on every iteration in master.  Acceptable for Phase 2a
(bounded leak rate, master is short-lived).  Phase 2 should cache
and reuse a single child_stack region across iterations to
eliminate the per-iter mmap overhead.

The legacy fork()+SIGKILL path is retained behind
`um_template_pause_private_stack=0` for regression testing
only.  The private-stack clone() path is the production default
and what Phase 2 builds on.

The child stack is cached (file-static in
`os_template_pause_fork_clone`), so per-iter mmap overhead is
paid only on the first call.

For Phase 2 work that needs to extend the child path with real
syscalls (identity re-plumbing — dev_set_mac_address,
inet_rtm_newaddr, tap fd swap), enable
`CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG=y` and boot with
`um_template_pause_mfc_diag=1` to install the SIGSEGV/SIGBUS/
SIGILL/SIGFPE handler that dumps any child-side faults via raw
write to fd 1.
| 1c    | `umlctl pool serve` daemon + multi-take | **LANDED (2026-05-21)** — Unix-socket RPC over `$XDG_RUNTIME_DIR/uml/pools/<name>/api.sock` (take/list/status/destroy/shutdown); 98/98 cargo tests; pool-serve-smoke selftest | `5576cdf21084` + `cda39d83ab29` (adaptive `wait_for_stop` poll cadence) |
| 2     | Kernel applies identity (MAC/IP/tap) | **LANDED (2026-05-21)**: MAC + IPv4 CIDR + IPv4 gateway applied to the in-guest netdev each take.  KUnit 13/13 PASS; template-pause-smoke case 4 PASS (vec0 MAC = 52:54:00:de:ad:be, inet 192.168.7.42/24 verified via `ip addr show`); fork-stress 10/10 PASS at N=100 strict gates.  Tap fd swap deferred (see "What does NOT work today"). | `e751762a8018` |
| 3     | Bench + acceptance gates             | **LANDED (2026-05-21)** — pool-bench 4/5 gates PASS on local hardware after `cda39d83ab29`'s adaptive poll fix: p99 14ms, RSS 132MiB, lifecycle drift 0.05%, throughput 3000/3000.  `take.p50` re-measure pending (initial 12.0ms with 10ms poll quantum; expected ≤3ms with 250µs initial quantum). | `cda39d83ab29` |
| 4     | syzkaller `vm/uml` Go shim           | **LANDED (2026-05-21)** — `umlctl pool take` / `pool status` / `pool destroy --name` daemon-routed client verbs; `umlctl exec --json` with NDJSON `start`/`stdout`/`stderr`/`console`/`exit` frames (spec memo 11 §3.4); `umlctl port-forward --json` TAP-direct envelope; daemon `exec` RPC + per-instance mconsole path synthesis (spec §6 q2); reference Go shim vendored at `tools/uml/syzkaller-vm-shim/uml.go` (drop-in for upstream syzkaller `vm/uml/uml.go`); 126/126 cargo tests; pool-exec-smoke + pool-port-forward-smoke selftests PASS end-to-end. | (this commit) |

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

### Phase 2 — identity application (2026-05-21)

Phase 2 lives in `arch/um/kernel/template_pause_identity.c`.  On
each take the master:

  * reads the per-take identity blob from the supervisor's memfd,
  * resolves the in-guest netdev by scanning for the first
    registered interface whose name starts with `vec` (UML vector
    driver) or `eth` (legacy `uml_net` driver) — see "Design
    choice" below for why this is not the blob's `tap_name`,
  * sets the MAC via `dev_set_mac_address` under RTNL,
  * clears any prior IPv4 primary address (idempotence),
  * binds the new IPv4 via `devinet_ioctl(SIOCSIFADDR /
    SIOCSIFNETMASK)` and brings the iface up via SIOCSIFFLAGS,
  * installs the default route via the blob's `ipv4_gateway`
    using `ip_rt_ioctl(SIOCADDRT)`.

Failures at any step are logged via `pr_warn` and the loop
continues — a bad blob never aborts the fork loop.

The apply happens in the MASTER (parent), not the M-fork child.
Rationale: the M-fork child runs on a private stack and cannot
safely re-enter kernel C code that touches shared physmem (bug B1
hazard zone).  The forked child inherits the applied identity via
CoW; the master's own netdev state drifts to the latest blob
across iterations (invisible to consumers because the master is
never exposed as a pool member).

**Design choice — tap_name vs in-guest netdev**: the blob's
`tap_name` is the HOST-side TAP device created by the supervisor
(via `ip tuntap add`), not the in-guest netdev name.  UML's vector
driver names interfaces `vecN`; the legacy `uml_net` driver uses
`ethN`.  Neither matches what the supervisor put in `tap_name`,
which is a host artefact.  The cheapest dispositive test (boot a
UML guest, run `ip link show`) confirms: the in-guest netdev is
`vec0`, not `tap-pool-foo`.  Phase 2 therefore resolves the
target by scanning `for_each_netdev(&init_net, ...)` with a small
priority order (`vec*` > `eth*` > anything-but-lo).  Adding a new
field to the blob for the in-guest name would require a v2 blob
format and break the wire format already shared with umlctl — not
worth it for the single-netdev-per-pool case Phase 2 covers.

Tests guarding the apply path:

  * `arch/um/kernel/template_pause_identity_test.c` — 13 KUnit
    cases over the parse helpers (CIDR, address, mask-from-prefix),
    enabled by `CONFIG_UM_TEMPLATE_PAUSE_IDENTITY_KUNIT=y`.
  * `tools/testing/selftests/um/template-pause-smoke/` case 4 —
    boots UML with `vec0:transport=fd`, writes a blob, SIGCONTs,
    verifies via in-guest `ip addr show vec0` that the MAC and
    IPv4 actually changed.  SKIPs cleanly when the build lacks
    `CONFIG_UML_NET_VECTOR`.
  * `tools/testing/selftests/um/template-pause-fork-stress/` —
    unchanged.  Validated post-Phase-2 at 10/10 PASS, N=100,
    strict gates (G1-G8 including G7 zero panics), confirming
    the apply path doesn't regress the fork-on-resume primitive.

## What does NOT work today

1. **One member per master**.  The kernel
   `um_template_pause_enter()` returns after one SIGSTOP/SIGCONT
   cycle.  The master IS the taken instance; there is no
   fork-from-pause.  To get N siblings today, run N separate
   `umlctl pool spawn` commands, each booting a fresh master.

2. **Tap fd swap not yet wired**.  Phase 2 (this commit) applies
   MAC + IPv4 CIDR + IPv4 default gateway to the in-guest netdev
   via `dev_set_mac_address` + `devinet_ioctl(SIOCSIFADDR/
   SIOCSIFNETMASK/SIOCSIFFLAGS)` + `ip_rt_ioctl(SIOCADDRT)`.  What
   the kernel does NOT yet do: hot-swap the underlying TAP fd via
   SCM_RIGHTS on the identity-blob channel.  Pool members keep
   the master's TAP fd, which is fine for single-tenant pools but
   prevents per-take TAP isolation.  Tracked as a follow-on
   (Phase 2.2 in the original memo; gate behind
   `CONFIG_UM_TEMPLATE_PAUSE_FORK_TAP_SWAP` when it lands).

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
