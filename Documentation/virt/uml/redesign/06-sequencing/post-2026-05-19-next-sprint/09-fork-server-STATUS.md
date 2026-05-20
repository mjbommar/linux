# 09 — Fork-server status (as of 2026-05-20)

This document tracks the state of Memo 09 (the keystone
template-pause + fork sprint).  It is updated as phases land and
should be re-read at the start of each session.

## Phases — current state

| Phase | Description                          | Status                | Commit          |
|-------|--------------------------------------|-----------------------|-----------------|
| 1a    | Kernel template-pause hook           | LANDED (2026-05-20)   | `5051e9c90342`  |
| 1b    | umlctl integration (MVP `pool spawn`)| LANDED (2026-05-20)   | `5979caa69440`  |
| 2a    | Kernel-side fork-on-resume loop      | **EXPERIMENTAL — broken** (2026-05-20) | this commit |
| 1c    | `umlctl pool serve` daemon + multi-take | BLOCKED on 2a fix  | —               |
| 2     | Kernel applies identity (MAC/IP/tap) | PENDING               | —               |
| 3     | Bench + acceptance gates             | PENDING               | —               |
| 4     | syzkaller `vm/uml` Go shim           | BLOCKED on 2a fix     | —               |

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

Three selftests guard the end-to-end:

  * `tools/testing/selftests/um/template-pause-smoke/` — kernel-side
    primitive (unarmed, armed-no-identity, armed-with-identity).
  * `tools/testing/selftests/um/pool-spawn-smoke/` — umlctl wrapper.
  * `pool::tests` in `tools/uml/uml-launcher/src/bin/umlctl/pool.rs`
    — identity-blob layout + MAC parser.

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

**Fix sketches**:

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
