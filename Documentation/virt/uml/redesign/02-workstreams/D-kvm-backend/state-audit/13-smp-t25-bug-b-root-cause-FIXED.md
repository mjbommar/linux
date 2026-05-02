# UML KVM v2 — Layer 13: SMP-T25 Bug B root cause FIXED

**Date:** 2026-05-02
**Tip with fix:** `b1421d7583e9` (branch `umlctl-deploy`)
**Status:** **FIXED.** Bug B class fully closed across all SMP fork-stress workloads.

## Headline

Bug B was caused by `kvm_v2_marshal_from_kvm_regs()` on the EINTR
path unconditionally writing `eintr_regs.rip` into `regs->gp[HOST_IP]`,
even when the EINTR caught the guest at the LSTAR trampoline (between
SYSCALL transition and the trampoline's `out %al, $0xf4` vmexit). The
next dispatch then re-entered KVM_RUN with `kvm_run.rip = LSTAR` and
USER CS, causing an instruction-fetch fault on the kernel-only LSTAR
page.

T25 fix: in the EINTR if/else chain at `vcpu.c:1926`, recognise
`eintr_regs.rip ∈ [LSTAR, LSTAR+2)` and rewind `HOST_IP` to
`HOST_CX - 2` (the user SYSCALL instruction itself). On the next
dispatch the user re-executes SYSCALL → trampoline runs normally
→ `handle_io_trap` dispatches the syscall.

## Smoking gun (BUG_B captured at run 31 of SMP-T24 BB-hunt-v2)

`/tmp/smp-t24-bbcap/run-31.log` (3.8MB trace dump, 5140 entries
frozen at TRACE_TRIGGER):

```
seq 5572645  pid=296  PRE_KVM_RUN     exit=2  port=0xfd
              rip=40273d24  rcx=402db248  hip=40273d24  hsp=7f7ffffd8418
seq 5572646  pid=296  POST_KVM_RUN    exit=10 port=0xfd
              rip=ffffe00000000040 (LSTAR)  rcx=402db248  hip=40273d24
seq 5572647  pid=296  EINTR_PATH      exit=10 port=0xfd
              rip=ffffe00000000040 (LSTAR)  hip=40273d24  ← still pre-marshal
              [marshal_from_kvm_regs runs on next line, writes hip=LSTAR]
   ...4144 dispatches of OTHER tasks elapse...
seq 5576791  pid=296  VCPU_RUN_ENTRY  ← TASK SWITCH back to pid=296
              hip=ffffe00000000040 (LSTAR — corrupted!)
              hsp=7f7ffffd83d8
seq 5576796  pid=296  PRE_KVM_RUN     exit=10 port=0xfd
              rip=ffffe00000000040 (marshal_to_kvm_regs wrote LSTAR)
seq 5576797  pid=296  POST_KVM_RUN    exit=2  port=0xf6
              cr2=ffffe00000000040  ← #PF on LSTAR
              IST=[15, ffffe00000000040, 0x2b, 0x10046, 7f7ffffd83d8, 0x23]
              error_code=0x15 = (P=1, U=1, ID=1) = user inst-fetch
              of kernel-only page  ← BUG_B SIGNATURE
seq 5576799  TRACE_TRIGGER (BUG_B fires, ring frozen)
```

Decoded:
- pid=296 was a normal Python child task with ~5000 prior dispatches.
- One of those dispatches caught EINTR-mid-LSTAR, corrupting
  `regs->gp[HOST_IP]` to LSTAR via `marshal_from_kvm_regs`.
- ~4144 dispatches later, scheduler returned to pid=296, marshalled
  the (corrupted) HOST_IP into `kvm_run.rip`, KVM_RUN entered with
  USER CS at LSTAR, CPU instruction-fetched from kernel-only page,
  faulted with error_code=0x15.

This explains every prior post-T22 BUG_B occurrence: the trigger is
**rare** (EINTR catching SYSCALL exactly between transition and OUT
vmexit) but the corruption is **persistent** until the task next
goes through `handle_io_trap` (which would overwrite HOST_IP from
HOST_CX). Under heavy SYSCALL load with many fork+exec churn (where
HOST_IP doesn't get overwritten through normal SYSCALL completion
between corruption and next vCPU re-entry), the corrupted HOST_IP
survives → fault on next dispatch.

## Why prior fixes (T13/T17/T19/T22) were partial

| Fix | What it addressed | Why residual remained |
|---|---|---|
| T13 (migrate_disable) | NM_stub+2 inline-PF wrong RSP | Not the LSTAR-EINTR mechanism |
| T17 (inline-#NM EINTR handler) | EINTR mid-NM-stub iretq pop | Only catches NM stub range, not LSTAR |
| T19 (IST sanity guards) | Defensive — reject CPL=0 IST writes | Doesn't address upstream HOST_IP corruption |
| T20 (RCU page free) | mmu_gather free-before-flush | Not the EINTR path bug |
| T22 (vmexit-on-#NM) | Replace `clts;iretq` with vmexit | Eliminates NM-stub mechanism but LSTAR-EINTR still occurs |
| **T25 (LSTAR-EINTR rewind)** | **Marshal-from-kvm-regs writes LSTAR to HOST_IP** | **Closes the root cause** |

The Bug B "class" was actually two distinct mechanisms (NM-stub iretq
+ LSTAR-EINTR HOST_IP corruption) sharing the same fault signature
(user #PF at kernel-half address). T22 closed mechanism #1; T25
closes mechanism #2.

## Validation matrix

| Workload | Pre-T25 | Post-T25 | Notes |
|---|---|---|---|
| threaded-fork-exec C × 30 | 29/30 (~97%) | **30/30 (100%)** | dedicated repro |
| threaded-fork-exec C × 20 (counter on) | n/a | **20/20**, 2 LSTAR-rewind triggers | confirms fix fires |
| threaded-subprocess-wait Python × 20 | 19/20 | 19/20 | the 1 fail was glibc heap corruption, BUG_B=0 — DIFFERENT bug |
| **mt-mini SMP T=8 × 20** | **~50%** | **20/20 (100%)** | same root cause! |
| cpython-tier0 | PASS | PASS | no regression |

## The fix (vcpu.c:1926-1980)

```c
if (eintr_regs.rip >= KVM_V2_LSTAR_GVA &&
    eintr_regs.rip <  KVM_V2_LSTAR_GVA + 2) {
    /*
     * SMP-T25 (2026-05-02) Bug B residual fix:
     * EINTR caught the guest at the LSTAR trampoline, between
     * the SYSCALL transition and the trampoline's
     * `out %al, $0xf4` vmexit. The 5-byte trampoline is
     * `out %al, $0xf4 ; sysretq` (bytes e6 f4 48 0f 07): the
     * OUT spans LSTAR..LSTAR+1, the SYSRETQ spans LSTAR+2..
     * LSTAR+4. EINTR at LSTAR or LSTAR+1 means the OUT had
     * not yet vmexited, so handle_io_trap never ran, so
     * HOST_IP was not redirected from LSTAR to HOST_CX
     * (post-SYSCALL user RIP). The marshal_from_kvm_regs
     * above therefore left regs->gp[HOST_IP] = LSTAR; on
     * the next dispatch's marshal_to_kvm_regs we'd write
     * kvm_run.rip = LSTAR with USER CS, causing an
     * instruction-fetch fault on the kernel-only LSTAR
     * page (BUG_B class: err=0x15 P=1/U=1/ID=1).
     *
     * Fix: rewind HOST_IP to the user-space SYSCALL
     * instruction itself (HOST_CX - 2; SYSCALL is 2 bytes,
     * opcode 0F 05). On the next dispatch the user re-
     * executes SYSCALL → CPU re-jumps to LSTAR → trampoline
     * OUT vmexits → handle_io_trap dispatches the syscall
     * normally. No user-side state is duplicated because we
     * only re-execute the 2-byte SYSCALL instruction itself
     * (which is idempotent: it just traps to the kernel).
     */
    KVMV2_TRACE(KVMV2_OP_EINTR_INLINE_LSTAR, regs, run, vcpu);
    /* rate-limited counter printk omitted for brevity */
    regs->gp[HOST_IP] = regs->gp[HOST_CX] - 2;
}
```

Plus a new trace op `KVMV2_OP_EINTR_INLINE_LSTAR = 19` so future
trace dumps tag the rewind events distinctly.

## Why two LSTAR_GVA bytes (not five)

The trampoline body is 5 bytes (`e6 f4 48 0f 07`):

| Offset | Bytes | Instruction |
|---|---|---|
| LSTAR | `e6 f4` | `out %al, $0xf4` |
| LSTAR+2 | `48 0f 07` | `sysretq` (REX.W + opcode) |

EINTR at LSTAR or LSTAR+1: OUT hasn't vmexited → syscall hasn't
been processed → must re-issue SYSCALL.

EINTR at LSTAR+2..LSTAR+4 cannot occur in normal v2 operation:
when the OUT vmexits, `handle_io_trap` sets `HOST_IP = HOST_CX`
directly and the next entry jumps to user RIP, **bypassing the
SYSRETQ entirely**. So the SYSRETQ region is dead code for v2 — it
exists only as a fallback for paths that re-enter at LSTAR for
reasons other than handle_io_trap (none currently).

## Identified follow-ups

1. **Other handler-stub EINTR cases**: same mechanism could in
   theory affect `out %al, $0xfd ; iretq` (#NM stub). But T22 made
   the iretq unreachable (vmexit-on-#NM means we never resume past
   the OUT). Verify: grep state-trace for any post-T22 events with
   eintr_regs.rip in `[NM_stub, NM_stub+2)` followed by HOST_IP
   corruption. None observed in current sweeps.

2. **glibc heap corruption residual**: the 1/20 threaded-subprocess-
   wait fail (run 14) had `rc=-6 stderr="free(): invalid size"` and
   `rc=-6 stderr="malloc(): corrupted top size"`. BUG_B=0. This is
   a separate page-recycling-class issue distinct from Bug B.
   Possible candidates: SMP-T23 cross-mm cr2-zero gate not catching
   all execve transitions, mmu_gather RCU-defer race. Tracked as
   future SMP-T26.

3. **SYSCALL instruction length assumption**: the fix assumes
   SYSCALL is 2 bytes (0F 05). If a future workload uses a REX-
   prefixed SYSCALL (4F 0F 05, 3 bytes), the rewind to HOST_CX-2
   would land mid-instruction. Defensive: peek user memory at
   HOST_CX-2..HOST_CX-1 for the 0F 05 sequence; if not found, fall
   back to leaving HOST_IP unchanged and let the dispatch loop
   re-attempt. Defer until a workload demonstrates the issue.

## Lessons (for future state-audit Layer 5 toolkit additions)

1. **Auto-freeze ring on hypothesis-specific triggers, not generic
   ones.** BUG_PR (high-cr2 user fault) freezes on init.sh demand-
   paging too, masking real BUG_B captures. T24 narrowed BUG_B's
   trigger to the kernel-half range and removed BUG_PR's auto-freeze
   — that's how we finally captured pid=296's full dispatch history.

2. **Look at the FULL trace, not just the few seqs around the
   trigger.** The corruption happened at seq 5572647 (4144 dispatches
   BEFORE the fault at 5576797). Only by grepping for pid=296 in
   the entire 3.8MB dump did the mid-LSTAR EINTR pattern become
   visible.

3. **Marshal direction matters for EINTR.** marshal_from_kvm_regs
   exists to "snapshot the guest's just-interrupted state." But for
   the LSTAR trampoline, "just-interrupted state" includes the
   transient kernel-half RIP that USER mode must never see. Special-
   case the kernel-half RIP range, just like we already special-
   case IDT handler stubs.
