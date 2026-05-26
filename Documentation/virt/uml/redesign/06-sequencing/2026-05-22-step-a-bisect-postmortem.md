# Step A bisect postmortem — 15-variant analysis of the post-swap user-wake regression

**Date:** 2026-05-22
**Status:** Postmortem; helpers landed (`tools/uml/syzkaller-vm-shim/uml.go`,
`arch/um/os-Linux/process.c`, `arch/um/os-Linux/time.c`,
`arch/um/os-Linux/io_uring.c`, `arch/um/kernel/physmem.c`);
call-site stays unwired in `arch/um/kernel/template_pause.c::child_entry_pool_member`.

**Reference docs:**
  * `2026-05-22-pool-completion-roadmap.md` — the working-backwards plan
    (§3.1 Option B contains the dispositive bisect table)
  * `post-2026-05-19-next-sprint/09-fork-server-USAGE.md` — operator guide
    for the current single-iter state
  * `state-audit/32` — userspace shared-physmem architectural finding

---

## 1. The problem one paragraph

After `os_template_pause_fork_clone_to(child_entry_pool_member)` in
`arch/um/kernel/template_pause.c`, the fork child runs through
`child_entry_pool_member`.  Master keeps a MAP_SHARED `physmem_fd`;
the child inherits the same fd-table reference and the same kernel-VA
mmap of it.  Bash userspace pages live in that physmem, so any
modification by iter N's bash propagates to master and to iter N+1.
The roadmap §3 names this the architectural wall blocking sustained
dispatch (Step A).  The fix shape is "swap the kernel-VA mapping to
a per-member memfd / O_TMPFILE with replicated content" — that's
`um_pool_replicate_physmem()`.  Empirically wiring it in breaks
single-iter: bash reaches `TPPM_MEMBER_ALIVE_1` then hangs in
`sleep(1)`.

## 2. The 15-variant dispositive bisect

All variants ran inside `child_entry_pool_member` against the
template-pause-pool-member-smoke selftest.  Build:
`/home/mjbommar/src/uml-builds/umb-pathA/linux` (kernel HEAD at the
time, with `CONFIG_UM_TEMPLATE_PAUSE_FORK=y`,
`CONFIG_UM_SNAPSHOT_FORKSERVER=y`).

| # | Variant                                                                       | Result            | Conclusion                                          |
|---|-------------------------------------------------------------------------------|-------------------|-----------------------------------------------------|
| 1 | Same-fd mmap-FIXED of physmem region (`um_pool_remap_self_test`)              | PASS              | mmap-FIXED operation itself is benign               |
| 2 | Dup'd-fd mmap-FIXED (same file, different fd value via `os_dup_file`)         | PASS              | Host kernel routes through inode; fd identity OK    |
| 3 | New-fd mmap-FIXED to `memfd_create`-backed fd                                  | FAIL              | Different INODE breaks user-mode timer wake-up      |
| 4 | New-fd mmap-FIXED to `O_TMPFILE` on /dev/shm or /tmp                           | FAIL              | Eliminates memfd-vs-tmpfs as differentiator         |
| 5 | Skip kernel-VA mmap, just swap global `physmem_fd`                             | FAIL              | SKAS kernel↔stub coherence cannot be preserved      |
| 6 | Anon intermediate (`os_remap_region_via_anon`) → new fd                        | FAIL              | Intermediate MAP_ANON doesn't reset host state      |
| 7 | Close inherited io_uring fds (closed=2: ubd + hostfs wb) before swap           | FAIL              | NOT io_uring task_work / TIF_NOTIFY_SIGNAL          |
| 8 | post-swap `sigtimedwait` drain (drained=0)                                    | FAIL              | NOT a stuck pending signal — none queued            |
| 9 | post-swap 50 ms one-shot + 200 ms busy_wait → `timer_getoverrun`/`gettime`     | **TIMER FIRES**   | Disproves "host POSIX timer is broken"              |
| 10 | post-swap 50 ms periodic timer + 525 ms busy_wait (4 reads)                   | **DELIVERED**     | itv cycles ~50 ms, overrun stable at 0 = signals delivered each tick |
| 11 | variant 10 + diagnose post-`start_userspace_fresh`                            | ARMED             | Timer survives stub creation; itv still ~50 ms      |
| 12 | Plain replicate (no instrumentation)                                          | FAIL              | Confirms baseline regression                         |
| 13 | replicate + post-swap `timer_worker_forget`+`rebuild`+`one_shot`              | FAIL              | Recreating POSIX timer doesn't recover               |
| 14 | variant 13 + post-swap `os_idle_prepare()` (rebuild signalfd)                  | FAIL              | NOT inherited signalfd binding                       |
| 15 | replicate + 200 ms in-kernel SIGALRM drain + timer reprime                    | FAIL              | "Priming" the signal stream doesn't transfer to user-mode |

## 3. What the bisect proves

### Proven negatives (hypotheses disproven)

  * **NOT** signal-delivery breakage (variants 9-10: SIGALRM IS being
    delivered every 50 ms post-swap; UML's `timer_alarm_handler` runs).
  * **NOT** stuck pending signal (variant 8: drained=0).
  * **NOT** io_uring task_work / TIF_NOTIFY_SIGNAL inheritance
    (variant 7: closing 2 inherited io_uring fds doesn't help).
  * **NOT** inherited signalfd binding (variant 14: rebuilding
    `wake_signals` via `os_idle_prepare()` doesn't help).
  * **NOT** the mmap-FIXED operation itself (variants 1-2: same-inode
    swap works).
  * **NOT** memfd-vs-tmpfs (variants 3-4: both fail identically).
  * **NOT** SKAS coherence skip (variant 5: master enters runaway loop).
  * **NOT** missing intermediate anon mapping (variant 6).
  * **NOT** stale POSIX timer handle (variant 13: recreating doesn't help).
  * **NOT** un-primed signal stream (variant 15: 200 ms kernel-context
    drain doesn't transfer).

### Proven positives

  * Different-INODE mmap-FIXED swap is the trigger (variants 3-4 fail;
    variants 1-2 pass).
  * Host POSIX timer continues firing on schedule post-swap
    (variants 9-10-11).
  * Signal handler runs and processes ticks correctly in kernel context
    (variant 10's overrun=0 across periodic timer).
  * Timer survives stub creation (`start_userspace_fresh`, variant 11).

### Open hypothesis

UML's idle / hrtimer-interrupt path interacts badly with the post-swap
clock-event device state.  The signal handler runs and the UML tick
processes — but the hrtimer wake-up doesn't translate into bash's task
being picked by `schedule()` once userspace runs.

This points specifically at:

  * `arch/um/kernel/process.c::arch_cpu_idle` →
    `arch/um/kernel/process.c::um_idle_sleep` →
    `arch/um/os-Linux/time.c::os_idle_sleep` (the signalfd-based wait)
  * `kernel/time/hrtimer.c::hrtimer_interrupt` invocation chain
    through `arch/um/kernel/time.c`'s clock_event_device

## 4. The next concrete debug step

Instrument the wake-up chain.  Specifically:

  1. **Confirm SIGALRM reaches the handler in userspace context.**
     Add a `printk_once_per_second` in `timer_alarm_handler`
     (`arch/um/os-Linux/signal.c:210`) that prints when first called
     post-swap-userspace-entry.  If this never fires post-userspace,
     the handler isn't reaching us in the userspace-resume path; if
     it fires but bash still hangs, the bug is purely in
     `hrtimer_interrupt`'s callback chain.

  2. **Trace `try_to_wake_up()`.**  Bash's hrtimer expiration should
     call `wake_up_process(bash)` which calls `try_to_wake_up`.  If
     that runs but the schedule pick doesn't happen, the runqueue
     state is corrupt.

  3. **Trace clock_event_device.set_next_event.**  The variant-10
     finding (signal delivered every 50 ms periodic) proves
     `set_next_event` runs.  But for bash's nanosleep (1 second
     one-shot), maybe `set_next_event` fails or the host
     `timer_settime(1 second)` doesn't fire SIGALRM after 1 second.
     Add a printk capturing the host-side return value of every
     `os_timer_one_shot` call.

  4. **Minimal Linux-only repro.**  Write a small C program that
     reproduces the SIGALRM-after-different-inode-mmap behavior
     outside UML.  If the bug doesn't reproduce in the minimal
     program, it's UML-specific (most likely the
     `clone(CLONE_VM)`-shared stub MM interaction with the mmap-
     FIXED swap).  If it does reproduce, file it as a Linux kernel
     bug and look for known regressions in this area.

**UPDATE (2026-05-22):** Variant (16) and a minimal Linux-only
repro (in tree at `tools/testing/selftests/um/mmap-fixed-sigalrm-
repro/`) both PROVE the host POSIX-timer + SIGALRM delivery path
is INTACT across mmap-FIXED-to-different-inode swaps:

  * The standalone repro arms a 50 ms one-shot timer on a tmpfs
    MAP_SHARED region, swaps the region's backing to a different
    O_TMPFILE inode with memcpy'd content, re-arms, and counts
    SIGALRMs in 200 ms windows on each side of the swap.
  * Result: `pre_swap_sigalrms=1 post_swap_sigalrms=1` — signals
    are delivered identically both sides of the swap.
  * **This dispositively narrows the search to UML-side code.**
    The bug is NOT in the host kernel's signal-delivery or
    POSIX-timer subsystems.  It is in UML's kernel-mode handling
    of one of:
      - the `clone(CLONE_VM)`-shared stub MM interaction with the
        mmap-FIXED swap (UML's stubs share VM with the kernel
        process — any swap is visible to the stub but the
        stub's own pid/futex state may not survive cleanly)
      - UML's `hrtimer_interrupt` callback chain on the
        clock_event_device path (run from the SIGALRM handler;
        relies on UML kernel data structures that ARE in
        physmem-backed slabs and DO change inode underneath
        them at swap time)
      - The runqueue state of the bash task in physmem-backed
        slabs (`task->__state`, `task->on_rq`, the CFS rq
        tree) — content is memcpy'd at swap but the host
        kernel's view of the underlying pages changes

## 5. Helpers landed and ready for one-line re-wire

The following helpers are in tree and tested compilable, waiting for
the fix:

  * `os_create_memfd(name, size)` — anonymous memfd
  * `os_create_tmpfile(dir, size)` — O_TMPFILE on /dev/shm or /tmp
  * `os_mmap_rw_scratch(fd, off, len, *out)` — scratch RW mmap helper
  * `os_remap_region_shared(addr, fd, off, len)` — mmap-FIXED swap
  * `os_remap_region_via_anon(addr, fd, off, len)` — anon-intermediate
    swap (variant 6)
  * `os_close_inherited_io_uring_fds()` — direct-syscall fd scan
  * `os_drain_pending_signals()` — sigtimedwait drain
  * `os_timer_diagnose(*overrun, *itv)` — timer state read
  * `os_busy_wait_ns(nsecs)` — clock_nanosleep-based wait
  * `um_pool_replicate_physmem()` — full replicate + swap + global
    physmem_fd update
  * `um_pool_remap_self_test()` — variant 1 dispositive control

Re-wire site: `arch/um/kernel/template_pause.c::child_entry_pool_member`,
between the task/signal-state restoration block and the
`disown_inherited + start_userspace_fresh` block.  A single
`(void)um_pool_replicate_physmem();` call there re-enables the
attempt once the host-side root cause is identified.

## 6. What this postmortem changes

Step A's INTEGRATION remains pending.  Steps B (sustained-smoke
PASS), C (pool-bench acceptance), and D's "Series 7 send" (gated on
24 h soak) remain pending.  Step D's syzkaller `vm/uml` Go shim is
landed in tree (`tools/uml/syzkaller-vm-shim/`), USAGE.md is
landed.

What this postmortem provides is a complete, dispositive map of
where the remaining architectural work needs to land.  The next
investigator can pick up from §4 without re-running any of variants
1-15.

---

*This is a snapshot of the bisect state and the open hypothesis.  When
the underlying host-side root cause is identified, edit this file with
the resolution and the date.*
