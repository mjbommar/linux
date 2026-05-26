# Path C result — v1 ceiling alive after 2026-05-21 fixes (Outcome 2)

**Date:** 2026-05-21 evening
**Experiment:** Path C from
`06-sequencing/post-2026-05-21-three-test-paths.md` §3.
**Branch:** `experiment-path-c` (in `/home/mjbommar/src/linux-path-c`,
not pushed — diagnostic only).
**Kernel:** `/home/mjbommar/src/uml-builds/uml-path-c/linux`.
**Verdict:** Outcome 2 — child panics at
`um_template_pause_enter+0xf6/0x147` exactly as the v1 ceiling
predicted.  SMP-T78/T79/T80 did NOT address it.  Pivot to Path A.

---

## 1. What I changed

Added a diagnostic-only `um_template_pause_continue_child=1`
cmdline flag in `template_pause.c`.  When armed, the M-fork
child takes the Memo 09 §2 original design path: returns up
the syscall stack from `um_template_pause_enter` normally,
instead of Phase 2a's `__NR_exit_group(0)` on a private stack.

Boot cmdline:
```
um_template_pause=fork
um_template_pause_continue_child=1
um_template_pause_private_stack=0
```

The private-stack flag also had to flip to 0 because Phase 2a's
default mode (`os_template_pause_fork_clone`) does inline-asm
`__NR_exit_group(0)` in the child before the C code can take any
branch.

The PATH-C parent's SIGKILL-child path was also guarded against
firing in `continue_child` mode — otherwise master kills child
before child reaches the v1-ceiling crash, which would mask the
result.

## 2. What I observed

Boot log (from `/tmp/path-c-2-ryw6f38v/uml.log`):

```
template_pause: armed via kernel cmdline (fork-on-resume) — EXPERIMENTAL
template_pause: PATH-C continue-child mode = ARMED (DIAGNOSTIC)
template_pause: private-stack mode = DISABLED (legacy fork+SIGKILL)
template_pause: /proc/um/template_pause ready
PATHC2_PRE_PAUSE pid=1
template_pause: enter("fork-smoke") — no UM_TEMPLATE_IDENTITY_FD;
                identity-blob channel disabled (err=-2)
template_pause: raising SIGSTOP at "fork-smoke" (pid=4056354)
template_pause: resumed via SIGCONT at "fork-smoke" (pid=4056354, count=1)
template_pause: torn down 1 stub(s) pre-fork
template_pause: host signal block ret=0
template_pause: raising SIGSTOP at "fork-smoke" (pid=4056354)
Kernel panic - not syncing: Kernel tried to access user memory at
                addr 0x6061c910, ip 0x6061c910
 [<6003a7c6>] um_template_pause_enter+0xf6/0x147
 [<6003a8dd>] template_pause_proc_write+0xc6/0xdd
um: DIAG panic: dumping last 46 mm-syscall events (head_snap=46)
```

Then master loops to iteration 2, 3, etc — each producing the same
panic in the child.  Master itself stays alive across panics
(consistent with the master holding `signals_blocked` and never
returning up the kernel stack).

## 3. What this means

  * **Panic IP** = `um_template_pause_enter+0xf6` — the function
    epilogue's `ret` instruction.  This is the EXACT same panic
    location and shape documented in
    `09-fork-server-STATUS.md` lines 219-251 (Phase 2a's original
    failure before the SIGKILL workaround).
  * **`addr == ip` = 0x6061c910** — the `ret` popped a value that's
    in the guest-userspace VA range.  Same shape as the
    EXTERNAL-RESEARCH §2.3 walk: signal handler frame pushed onto
    the kernel stack overwrote the saved RIP slot at the function
    epilogue's reserved offset.
  * **My PATHC: inline-asm writes never appeared** — meaning the
    child never reached the `return 0` from `fork_on_resume_loop`.
    Either:
    (a) The child crashed BEFORE the inline-asm write fired
        (corruption upstream of where I instrumented), OR
    (b) The child's inline-asm write executed but its host fd 1
        was already broken by the post-fork state.
    Either way the v1 ceiling is intact: even reaching the
    function-prologue printing point doesn't survive.

  * **SMP-T78/T79/T80 don't address it.**  The fixes I shipped
    today are at orthogonal layers:
    - T78: kvm-v2 backend's migrate_disable around handle_syscall.
      Not on the template_pause path (this is seccomp backend).
    - T79: kvm_v2_snapshot_pick_vcpu race.  Same — kvm-v2 only.
    - T80: is_user=1 flag in timer_real_alarm_handler.  This is
      the closest in spirit (signal handler intercept) but it
      changes WHICH counter the tick goes to, not WHETHER the
      signal frame gets pushed on the parent's kernel stack.

## 4. Recommendation per the Path C plan §3.3

**Outcome 2.  Pivot to Path A's integration build-out.**

Path A's stack-pivot primitive (`tools/testing/selftests/um/
rt-sigreturn-isolation/`, committed at 4322afe2e555 + 73c98eabd31b)
demonstrated the mechanism works on this host: 6 instructions of
inline asm pivots %rsp + jmpq into a clean entry, bypassing the
corrupted saved-RIP slots.

The integration into UML's template_pause path:

  1. In the M-fork child branch (currently the spot where
     `template_pause_continue_child_armed_flag` was tested), do
     NOT `return 0` from C — instead, execute the stack-pivot
     into a `template_pause_child_entry()` function that runs
     on a private MAP_PRIVATE stack.

  2. `template_pause_child_entry()` calls:
     - `preempt_enable()` (master held it)
     - `um_template_identity_apply(&blob)` (delivered via the
       memfd-shared blob the daemon writes)
     - `um_skas_respawn_all_stubs()` (rebuild SKAS state)
     - then enters the guest's main loop indefinitely

  3. The master never returns from `um_template_pause_enter`
     either — it loops in `fork_on_resume_loop`'s blocking
     primitive (per the AFL forkserver invariant the
     EXTERNAL-RESEARCH memo §1.7 quotes).

This is the work the comparison memo §3 named "1.6 + 1.7 + 1.3
in sequence."  Path A landed (1.7 mechanism).  Next: 1.6 (AFL
preconditions) + 1.3 (pre-fork pool model wiring).

## 5. Cross-references

  * Path A landing: `4322afe2e555` (test code) +
    `73c98eabd31b` (drop binary).
  * Path C experiment branch: `experiment-path-c` (local-only,
    not pushed).
  * Path C kernel: `/home/mjbommar/src/uml-builds/uml-path-c/linux`.
  * Comparison memo:
    `post-2026-05-21-pool-architecture-comparison.md` §1.3 + §1.7.
  * Three test paths memo:
    `post-2026-05-21-three-test-paths.md` §3.
  * EXTERNAL-RESEARCH §2 — the corrupted-saved-RIP analysis.
  * `09-fork-server-STATUS.md` §"Phase 2a v1-ceiling" — original
    documentation of the same panic IP from 2026-05-20.

## 6. Test artifacts (transient, will be cleaned up)

  * `/home/mjbommar/src/linux-path-c/` — worktree with the
    diagnostic patch.  Branch `experiment-path-c`.
  * `/home/mjbommar/src/uml-builds/uml-path-c/` — built kernel.
  * `/tmp/path-c-2-ryw6f38v/uml.log` — the panic log quoted in §2.
  * `/tmp/path-c-experiment.py` + `/tmp/path-c-experiment-2.py`
    — the Python harness drivers.

These are diagnostic-only; do not merge.  Memo + Path A's
selftest are the durable record.
