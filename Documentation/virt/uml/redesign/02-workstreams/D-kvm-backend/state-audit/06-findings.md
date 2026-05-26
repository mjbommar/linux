# UML KVM v2 — Layer 6: Findings & Decision Tree

**Created:** 2026-05-01
**Tip at audit:** `7acce0471395`
**Inputs:** [00-overview.md](00-overview.md), [01-state-inventory.md](01-state-inventory.md),
[02-operations-inventory.md](02-operations-inventory.md), [04-suspect-register-audits.md](04-suspect-register-audits.md).

Punch-list for the next session. Symptom: mt-mini T=8 ncpus=4 fails ~50%
(VERIFY_FAIL got=0 expect=tid; SIGSEGV @ low addr in slow_memset;
MMAP_NULL; cross-thread RBP/RSP swap; user_rip in DATA pages). Layer-4
ruled out marshal-side cross-task contamination. Layer-1 surfaced 11
"Notable observations." This document scores them, defines the
definitive empirical test for the leading hypothesis (cross-vCPU stale
guest TLB), and gives a fix-attempt order with pre-evaluated trade-offs.

---

## Section A — Specific bug candidates

Numbering matches L1's "Notable observations" list (01-state-inventory.md:257).

### A1. Cross-task IST/TSS sharing (L1 obs #1)
**Candidate.** `vcpu->ist_stack_kva` (kvm_v2_backend.h:324) and
`vcpu->tss_kva` (kvm_v2_backend.h:327) are physically per-host-CPU,
shared by every UML task on that pool entry. Snapshot/restore via
`arch_thread.kvm_v2.ist_frame[]` (processor_64.h:32-58) is the only
isolation.

**Plausibility for mt-mini.** HIGH for IDT-frame symptoms; LOWER for
the data-page symptoms. #121 already closed the EINTR-mid-IDT-delivery
gap (vcpu.c:1216-1218 saved_cr2 + syscall_trap.c:967 raw IST snapshot).
Residual gap from L2: `kvm_v2_ist_frame_write` (syscall_trap.c:875)
reads CS/SS LIVE from the IST page (lines 893, 896) AFTER
`segv_handler`/`interrupt_end` may have yielded
(syscall_trap.c:1205,1237) — another task on this CPU could have
overwritten the page. This would corrupt CS/SS in the iretq frame, not
arbitrary user RBP/RSP. Does NOT cleanly explain mt-mini.

**Empirical test.** Add a `pr_emerg_ratelimited` in
`kvm_v2_ist_frame_write` (syscall_trap.c:893-896) that prints
`(pid, cpu, cs_read, ss_read)` and compares against expected
(0x33, 0x2b). Bound to 30 hits.

**Effort if confirmed.** SMALL (~1 day). Cache CS/SS at fault entry
into `arch_thread.kvm_v2.ist_frame_seg` instead of re-reading the
shared page.

### A2. KVM_SYNC_X86_EVENTS not enabled (L1 obs #2)
**Candidate.** `kvm_valid_regs` only sets REGS|SREGS (vcpu.c:924-925).
Pending exception/interrupt/NMI/SIPI never round-trip.

**Plausibility for mt-mini.** LOW. v2 surfaces guest exceptions as
KVM_EXIT_IO via the trampoline-stub (backend.h:142-148). No exception
injection path is exercised. mt-mini does not use NMI/APIC/SMI.

**Empirical test.** Print `run->s.regs.events.exception.injected` and
`...interrupt.injected` after every KVM_RUN for one boot. If always 0,
not the bug.

**Effort if confirmed.** MEDIUM (~3 days). Wire EVENTS into
sync_valid_fields, define injection contract.

### A3. CR8 unmanaged (L1 obs #3)
**Plausibility for mt-mini.** ZERO. No APIC, no TPR-driven masking.
Defer indefinitely.

### A4. MSR_KERNEL_GS_BASE unmanaged (L1 obs #4)
**Plausibility for mt-mini.** ZERO. D.1 trampoline does not use `%gs:`
(vcpu.c:310-312). Static-glibc workloads don't touch KGS. Defer.

### A5. DR0–DR7 unmanaged (L1 obs #5)
**Plausibility for mt-mini.** ZERO. mt-mini sets no hw breakpoints.
Defer.

### A6. EFER double-written (L1 obs #6)
**Plausibility for mt-mini.** ZERO (correctness). Per-dispatch write
(vcpu.c:1307) is redundant with install (vcpu.c:520) but not wrong.
Pure performance.

**Effort.** TRIVIAL. Hoist out of `load_user_sregs` for Phase H.

### A7. CR2 has 3 writers (L1 obs #7)
**Candidate.** Three potential per-dispatch writers of `sregs.cr2`:
(a) per-dispatch zero (vcpu.c:1220), (b) saved_cr2_at_eintr restore
(vcpu.c:1217), (c) prior-exit SYNC round-trip residue.

**Plausibility for mt-mini.** MEDIUM. #121-D15 was exactly this class
of bug. Current code reads "zero unless saved_cr2_valid" but relies on
no third path leaving sregs.cr2 non-zero. If that invariant breaks
(e.g., post-exit handler doesn't clear), a stale CR2 enters guest →
PF stub mis-dispatch → arbitrary register state. This is
guest-visible only on a subsequent #PF, but mt-mini's slow_memset is
PF-heavy.

**Empirical test.** Add `pr_emerg` in `load_user_sregs` (vcpu.c:1216)
when `sregs->cr2 != 0 && !saved_cr2_valid` BEFORE the assignment
(catch the residue path). Bound to 30 hits.

**Effort if confirmed.** TRIVIAL. Always zero before the conditional
restore.

### A8. current_mm not RCU (L1 obs #8)
**Plausibility for mt-mini.** LOW. Per backend.h:351-356 the read in
the kicker is advisory: stale read = unneeded IPI, bounded by
`kick_pending` cmpxchg dedup. No use of mm contents. mt-mini is
single-mm anyway.

**Effort if needed.** MEDIUM (RCU-protect requires sync at exit_mmap).

### A9. cpuid_primed shared sticky state (L1 obs #9)
**Plausibility for mt-mini.** ZERO. First-run install is correct
under v2's single-VM model; CPUID is identical for all tasks. Defer.

### A10. Per-mm tlb_gen vs per-vCPU last_seen split (L1 obs #10)
**Candidate.** `mm->context.tlb_gen` per-mm (mmu.h:64-80);
`vcpu->last_seen_tlb_gen` per-vCPU (backend.h:341). The split itself
is correct (each vCPU's hw TLB needs its own flush). The BUG is that
the kicker is wired (vcpu.c:820+) but the activation in `um_tlb_sync`
is COMMENTED OUT (tlb.c:511-519). `tlb_gen` IS bumped (tlb.c:512) but
no kick fires, so other vCPUs' TLBs stay stale until their own next
dispatch.

**Plausibility for mt-mini.** HIGHEST. This is the L4 smoking-gun
hypothesis (04-suspect-register-audits.md:340-379). A vCPU running mm
M with stale guest TLB after another vCPU drained PTEs in M will load
wrong physmem PFNs into ANY user register/memory operand — explains
RBP/RAX/RDX wrong simultaneously, MMAP_NULL, user_rip in data pages
(iretq pops corrupt RIP from a stale-TLB stack page).

**Empirical test.** See Section B.

**Effort if confirmed.** Activation is one line (uncomment
tlb.c:517-518). Tuning to avoid IPI storm regression: see Section C.

### A11. Per-task ist_frame/iotrap_fpu over per-vCPU pages (L1 obs #11)
Same class as A1 (snapshot/restore must bracket every exit/entry).
#121 closed the known gap. No new evidence.

### Cross-cutting from L2: handle_io_pf yield window
**Candidate.** `kvm_v2_handle_io_pf` (syscall_trap.c:1052) calls
`segv_handler` → `handle_mm_fault` → may sleep (1205); calls
`interrupt_end` (1237) → may schedule. Between the yield and the
post-handler `kvm_v2_ist_frame_write` (1254), another task on the
same CPU can dispatch on this vCPU and overwrite the IST page, then
our `ist_frame_write` reads CS/SS from those clobbered slots
(lines 893, 896).

**Plausibility for mt-mini.** MEDIUM. Symptom would be wrong CS/SS in
the iretq frame, leading to #GP on iretq or guest CPL change.
Distinct from but in the same class as A1.

---

## Section B — Cross-vCPU TLB stale: definitive empirical test

The single-largest hypothesis. Must be PROVEN or REFUTED before another
fix attempt.

### Step 1 — Instrumentation patch

In `kvm_v2_load_user_sregs` (vcpu.c around line 1273-1275), BEFORE the
`atomic64_set(&vcpu->last_seen_tlb_gen, ...)`:

```c
if (current->mm) {
    u64 cur_gen = atomic64_read(&current->mm->context.tlb_gen);
    u64 last   = atomic64_read(&vcpu->last_seen_tlb_gen);
    if (cur_gen >= last + 3) {
        static atomic_t hits = ATOMIC_INIT(0);
        if (atomic_inc_return(&hits) <= 30) {
            pr_emerg("KVM_V2_TLB_LAG cpu=%d pid=%d mm=%px last=%llu cur=%llu lag=%llu\n",
                     vcpu->cpu, current->pid, current->mm,
                     last, cur_gen, cur_gen - last);
        }
    }
}
```

Bounded counter (30/boot) — so even an IPI-storm-equivalent volume of
prints does not collapse the boot.

### Step 2 — Run

Build with the patch, run the existing mt-mini failing config:

```
mt-mini T=8 ncpus=4 N=20
```

Capture `dmesg` after the run. Note correlation between TLB-LAG
appearances and per-iter `pass`/`fail` outcome.

### Step 3 — Predictions

| Outcome                                                       | Conclusion                                                           |
|---------------------------------------------------------------|----------------------------------------------------------------------|
| TLB-LAG fires often, every FAIL iter shows ≥1 lag ≥3          | **CONFIRMED**: cross-vCPU stale guest TLB. Proceed to Section C.     |
| TLB-LAG fires often, but uncorrelated with FAIL iters         | TLB lag is normal; bug is elsewhere. Proceed to Section D priorities. |
| TLB-LAG never fires (gen never lags by 3+)                    | gen-counter caught up promptly via natural CR4.PGE; bug is elsewhere. Proceed to Section D. |
| TLB-LAG fires only on PASS iters                              | Anti-correlation = TLB lag is healthy churn. Bug is elsewhere.       |

### Step 4 — Sharper variant if Step 3 is ambiguous

If lag fires but correlation is unclear, add a SECOND pr_emerg in
`kvm_v2_handle_io_pf` (syscall_trap.c around line 1187, after
`regs->faultinfo` is populated) that prints `(pid, cpu, error_code,
cr2, user_rip)` for the FIRST fault per pid per boot whose
`cr2 < 0x1000` (the SIGSEGV-low-addr signature). Cross-correlate with
TLB-LAG hits on the same `(cpu, pid)`. If a low-addr fault always
follows a TLB-LAG on the same vCPU within ≤2 dispatches, confirmed.

---

## Section C — If TLB stale is confirmed: fix options ranked

Previous activation regressed T=4 from 60/60 to 18/60-46/60 (commented
at tlb.c:493-498). The IPI itself disrupts forward progress. The fix
must preserve cross-vCPU invalidation WITHOUT IPI storm.

### C1. Throttled kick — max one per N µs per vCPU
**Mechanism.** Add `vcpu->last_kick_ns` (per-vCPU). Kick only if
`now - last_kick_ns > THRESHOLD` (e.g., 100 µs).
**Pro.** Trivial. Bounds IPI rate.
**Con.** Throttle period creates a window where stale TLB persists.
mt-byteset's verifier runs in tight loops — even 100 µs of staleness
on a CoW page is ~10K loop iters of wrong data.
**Verdict.** Risky. Likely trades one failure mode for another.

### C2. Delayed kick — queue, fire only after N drains accumulate
**Mechanism.** `mm->context.tlb_gen` bumped per drain; kick only when
`gen % N == 0`.
**Pro.** Reduces IPI count by N.
**Con.** Same staleness window as C1, sized by drain frequency.
**Verdict.** Worse than C1 (no time bound, just count bound).

### C3. CR4.PGE toggle on syscall return path (no kick)
**Mechanism.** Each vCPU's `kvm_v2_load_user_sregs` already toggles
CR4.PGE on every dispatch (vcpu.c:1259). `kvm_v2_handle_io_trap`
syscall arm (syscall_trap.c:1693+) marshals back to the mmap and
KVM_RUNs again — the next dispatch's load_user_sregs will toggle PGE
naturally. So the ONLY case where this fix changes anything is when a
vCPU is blocked in KVM_RUN and never naturally exits.
**Pro.** Zero cost. No IPI.
**Con.** Doesn't actually help: a vCPU stuck in KVM_RUN executing user
code with stale TLB will not exit until SIGALRM (10 ms) or its own
fault. The ENTIRE PROBLEM is that other vCPUs' TLB needs flushing
NOW, not on their next natural exit.
**Verdict.** Not actually a fix for the stated hypothesis. Equivalent
to C4.

### C4. Rely on SIGALRM — vCPUs naturally exit every 10 ms
**Mechanism.** Do nothing. SIGALRM (vcpu.c:1717 EINTR path) yanks
every vCPU out of KVM_RUN every 10 ms; their next dispatch's
CR4.PGE toggle flushes.
**Pro.** Zero code change, zero IPI.
**Con.** 10 ms staleness window. mt-mini's verifier loop is faster.
**Verdict.** This is the CURRENT BEHAVIOR (the bug).

### C5. Cross-vCPU shared gen-counter polled at vmexit
**Mechanism.** Place `mm->context.tlb_gen` in a shared region readable
from guest. At every vmexit (syscall_trap.c:1693 marshal back), check
`tlb_gen vs last_seen` and if stale, force a re-entry detour through
`load_user_sregs` (CR4.PGE toggle).
**Pro.** No IPI. Each vCPU self-flushes on its NEXT vmexit (which
happens at every syscall — much more frequent than 10 ms).
**Con.** Only flushes on vmexit. A vCPU spinning in user code (no
syscall, no fault) still won't flush until SIGALRM. mt-mini's
slow_memset between sched_yield calls might fit this gap.
**Verdict.** Better than C1-C4. Solves the common case (syscall-heavy
workload) without IPI.

### C6. Hybrid: C5 + targeted SIGALRM acceleration
**Mechanism.** C5 by default. When `kvm_v2_tlb_kick_others` would
fire, instead of pthread_sigqueue(IPI_SIGNAL), call into the host
timer to schedule an early SIGALRM on the target cpu_thread (or just
let it fire naturally). For SMP-with-mm-churn workloads, the existing
SIGALRM cadence (1 ms/10 ms via `arch/um/os-Linux/signal.c`) may
suffice if the timer is tightened.
**Pro.** Zero IPI cost; uses existing yank mechanism (SIGALRM EINTR
is well-tested by #121).
**Con.** Tightening SIGALRM increases overall preemption cost.
**Verdict.** WORTH PROTOTYPING. Tighten timer to 1 ms during the
test, see if mt-mini stabilizes.

### Recommended order
1. **C6** — tighten host timer to 1 ms (arch/um/os-Linux/signal.c
   timer-real interval), measure mt-mini PASS rate. If PASS → done.
2. **C5** — wire vmexit-time gen check (no shared mmap needed; check
   `mm->context.tlb_gen vs vcpu->last_seen_tlb_gen` at end of
   `handle_io_trap` syscall arm; if stale, set a flag that
   `load_user_sregs` honors next dispatch).
3. **C1** — throttled IPI as last resort.

---

## Section D — Other bug candidates to investigate in parallel

Even if A10 (cross-vCPU stale TLB) is the primary bug, these may
compound. Priority for sequential investigation if A10 alone doesn't
restore PASS=N/N:

| Pri | ID  | Bug                                                | Effort |
|-----|-----|----------------------------------------------------|--------|
| P1  | A7  | CR2 third-writer residue                           | TRIVIAL|
| P2  | L2-cross | handle_io_pf yield-window IST clobber         | SMALL  |
| P3  | A1  | ist_frame_write reads live shared CS/SS            | SMALL  |
| P4  | A6  | EFER double-write — pure perf, but proves discipline | TRIVIAL |

P1 (CR2) is cheap to instrument and fix; do alongside A10's
TLB-LAG patch. P2/P3 require more careful synchronization and should
wait until A10 is settled.

---

## Section E — Decision tree for next session

```
START
  │
  ▼
[1] Apply Section B Step 1 instrumentation (TLB-LAG printk).
    Also apply A7 instrumentation (CR2-residue printk) — cheap, parallel.
    Build, boot, run mt-mini T=8 ncpus=4 N=20.
  │
  ▼
[2] Inspect dmesg.
  │
  ├── TLB-LAG fires often + correlates with FAIL iters
  │     → A10 CONFIRMED. Go to [3].
  │
  ├── TLB-LAG never fires OR uncorrelated with FAIL
  │     → A10 REFUTED. Go to [4].
  │
  ├── CR2-residue fires (sregs->cr2 != 0 entering load_user_sregs without saved_cr2_valid)
  │     → A7 CONFIRMED. Apply trivial fix (always zero before conditional restore),
  │       re-run. If PASS=N/N, done. If still FAIL, go to [3] or [4].
  │
  └── Both quiet → mystery still elsewhere. Go to [4].
  │
  ▼
[3] A10 CONFIRMED — try C6 first.
    Tighten arch/um/os-Linux/signal.c timer to 1 ms. Re-run.
      ├── PASS=N/N → record memo, ship.
      ├── PASS improves but not stable → try C5 (vmexit gen check). Re-run.
      │   ├── PASS=N/N → record memo, ship.
      │   └── still FAIL → try C1 (throttled IPI, 100 µs). Re-run.
      └── No improvement → C5 + C6 combined. If still FAIL, escalate to [4]
          (TLB lag is real but not THE bug — something else compounds).
  │
  ▼
[4] A10 not (or only partly) the bug. Investigate Section D in priority order.
    For each P_i:
      - Apply targeted instrumentation (printk at the suspect site).
      - Run mt-mini T=8 ncpus=4 N=20.
      - If correlation found → fix → verify.
      - If no correlation → next P_i.
  │
  ▼
END.
  Update [99-findings.md] (or this file) with what was confirmed/refuted.
  Update memo (e.g., docs/memos/30-gate-discipline.md) noting the
  test results in service of the gate's PASS=25/FAIL=3/EXPECTED_FAIL=3
  baseline.
```

---

## Cited file references

- arch/um/backend/kvm-v2/vcpu.c:820, 826-866 — `kvm_v2_tlb_kick_others`
- arch/um/backend/kvm-v2/vcpu.c:1216-1221 — saved_cr2 install (#121 fix)
- arch/um/backend/kvm-v2/vcpu.c:1259 — CR4.PGE toggle (per-vCPU local flush)
- arch/um/backend/kvm-v2/vcpu.c:1270-1275 — kick_pending ack + last_seen update
- arch/um/backend/kvm-v2/vcpu.c:1307 — EFER per-dispatch write (A6)
- arch/um/backend/kvm-v2/vcpu.c:1321 — CR0.TS arm (lazy FPU)
- arch/um/backend/kvm-v2/vcpu.c:1693-1696 — pre-unblock_signals snapshot
- arch/um/backend/kvm-v2/vcpu.c:1717-1801 — EINTR path
- arch/um/backend/kvm-v2/vcpu.c:1761-1776 — EINTR-mid-stub bypass / snapshot
- arch/um/backend/kvm-v2/vcpu.c:1814-1815 — marshal-from snapshot
- arch/um/backend/kvm-v2/vcpu.c:924-925 — `kvm_valid_regs = REGS|SREGS` (A2)
- arch/um/backend/kvm-v2/syscall_trap.c:875, 893, 896 — ist_frame_write live CS/SS read (A1, L2-cross)
- arch/um/backend/kvm-v2/syscall_trap.c:967 — ist_frame_snapshot_raw (#121 fix)
- arch/um/backend/kvm-v2/syscall_trap.c:1052, 1205, 1237 — handle_io_pf yield sites (L2-cross)
- arch/um/backend/kvm-v2/kvm_v2_backend.h:288-357 — struct kvm_v2_vcpu (per-host-CPU)
- arch/um/backend/kvm-v2/kvm_v2_backend.h:336, 341, 351-356 — kick_pending / last_seen_tlb_gen / current_mm
- arch/um/include/asm/mmu.h:64-80 — `mm->context.tlb_gen` definition
- arch/um/kernel/tlb.c:511-519 — gen-bump + COMMENTED-OUT kick activation (A10 root)
- arch/x86/um/asm/processor_64.h:32-58, 76-103 — per-task ist_frame, saved_cr2_at_eintr
