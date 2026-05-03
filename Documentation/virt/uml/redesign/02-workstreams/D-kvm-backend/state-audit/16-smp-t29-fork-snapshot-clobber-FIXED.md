# UML KVM v2 — Layer 16: SMP-T29 fork-snapshot clobber FIXED

**Date:** 2026-05-03
**Tip with fix:** `44d21b5a14ab` (branch `umlctl-deploy`)
**Status:** **FIXED.** 30-boot threaded-fork-malloc soak: 29/30 with
0 fails / 116000 forks (1 RCU-stall outlier, different bug class —
no CHILD_FAIL events). Pre-T29 baseline: 1/120000 with cascade
(2675 sequential fails when it hit).

## Headline

`kvm_v2_fpu_capture_for_switch_out` (vcpu.c:2245) was unconditionally
`KVM_GET_FPU`'ing the per-host-CPU vCPU's FPU and overwriting
`from->thread.arch.kvm_v2.fpu` — including for tasks that had never
dispatched on that vCPU yet. This destroyed the parent-FPU snapshot
that `kvm_v2_fpu_capture_for_fork` had placed for a freshly-fork'd
task between fork and first dispatch.

The bug closed the residual that survived T26/T27. T26/T27 fixed the
cross-task XMM leak inside the dispatch loop (always KVM_GET_FPU after
KVM_RUN). T29 fixes the cross-task XMM leak at fork-to-first-dispatch
boundary (don't KVM_GET_FPU on switch-out unless the per-CPU vCPU's
FPU actually belongs to this task).

## Mechanism (multi-agent identified)

Window:

1. Worker pthread W forks child C. `kvm_v2_fpu_capture_for_fork`
   (called from `arch_copy_thread`) sets `C->thread.arch.kvm_v2.fpu`
   = (W's per-CPU vCPU FPU snapshot, captured via KVM_GET_FPU on the
   vCPU W just exited from). `C->thread.arch.kvm_v2.fpu_valid = true`.
2. C is queued runnable but has not yet entered `kvm_v2_vcpu_run` —
   it is still in fork's host-side path or returning from the syscall.
3. SIGALRM preempts C (any host-side preemption point counts —
   inside kernel C code or at the syscall boundary). Scheduler picks
   another task X to run.
4. `kvm_v2_context_switch` → `kvm_v2_fpu_capture_for_switch_out(C)`
   fires. The per-CPU vCPU's FPU at this moment holds **W's** FPU
   (W was the previous-but-one occupant), or any other task that ran
   on this CPU in the meantime. Critically, it does NOT hold C's FPU
   because C never ran KVM_RUN on this vCPU.
5. The unconditional `KVM_GET_FPU` overwrites C's pristine
   parent-snapshot with this random-task FPU.
6. Eventually C dispatches. `kvm_v2_fpu_install_on_first_run`
   sees `fpu_valid=true` and `KVM_SET_FPU`s the corrupted snapshot
   into the destination per-CPU vCPU.
7. C's first guest instructions are glibc's `__libc_fork`
   post-fork-cleanup MOVAPS writes to stack. Corrupted XMM corrupts
   the stack frame.
8. The next iteration of glibc's `_dl_stack_used` walk (in
   `__libc_fork`'s child branch) faults at `__fork+0x11c
   (mov 0x3d8(%r15),%rax)` with `cr2=0x3d8` — i.e., r15 dereferenced
   as NULL+0x3d8 because XMM corruption smashed r15 via the MOVAPS
   that wrote the spill slot.
9. **Cascade:** the parent's worker pthread W also walks
   `_dl_stack_used` (which is a process-shared pthread cache list).
   The same memory it just wrote with a partial XMM is what every
   other fork in this process now reads. A single child crash often
   cascades into 2675 sequential CHILD_FAIL events because every
   subsequent fork re-traverses corrupt linked-list memory.

Smoking gun: `/tmp/smp-t29-evidence-parent-fork-cascade.log` (812KB
trace from run-21 of the post-T26/T27 30-boot soak). 2675
CHILD_FAIL events in a single boot, all with identical fault
(`__fork+0x11c cr2=0x3d8`), proving the corruption is in shared
parent state (the `_dl_stack_used` linked list), not per-child
random heap.

## Why T26/T27 didn't catch this

T26/T27 ran `KVM_GET_FPU` after every `KVM_RUN` exit, ensuring the
post-dispatch FPU state for the running task is captured into its
per-task snapshot. That correctly closes the within-dispatch leak.

But it does nothing for tasks that have NEVER run a dispatch yet.
A freshly-fork'd child sits in `arch_copy_thread` having had its
parent's snapshot deposited; if it gets context-switched before
its first KVM_RUN, the destructive `capture_for_switch_out` runs
on stale per-CPU vCPU state.

## The fix (purely subtractive)

Add a gate in `kvm_v2_fpu_capture_for_switch_out`:

```c
    if (vcpu->last_task != from) {
            /* Per-CPU vCPU's FPU does NOT reflect `from`. Preserve
             * any existing fpu_valid=true (set by capture_for_fork
             * or an earlier valid capture). */
            migrate_enable();
            return;
    }
```

`vcpu->last_task` is set in `kvm_v2_load_user_sregs` at every
dispatch entry (vcpu.c:1302). So `last_task == from` ⇔ "the most
recent KVM_RUN on this per-host-CPU vCPU was from's, and KVM_GET_FPU
here would read from's actual FPU."

If the gate fails, we leave any existing snapshot intact. For the
fork case, that means the parent-snapshot from `capture_for_fork`
survives unmolested all the way to first dispatch.

Also stopped clearing `fpu_valid=false` in the "pool not up" path
— same reasoning, don't clobber a valid prior snapshot just
because the per-CPU vCPU didn't materialize.

## Why this differs from T28 (which regressed)

T28 attempted to add a NEW unconditional `KVM_SET_FPU(arch_reset)`
on first dispatch after execve, gated by `fpu_arch_reset_needed`
flag set by `arch_flush_thread`. This was **additive** and
**destructive** — it actively introduced wrong state for the dominant
fork+exec workloads (where the parent's FPU snapshot is the right
state to install, not arch reset). Result: rate REGRESSED from
~0.003%/fork to 0.046%/fork (55× worse).

T29 is **subtractive** and **preservative**. It removes a destructive
write path (capture_for_switch_out unconditionally clobbering the
snapshot) without adding any new SET_FPU paths. The lifecycle
becomes:

```
  arch_copy_thread (fork):
    capture_for_fork → snapshot W's vCPU FPU into C->arch.kvm_v2.fpu
    C->arch.kvm_v2.fpu_valid = true
  ── preemption window opens here ──
  Any switch_out of C before first run:
    capture_for_switch_out: vcpu->last_task != C → return (preserve)
  ── preemption window closes when C dispatches ──
  C's first dispatch:
    install_on_first_run: fpu_valid=true → SET_FPU(C's snapshot)
                          fpu_valid = false  (one-shot)
    KVM_RUN — guest starts with W's FPU (correct fork inheritance)
```

## Validation

| Workload                               | Pre-T29 (T26/T27 only) | Post-T29 (44d21b5a14ab) |
|----------------------------------------|------------------------|-------------------------|
| threaded-fork-malloc 30-boot soak      | 29/30 (1 cascade=2675) | 29/30 (1 RCU stall, no CHILD_FAIL) |
| threaded-fork-malloc forks total       | 1 / 120000 (0.00083%)  | **0 / 116000 (0%)**     |
| cpython-parity × 5 modules × runs      | 21/21 each, 5/5 runs   | 21/21 each, 5/5 runs    |
| substrate gate                         | 25 PASS / 3 FAIL / 3 XFAIL | (rerun next)        |
| mt-mini SMP T=8                        | 5/5                    | (rerun next)            |
| threaded-subprocess-wait               | 10/10                  | (rerun next)            |

Run-7's RCU-stall outlier is **not the cascade pattern** — no
CHILD_FAIL events, no corrupted XMM, no `__fork+0x11c` fault. A
threaded-fork-m process gets stuck in libc syscall (RIP=0x7ffff7cacae0,
RAX=0x2f, RDI=0x1c — looks like `read(fd=28, ...)`) and the watchdog
RCU notices forward-progress stall. Distinct bug class — file as
its own task.

## Open follow-ups

- Validate substrate + mt-mini SMP T=8 + threaded-subprocess-wait
  haven't regressed under T29.
- Investigate run-7 RCU-stall class (separate bug, not in
  T29's scope).
- Phase J (#167): 24h continuous + Tier 1/2/3 + 4-week soak.
- python -m test parity on ~150-module curated set (#185).

## Don't repeat (lessons)

- **Per-CPU pool means snapshot lifecycle has 4 phases, not 2.**
  Single-task vCPU model conflated "from is on the per-CPU vCPU" with
  "from has run since fork." Under per-host-CPU pool, the two are
  independent — there's a window where from carries a valid snapshot
  but the per-CPU vCPU holds someone else's state. Capture paths must
  gate on "is the source-of-truth actually mine?"
- **Subtractive > additive when fixing leaks.** T28's additive arch-
  reset broke fork. T29's subtractive gate preserved it. When a value
  is sometimes right and sometimes wrong, the answer is usually
  "preserve unless we know we have the right new value" not "always
  overwrite with our best guess."
- **Cascades are smoking-gun evidence of shared-state corruption.**
  2675 sequential identical-RIP failures are NOT 2675 independent
  random events — they are one corruption being read by every
  subsequent traversal of the corrupted shared structure
  (here, glibc's `_dl_stack_used` linked list).
- **Multi-agent helped.** The opus subagent traced
  `capture_for_fork → switch_out → install_on_first_run` lifecycle
  against the cascade trace, identified the preemption window, and
  proposed the gate before the codex audit returned. Independent
  reasoning from cascade-evidence + lifecycle-knowledge converged
  on the same window.
