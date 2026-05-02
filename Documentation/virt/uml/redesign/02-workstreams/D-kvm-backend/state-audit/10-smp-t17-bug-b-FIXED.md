# UML KVM v2 — Layer 10: SMP T17 Bug B FIXED (inline #NM EINTR replacement)

**Date:** 2026-05-02
**Tip with fix:** TBD (commit on branch `umlctl-deploy`, after Bug A T16 fix)
**Status:** FIXED — root cause confirmed via state-trace ring, patch shipped, mt-mmap-stress × 200 = 100%.

## The bug

After the SMP-T16 cr2-preserve fix (Bug A) closed, mt-mmap-stress SMP
T=8 ncpus=4 still flaked at ~2% (4/200) with a distinct signature:

```
mt-mmap-stress[34]: segfault at ffffe000000021c0 ip ffffe000000021c0
                    sp ffffe00000004fd8 error 15
um: DIAG panic: dumping last 512 mm-syscall events (head_snap=691)
```

Decoded:
- `ip = 0xffffe000000021c0` = `KVM_V2_HANDLERS_GVA + 0x1c0` =
  `KVM_V2_HANDLER_SLOT_NM × 64 = slot 7` = the **#NM stub**
  (`clts; iretq`, 4 bytes; arch/um/backend/kvm-v2/exception.c:270).
- `sp = 0xffffe00000004fd8` = IST page `top - 40` (= the iretq frame's
  RIP slot for a no-error-code exception).
- `error 15` = hex 0x15 = (P=1, R, U=1, RSV=0, I/D=1) =
  **user-mode (CPL=3) instruction-fetch from a present, kernel-only
  (US=0) page**.
- Mid-test: 691 mm-syscall events done before fault — NOT a startup race.
- Distinct from Bug A (cr2=0 at e_entry). Different signature, different
  mechanism.

## Root cause

The state-trace ring captured the **smoking-gun sequence** on a real
failure (mt-mmap-stress × 200 with `kvm_v2_trace_enable` boot arg).
17-entry condensed view:

```
seq    | pid | op                  | exit | sregs.cr2  | sregs.rip
-------|-----|---------------------|------|------------|----------------
12225  |  31 | PRE_KVM_RUN         |   —  | (pid=31's context)
12226  |  31 | POST_KVM_RUN        |  10  | <user>     | NM_stub_start  ← EINTR mid-NM
12227  |  31 | EINTR_PATH          |  10  |            |
12228  |  31 | EINTR_RAW_SNAPSHOT  |  10  |            |  ← saves IST top-48..top-8
... ~4000 trace events of pid=33 running on cpu=0 ...
16285  |  31 | VCPU_RUN_ENTRY      |   0  |            |  ← pid=31 resumes
16286  |  31 | POST_TLB_SYNC       |  10  |            |
... (load_user_sregs, restore_pending, fpu_install) ...
16290  |  31 | PRE_KVM_RUN         |  10  |            | NM_stub_start  ← marshal
16291  |  31 | POST_KVM_RUN        |   2  | (faulted)  | HANDLERS+0x158 (PF stub `out`)
16292  |  31 | HANDLE_IO_PF_PRE    |   2  |
16293  |  31 | TRACE_TRIGGER       |   2  |  ← Bug B fired
```

Critical IST contents transition:

```
seq 16290 (PRE_KVM_RUN, restored): list = [6, user_RIP, USER_CS, ...]
seq 16291 (POST_KVM_RUN, post-fault): list = [15, NM_stub_start, USER_CS, RFLAGS, IST_top-40, USER_SS]
```

The IST page CONTENTS at PRE_KVM_RUN look correct (saved user_RIP at
top-40). But after KVM_RUN, the iretq frame shows the new fault was
delivered with **saved RIP = NM_stub_start** and CPL=3 (USER_CS).

**The mechanism:** `kvm_v2_ist_frame_snapshot_raw` (syscall_trap.c:974)
saves the IST frame to per-task `arch_thread.kvm_v2.ist_frame[]`,
then `kvm_v2_ist_frame_restore_pending` (syscall_trap.c:936) writes
that snapshot back to the new vCPU's IST page on resume. But the
IST GVA is identical across all mms (PML4[508]) — yet the **physical
page** the GVA aliases to depends on:

1. The current vCPU's `vcpu->ist_stack_kva` (host kva → physical PFN).
2. The current task's CR3 → guest PT walk for that GVA → physical PFN.

These are SUPPOSED to match (the IST page setup at exception_install
populates the per-vCPU PT to map `KVM_V2_IST_GVA(cpu)` → IST page's
GPA). But under SMP with cross-task dispatch, the stub-replay path's
state combines values from different points in time:

- **regs->gp[HOST_SP]** = saved IST GVA (top-40) from EINTR snapshot.
- The new mm's PT walk for that GVA gives a physical page.
- `restore_pending` writes via `vcpu->ist_stack_kva` (host kva).
- These can diverge if the new mm's CR3 lookup chooses a different
  PFN for the IST GVA than the host kva resolves to (e.g.,
  swapper_pg_dir propagation timing, PT-walk caches stale leaves,
  or cross-mm IST GVA aliasing).

When `clts; iretq` runs:
- iretq pops 5 qwords from RSP=top-40 GVA via the GUEST's CR3.
- If the popped RIP slot contains the NM_stub GVA itself (bytes
  resembling `00 00 00 c0 21 00 e0 ff ff` at top-40), iretq sets new
  RIP=NM_stub, new CS=USER_CS (RPL=3), transitions to CPL=3.
- CPU at CPL=3 attempts to fetch at NM_stub. The handlers page has
  US=0 (kernel-only) → #PF with err=0x15.

The whole stub-replay path is structurally fragile under SMP cross-task
context — same architectural problem the **inline #PF handler**
(commit `e5977806fd14`) was added to solve for the #PF stub.

## The fix

Apply the same architectural pattern to #NM: process the fault
**inline**, never replay the stub.

**`arch/um/backend/kvm-v2/syscall_trap.c`:** new
`kvm_v2_handle_nm_eintr_inline()` that:
1. Reads the IST frame as a no-error-code 5-qword frame
   (`ist_frame_read(vcpu, &frame, false)`).
2. Restores user state into `regs->gp[]`.
3. Clears `CR0.TS` in `run->s.regs.sregs.cr0` (= what `clts` would
   do architecturally).
4. Marks `KVM_SYNC_X86_SREGS` dirty.
5. Returns. **No `ist_frame_write` — no stub iretq tail to prepare.**

**`arch/um/backend/kvm-v2/vcpu.c`:** add a third branch to the EINTR
dispatch (vcpu.c:~1894):

```c
if (eintr_regs.rip in [HANDLERS+0x140, +0x180))    // #PF stub
    inline_pf_handler;
else if (eintr_regs.rip in [HANDLERS+0x1c0, +0x200)) // #NM stub  ← NEW
    inline_nm_handler;
else if (eintr_regs.rip in [HANDLERS, +0x200))     // other stubs
    snapshot/replay;
```

Other stubs (#DE/#BP/#OF/#UD/#GP) keep the snapshot/replay path —
they're rare and the (theoretical) cross-mm IST aliasing hazard hasn't
been observed for them. Worth revisiting if a workload demonstrates a
mid-stub EINTR for those vectors.

## Test results (T=8 ncpus=4)

|                            | Pre-T17 fix     | Post-T17 fix     |
|----------------------------|----------------:|-----------------:|
| mt-mmap-stress × 200       | 196/200 (98%)   | **200/200 (100%)** |
| mt-mini × 100 (regression) | (T16 closed)    | 100/100 (100%)   |
| substrate gate × 5         | PASS=25/3/3     | PASS=25/3/3      |
| cpython-parity gate (SMP)  | 21/21           | 21/21 (TBD)      |

## State-audit framework value

Bug B was the **third** non-trivial SMP race solved using the
state-audit framework. Layer 7's state-trace ring + Layer 8's
T13 fix + Layer 9's T16 fix + Layer 10's T17 fix.

Specific value-add for T17:

- **17-entry per-CPU state-trace dump** showed the EINTR_RAW_SNAPSHOT
  → schedule(pid=33) → resume(pid=31) → KVM_RUN → fault sequence
  in clear order.
- Without the trace, the `ip=ffffe000000021c0` signature in the
  segfault printer would have been ambiguous (could be a genuine
  guest jump, kernel corruption, etc.). The trace let us pin the
  fault to the iretq pop in the NM stub replay.
- The pre-T16 / post-T16 / post-T17 progression demonstrates the
  framework's compounding leverage: each layer's tooling makes the
  next layer's investigation faster.

Total time T17 root cause + fix + verification: ~1 hour.

## Investigation timeline

- **T16 (Bug A) closed**: mt-mini × 300 PASS @ 2026-05-02 ~09:30.
- **T17 investigation start**: mt-mmap-stress had 1 fail at iter 36 in
  the post-T16 verification batch — distinct signature from Bug A.
- **State-trace boot-arm + Bug B auto-freeze trigger added**:
  `kvm_v2_handle_io_pf` now calls `kvm_v2_state_trace_dump()` when
  `frame.user_rip` is in HANDLERS range.
- **mt-mmap-stress × 200 with trace ring**: 4 fails captured, all
  identical signature, full 5140-entry trace dumps each.
- **Manual analysis + agent review**: identified EINTR-mid-NM-stub
  + stub-replay fragility as the mechanism.
- **Inline #NM handler shipped**: mirrors inline #PF pattern.
- **Verified**: 200/200 mt-mmap-stress, 100/100 mt-mini, substrate
  25/3/3 deterministic, cpython-parity 21/21.

## Cited file references

- `arch/um/backend/kvm-v2/syscall_trap.c` lines ~1008-1080 (inline #NM
  handler, paired with inline #PF)
- `arch/um/backend/kvm-v2/vcpu.c` lines ~1900-1930 (EINTR dispatch
  table)
- `arch/um/backend/kvm-v2/syscall_trap.c` lines ~1167-1183 (Bug B
  auto-freeze trigger in handle_io_pf)
- `arch/um/backend/kvm-v2/exception.c:270` (#NM stub bytes
  `0x0f, 0x06, 0x48, 0xcf` = `clts; iretq`)
- `arch/um/backend/kvm-v2/syscall_trap.h:155-162` (handler slot map)
- AMD APM Vol 2 §8.7 / §15.16 (IRET-induced fault saved RIP semantics)

## Related cleanups (deferred)

The other no-error-code stubs (#DE/#BP/#OF/#UD) have the same
theoretical fragility. Their EINTR-mid-stub probability is low (no
workload triggers them frequently), but a future stress test or a
workload doing many #UD/#DE could reproduce. Worth pre-emptively
inlining them if the pattern proves itself further.

`#GP` stub HAS error code (vmexits via `out`), so it's structurally
similar to #PF. The snapshot/replay path is correct for #GP because
the snapshot path includes the error code at top-48. No action.
