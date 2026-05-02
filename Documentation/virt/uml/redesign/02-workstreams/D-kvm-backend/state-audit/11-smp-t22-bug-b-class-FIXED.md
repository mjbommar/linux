# UML KVM v2 — Layer 11: SMP T22 — Bug B class CLOSED via vmexit-on-#NM

**Date:** 2026-05-02
**Tip with fix:** `ddf3cfe5cf31` (branch `umlctl-deploy`)
**Status:** SHIPPED — eliminates the entire "iretq pops kernel-half RIP from corrupted IST top-40" surface for #NM. Empirical: threaded-subprocess-wait.py × 20 baseline 14/20 → with T19+T20+T22 17-18/20 with NM_stub+2 = 0 in dmesg. All other gates (mt-mini SMP T=4 30/30, mt-mmap-stress SMP T=8 PASS, substrate 25/3/3, cpython-parity 21/21 PARITY) regression-free.

## Predecessors

- **Layer 10 / SMP-T17** closed the *EINTR-mid-NM-stub* variant of Bug B
  via the inline `kvm_v2_handle_nm_eintr_inline` host-side handler at
  `vcpu.c:1905-1930`. That fix bypassed the in-guest stub iretq when
  EINTR caught us mid-stub, eliminating the snapshot-replay write
  back into a possibly-CR3-aliased IST page.
- **SMP-T19** added defensive sanity guards on three IST writers
  (snapshot_raw, restore_pending, ist_frame_write) refusing kernel-half
  RIPs / CPL=0 frames. Improved threaded-subprocess-wait residual but
  did not close it.
- **SMP-T20** switched `um_mmu_gather_drain` to `call_rcu`-deferred
  page-free per the multi-agent investigation Angle 2's recommendation.
  Architecturally sound (pages return to buddy only after every vCPU
  has dispatched at least once and flushed its guest TLB) but did not
  visibly help threaded-subprocess-wait.

## The residual bug post-T19+T20

Same `python3[N]: segfault at 0 ip ffffe000000021c2 sp ffffe00000004fd8
error 20` signature persisted at ~3-5/20. Decoded:

- `ip = 0xffffe000000021c2 = NM_stub + 2 = the iretq instruction` of the
  in-guest #NM stub (`clts; iretq`, 4 bytes).
- `sp = 0xffffe00000004fd8 = IST top - 40` = the iretq frame's RIP slot.
- `error 0x14 = (P=0, US=1, I/D=1)` = user-mode instruction-fetch from
  a not-present page.

Decoded mechanism: hardware delivered #NM to a CPL=3 task, pushed the
iretq frame to IST top-40 with the user RIP/CS, the in-guest stub
ran `clts` then `iretq`. The iretq POPPED a kernel-half RIP from
top-40 (instead of the user RIP hardware had pushed there). The
kernel-half RIP combined with a user-CPL CS RPL=3 in top-32 produced a
CPL=3 instruction-fetch at NM_stub+2 — which is mapped only kernel-only
in user-CR3, hence P=0.

The empirical signal that this bug class is timing-sensitive:

- With `kvm_v2_trace_enable` boot arg (state-trace ring overhead ~2us
  per dispatch × 18 ops/dispatch = ~36us added latency), 14/15 PASS.
- Without state-trace overhead: ~6/20 FAIL.
- Write-side guard (T19c) on `kvm_v2_ist_frame_write` never fires
  (KERN_RIP_HITS=0 over 20 boots) — the corruption is NOT via host
  software writes to IST top-40.

That left two candidate writers of the kernel-half RIP into IST top-40:

- Hardware IDT push *during* the in-flight stub execution from a
  nested fault (would require the stub itself faulting — but `clts`
  and `iretq` don't fault).
- TDP / page-recycling at the IST page level — a stale TLB or the
  guest TDP entry for the IST GVA temporarily pointing at a recycled
  physical page whose contents look like a kernel-half RIP.

Either way, the **mechanism that surfaces the corruption is the
in-guest iretq from IST top-40**. Eliminating that iretq closes the
surface independent of which writer is at fault.

## The fix (SMP-T22)

Replace the #NM stub bytes from `clts; iretq` (`0f 06 48 cf`) with
`out %al, $UM_KVM_TRAP_NM ; iretq` (`e6 fd 48 cf`). The iretq tail is
now UNREACHABLE — `out` causes vmexit on the first instruction.

Host-side handler `kvm_v2_handle_io_nm` (at `syscall_trap.c`, mirroring
the existing `kvm_v2_handle_nm_eintr_inline` pattern):

```c
kvm_v2_ist_frame_read(vcpu, &frame, false /* no error_code */);
regs->gp[HOST_IP]     = frame.user_rip;
regs->gp[HOST_SP]     = frame.user_rsp;
regs->gp[HOST_EFLAGS] = frame.user_rflags;
regs->is_user         = 1;
run->s.regs.sregs.cr0 &= ~X86_CR0_TS;             /* host clts */
run->kvm_dirty_regs   |= KVM_SYNC_X86_SREGS;
current->thread.arch.kvm_v2.nm_ts_bypass = true;  /* one-shot */
interrupt_end();
kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
run->kvm_dirty_regs   |= KVM_SYNC_X86_REGS;
```

Same flow as #PF/#GP/#UD/#DE/#OF: no in-guest iretq from IST, no
opportunity for the iretq to pop a corrupted frame.

## The lazy-FPU coexistence problem (and the one-shot bypass)

Phase H.2's `kvm_v2_load_user_sregs` arms `CR0.TS = 1` on EVERY dispatch
so the first FP instruction in user code triggers #NM (lazy-FPU
detection). Without the in-guest stub clearing TS, my host-side TS
clear in `handle_io_nm` would be undone before the user retries the FP
instruction:

1. Dispatch 1: load_user_sregs arms TS=1. KVM_RUN. User FP → #NM →
   `out` → vmexit → handle_io_nm: clear sregs.cr0.TS, set HOST_IP =
   user_rip. Return.
2. Dispatch 2: load_user_sregs arms TS=1 *again* (TS arming is
   unconditional). KVM_RUN at HOST_IP=user_rip with TS=1. User FP
   faults *again* → #NM → vmexit → ... INFINITE LOOP. Boot hangs at
   first FP-using process (which is essentially everything that links
   glibc).

Fix: a one-shot `arch_thread.kvm_v2.nm_ts_bypass` flag. `handle_io_nm`
sets it true. `load_user_sregs` checks it: if set, SKIP the TS arm
AND clear the existing TS bit in sregs.cr0, then clear the flag.

Result: dispatch 2 enters with TS=0, user FP succeeds, user runs until
it vmexits via syscall (or another exception). Dispatch 3 (after the
syscall handler) arms TS=1 normally — so the *next* FP burst still
trips one #NM each, preserving the lazy-FPU detection.

The same flag is set by `kvm_v2_handle_nm_eintr_inline` (the EINTR
variant) for consistency — that path had the same theoretical
problem but rare enough to not surface in practice.

## Empirical results

Test: `tools/testing/selftests/um/fork-tree-3level/repros/threaded-subprocess-wait.py`,
2 Python threads × 200 iters of subprocess.Popen([sys.executable, "-c",
"import time; time.sleep(0.001)"]) + wait. Run via umlctl with
`backend=kvm-v2 ncpus=4 mem=2048M` — the canonical SMP fork-stress
pattern that mirrors regrtest worker spawning.

| Build | PASS/20 | NM_stub+2 in dmesg | Other fails |
|---|---|---|---|
| Pre-fix baseline (`fefdc8666961`) | 14/20 | 5/8 of fails | 3/8 of fails |
| T19 + T20 only (`d9ed9e14c7b6`) | 14-17/20 | still dominant | unchanged |
| T22 (this commit, `ddf3cfe5cf31`) | 17-18/20 | **0** | residual = page-recycling + worker rc=1 |

Regression gates (all PASS post-T22):

| Gate | Result |
|---|---|
| mt-mini SMP T=4 ncpus=4 × 30 | 30/30 |
| mt-mmap-stress SMP T=8 ncpus=4 | PASS |
| substrate (regrtest-repros) | 25/3/3 |
| cpython-parity (21 modules) | 21/21 PARITY |

## Files touched

- `arch/um/backend/kvm-v2/syscall_trap.h` — add `UM_KVM_TRAP_NM = 0xfd`
- `arch/um/backend/kvm-v2/exception.c` — change NM stub bytes
- `arch/um/backend/kvm-v2/syscall_trap.c` — add `kvm_v2_handle_io_nm`,
  add UM_KVM_TRAP_NM dispatch case, set `nm_ts_bypass` in inline EINTR
  handler
- `arch/um/backend/kvm-v2/vcpu.c::kvm_v2_load_user_sregs` — respect
  `nm_ts_bypass` one-shot
- `arch/x86/um/asm/processor_64.h::arch_thread.kvm_v2` — add
  `nm_ts_bypass` field; reset in `arch_flush_thread` and
  `arch_copy_thread`

LoC: 148 lines added / 15 changed across 5 files.

## Performance

#NM is rare — empirics from `mt-byteset` (vcpu.c:1793 comment) show
~5% of dispatches use FP (`fpu_taken ≈ 5%, fpu_skipped ≈ 95%`). T22
adds one vmexit per #NM. Dwarfed by the FPU instruction itself
(usually 10s-100s of cycles). Net cost: well under 1% wall-clock.

## Why this won't regress

- NM behavior is functionally identical: TS=1 on entry, clear TS on
  first FP instruction, FPU usable until next dispatch.
- The one-shot bypass is post-#NM only. Non-#NM exits arm TS normally.
- Lazy-FPU detection (skip GET_FPU when sregs.cr0.TS stays 1 after
  vmexit) still works — `nm_ts_bypass` only affects the NEXT dispatch's
  arm, not the post-vmexit GET decision.

## Residual work

The remaining 2-3 fails per 20 boots in threaded-subprocess-wait are:

1. **High-cr2 user faults** (e.g., `segfault at 510b520 ip 0x6b0673 error 4`).
   Page-recycling-class. SMP-T20's RCU-deferred page free was
   architecturally correct but did not visibly help. Either the
   call_rcu callback fires too late or the bug is at a different layer
   (TDP / mm_id propagation under fork-stress).

2. **Python `fails=1` with no kernel signature**. A worker subprocess
   returns rc != 0 silently. Root cause unknown — could be a child
   Python tripping a fault we don't surface as a signal. Need to add
   diagnostics that catch the worker rc and dump its dmesg around the
   exit time.

These are the targets for SMP-T23+.
