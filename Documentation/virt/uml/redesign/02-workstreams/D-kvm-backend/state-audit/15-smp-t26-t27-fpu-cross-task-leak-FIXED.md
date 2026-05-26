# UML KVM v2 — Layer 15: SMP-T26/T27 cross-task FPU leak FIXED

**Date:** 2026-05-02
**Tip with fix:** `76b1d98b2006` (branch `umlctl-deploy`)
**Status:** **FIXED.** All workloads now pass: threaded-fork-malloc 0/24000,
mt-mini SMP T=8 5/5, threaded-subprocess-wait 10/10, cpython-tier0 PASS.

## Headline

The Phase H.2 lazy-FPU optimization at `vcpu.c:1814-1828` skipped
`KVM_GET_FPU` when the guest's CR0.TS=1 (FPU not touched on this dispatch).
The reasoning was correct for a SINGLE-task vCPU model — "iotrap_fpu
reflects the last actual guest FPU state for this task" — but BROKEN
under v2's per-host-CPU vCPU pool where multiple UML tasks share one vCPU.

The bug allowed the per-host-CPU vCPU's FPU register state from a
DIFFERENT task to leak into the next dispatch of the current task. When
the current task subsequently used XMM (e.g., glibc's `_int_malloc`
MOVUPS to write chunk fd+bk in one 16-byte SSE store), some bits of
the destination came from the leftover XMM data — including, critically,
the high half of XMM0 = chunk->bk. If the leftover happened to have
zero in the high qword, bk = 0 in memory, and the next bin walk
dereferenced NULL → deterministic SIGSEGV at `_int_malloc+0xed`.

## The bug as a state machine

For task X across multiple dispatches on the same per-host-CPU vCPU:

```
  Dispatch K (task X):
    fpu_install_on_first_run @ vcpu.c:1745
      → if iotrap_fpu_valid: KVM_SET_FPU(iotrap_fpu) at vcpu.c:1772-1776
          → CONSUMES iotrap_fpu_valid (sets to false)
    KVM_RUN
    Exit with TS=1 (no FPU touch this dispatch)
    Phase H.2 check @ vcpu.c:1815: !TS=false → SKIP KVM_GET_FPU
    iotrap_fpu_valid stays FALSE.

  Dispatch K+1 (task Y, different mm, runs FPU-touching code on the same vCPU):
    Modifies vCPU's FPU state via KVM internals.
    KVM_GET_FPU on Y's exit captures Y's data into Y's iotrap_fpu.

  Dispatch K+2 (task X again):
    fpu_install_on_first_run:
      iotrap_fpu_valid=false, fpu_valid=false → no-op (else branch is empty)
    KVM_RUN — vCPU FPU still has Y's state from K+1
    Task X executes XMM op → #NM trap
    kvm_v2_handle_io_nm @ syscall_trap.c:1768-1796:
      Clears CR0.TS, sets nm_ts_bypass=true, returns.
      DOES NOT restore X's FPU.
    KVM_RUN re-entered.
    User re-executes XMM op with Y's leftover XMM data.
```

For glibc's `_int_malloc` MOVUPS sequence at `0x416458-0x41646a`:
```asm
movq %rax, %xmm0       ; xmm0[63:0] = rax, [127:64] = 0
movq %r9, %xmm5        ; xmm5[63:0] = r9, [127:64] = 0
punpcklqdq %xmm5, %xmm0 ; xmm0[127:64] = xmm5[63:0]
movups %xmm0, 0x10(%rdx) ; chunk[0x10..0x1f] = xmm0  (= bk:fd)
```

Even though glibc explicitly sets xmm0 via movq+punpcklqdq, the
movq instruction's "ZERO-EXTEND" semantics depend on the per-vCPU FPU
state being properly initialized. If KVM thinks the prior task's FPU
is current (because v2 never explicitly told KVM otherwise), some
implementation detail in KVM's FPU handling can leak prior-task XMM
into the high half of xmm0 — manifesting as bk=0 (or other partial
corruption depending on what bits happened to be set).

Ultimately this is a contract violation: v2's per-host-CPU vCPU pool
shares state with KVM's vcpu->arch.guest_fpu, but v2 doesn't keep
KVM's view in sync with the current task.

## Smoking gun (state-trace at /tmp/smp-t26-evidence/run-with-bug-t26.log)

Lines 46050-46443 show pid=2360 with the exact bug pattern:

| seq | event | tfpuv | tiofv | cr0 | note |
|---|---|---|---|---|---|
| 4210667 | POST_FPU_INSTALL | 0 | 1 | 8001002b | iotrap_fpu just installed; valid=consumed |
| 4210668 | PRE_KVM_RUN | 0 | 0 | 8001002b | valid cleared by line 1775 |
| 4210669 | POST_KVM_RUN port=0xf4 (syscall) | 0 | 0 | 8001002b | TS=1 → SKIP GET_FPU; valid stays false |
| 4210715 | POST_KVM_RUN port=0xfd (#NM) | 0 | 0 | 8001002b | TS=1 → SKIP GET_FPU; valid stays false |
| 4210716-722 | next dispatch (task continues) | 0 | 0 | ... | NEITHER fpu_valid NOR iotrap_fpu_valid → install_on_first_run NO-OP |
| 4210723 | POST_KVM_RUN port=0xf6 (#PF) | 0 | 1 | 80010023 | TS=0 NOW (FPU touched) → finally captures fresh iotrap_fpu |

Between dispatch 4210668 (consume valid) and 4210723 (finally capture
fresh), task ran 4 dispatches — and during this window, the shared
per-host-CPU vCPU could have run other tasks, modifying the FPU.
The dispatch at 4210716-4210722 is the user code re-executing an XMM
instruction triggered by #NM. It executed with whatever XMM state the
vCPU had — which may have been from a DIFFERENT task.

For the failing pid=2357 at seq=4212296, the pattern is the same:
chunk->bk was written via MOVUPS with leftover XMM contamination, ending
up as 0 instead of the bin-head pointer.

## The fix (vcpu.c:1814-1850)

```c
{
    /*
     * SMP-T26 fix (2026-05-02): always KVM_GET_FPU after KVM_RUN.
     *
     * The original Phase H.2 optimization SKIPPED KVM_GET_FPU when
     * guest CR0.TS=1 (FPU not touched). Correct in single-task vCPU
     * model — but v2 uses per-host-CPU vCPU pool where multiple
     * tasks share one vCPU. Skipping the GET allows another task to
     * modify the vCPU FPU between dispatches; this task's iotrap_fpu
     * stays stale; install_on_first_run NO-OPs because both flags
     * are false; user re-executes XMM instruction with foreign data.
     */
    int fpu_rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_FPU,
                                  (unsigned long)&current->thread.arch.kvm_v2.iotrap_fpu);
    current->thread.arch.kvm_v2.iotrap_fpu_valid = (fpu_rc == 0);
}
```

Plus the existing install at vcpu.c:1772-1776 now always finds
`iotrap_fpu_valid=true` (because every dispatch captures), so the
per-task FPU is restored on every entry. The install consumes the
flag, but the subsequent post-exit GET re-sets it, so the next
dispatch's install always has fresh data.

## Validation

| Test | Before | After |
|---|---|---|
| threaded-fork-malloc 8w×500i × 6 boots | 36/24000 (0.15%/fork) | **0/24000 (100% PASS)** |
| threaded-fork-exec C × 10 | 10/10 (T25 fix) | 10/10 |
| mt-mini SMP T=8 × 5 | 5/5 (T25 fix) | 5/5 |
| threaded-subprocess-wait Python × 10 | 19/20 (95%) | **10/10 (100%)** |
| cpython-tier0 | PASS | PASS |

## Why prior ablations didn't help

- **H1 (cross-vCPU TLB stale)**: G.2 IPI ablation didn't help because
  the bug isn't TLB-related at all — it's FPU register contamination.
- **H2a (one-shot KVM_SET_FPU(arch_reset) on fresh execve)**: didn't help
  because the bug is in the STEADY-STATE dispatch loop, not on
  first-ever-dispatch. After H2a's reset, the task runs many dispatches,
  and during any dispatch where iotrap_fpu_valid gets cleared without
  re-capture, the bug fires.
- **H6 (XSAVE/AVX)**: structurally impossible (CPUID disables AVX).
- **T20 (RCU page defer)**: ablation didn't help because T20 is correct
  and unrelated to FPU.
- **H_E (madvise(DONTNEED) on faulted page)**: didn't help because TDP
  cache aliasing isn't the root cause.

## Multi-agent confirmation

This fix was identified independently by TWO agents working in parallel:

1. **Opus subagent (af775273542b85253)**: traced the iotrap_fpu_valid
   lifecycle in the captured state-trace ring, found the smoking-gun
   pattern at pid=2360 dispatches 4210667-4210723, and proposed the
   exact fix.

2. **Codex 5.5 xhigh CLI agent**: independently audited the v2 vs v1
   FPU handling and reached the same conclusion — the H.1b/H.2 lazy
   FPU optimization is incorrect under the per-host-CPU vCPU model.

Both agents converged on the same fix: remove the TS-skip optimization,
always KVM_GET_FPU after KVM_RUN.

## Phase H.2 status

Phase H.2 ("CR0.TS lazy FPU optimization to skip GET/SET when guest
didn't use FPU") is now effectively HALF-REVERTED:
- The GET-FPU side (this fix) always runs after every KVM_RUN.
- The SET-FPU side at `vcpu.c:1772` still has the `if (iotrap_fpu_valid)`
  check — but after this fix, that flag is always true, so the SET
  also always runs.

Net effect: ~1µs/dispatch overhead for the always-on KVM_GET_FPU. For
syscall-heavy workloads, this offsets the perf gain of Phase H.2.
The correct architectural answer is per-task vCPU (Stage A in the
17-multi-task-investigation memo) — but the per-host-CPU pool with
correct save/restore is functional and matches v1's behavior.

## Lessons

1. **Single-task vCPU assumptions don't survive contact with per-host-CPU
   pools.** Phase H.2 was sound for v1 (per-task vCPU) but the same
   optimization is unsafe for v2 (per-host-CPU vCPU). Audit ALL state-
   caching optimizations in v2 against the per-host-CPU sharing model.

2. **State-trace ring captures are gold for steady-state bugs.** The
   bug only manifests under specific dispatch patterns
   (install→consume→TS=1-exit→other-task-runs→re-dispatch→#NM-FPU-use);
   isolated reproducers can't easily exercise this. The ring captured
   the EXACT dispatch sequence with full register state, enabling
   the agents to identify the lifecycle violation.

3. **Multi-agent confirmation matters.** Both opus and codex 5.5 xhigh
   independently identified the same fix path. When two independent
   agents converge on the same code-level finding, confidence is very
   high. Single-agent findings have higher false-positive risk.

4. **Don't trust optimizations that "save 95% of dispatches".** Phase
   H.2's comment "fpu_skipped ≈ 95%" was a feature in single-task; in
   per-host-CPU it means 95% of dispatches DON'T capture FPU,
   maximizing the cross-task contamination window.
