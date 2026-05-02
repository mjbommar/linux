# UML KVM v2 — Layer 9: SMP T16 Bug A FIXED (cr2-preserve across same-task re-entry)

**Date:** 2026-05-02
**Tip with fix:** TBD (commit on branch `uml-redesign-plan`)
**Status:** FIXED — root cause confirmed via state-trace ring, patch shipped, mt-mini × 300 = 100%.

## The bug

After the T13 `migrate_disable()` fix landed, mt-mini SMP T=8 ncpus=4 still
flaked at ~1% (2/200 reproducible across multiple sweeps). The fail
signature was identical every time:

```
um: kvm-v2 #PF[1] cr2=0 captured=0 stub_rdx=0 regs_rdx=0
    sentinel=ffffffffffffffff err=14 user_rip=40023340 pid=1
    comm=mt-init.XXXXXX.
mt-init.XXXXXX.[1]: segfault at 0 ip 0000000040023340 sp 0x...
    error 14 in ld-linux-x86-64.so.2[23340,40001000+2f000]
```

`user_rip=0x40023340` is **ld-linux-x86-64.so.2's e_entry exactly** (load
base 0x40000000 + ELF Entry 0x23340). `err=0x14` decodes to (P=0, R, U=1,
RSV=0, I/D=1) = **user instruction-fetch fault, page not present**.

Architecturally, instruction-fetch #PFs save `RIP = CR2 = the fault target`.
But we observed `RIP=e_entry, CR2=0` — a contradiction under normal
SVM semantics. The in-stub `mov %cr2, %rax` independently captured `0`
(captured_cr2=0), proving the architectural CR2 register really was 0
when the stub ran.

## Root cause

`kvm_v2_load_user_sregs` (arch/um/backend/kvm-v2/vcpu.c:1276) wrote
`sregs->cr2 = 0` **unconditionally** on every dispatch (except the
`saved_cr2_valid` rescue path for EINTR-mid-stub). Combined with
`KVM_SYNC_X86_SREGS` dirty (forced by the CR4.PGE toggle in the same
function), this writes `vcpu->arch.cr2 = 0` and `svm->vmcb->save.cr2 = 0`
on every VMRUN entry.

**The losing scenario** (captured by the state-trace ring on a real
failure, see `parse-trace.py` output below):

1. PRE_KVM_RUN at e_entry. cr2=0.
2. POST_KVM_RUN: `exit=10 (KVM_EXIT_INTR)`, `cr2=0x40023340`. **Hardware
   queued the #PF for IDT delivery** (CR2 written), but **SIGALRM EINTR
   preempted KVM** before the in-stub `mov %cr2, %rax` could capture it.
3. EINTR_PATH taken. `eintr_regs.rip` is in user code (e_entry, not in
   the stub-RIP range `[HANDLERS+0x140, +0x180)`), so the EINTR handler
   does NOT set `saved_cr2_valid=true`. The real fault address is
   recorded in the KVM mmap (`run->s.regs.sregs.cr2`) but UML side
   doesn't snapshot it.
4. Next dispatch. POST_TLB_SYNC: cr2 still=0x40023340 in the mmap.
   Then **POST_LOAD_SREGS: cr2=0** — `sregs->cr2 = 0` clobbered it.
5. Re-enter guest. Hardware re-fires the same #PF, briefly setting
   CR2=e_entry. But an internal NPF (nested page fault, e.g. on the
   stub page or IST page in the new mm's TDP context) intercepts
   between IDT delivery and the stub's `mov %cr2, %rax`. KVM's
   internal NPF re-entry reloads `vmcb.save.cr2 = vcpu->arch.cr2 = 0`,
   destroying the architectural CR2 the stub was about to read.
6. Stub captures CR2=0, vmexits via `out`. handle_io_pf reports a
   bogus NULL-deref at e_entry. segv_handler can't fix VA 0 → SIGSEGV
   → init dies → kernel panic.

## Empirical confirmation

`tools/testing/selftests/um/state-trace/` ring (state-audit Layer 7)
captured the smoking-gun trace. Key 17-entry dump from the failing
iteration:

```
seq | op              | exit | sregs.cr2 | sregs.rip
----|-----------------|------|-----------|----------
 5  | PRE_KVM_RUN     | —    | 0         | 0x40023340
 6  | POST_KVM_RUN    | 10   | 0x40023340| 0x40023340  ← HW set CR2 = e_entry
 7  | EINTR_PATH      | 10   | 0x40023340| 0x40023340  ← rip user-half, no rescue
 9  | POST_TLB_SYNC   | —    | 0x40023340| 0x40023340  ← KVM mmap still holds it
10  | POST_LOAD_SREGS | —    | 0         | 0x40023340  ← OUR ZERO CLOBBERED IT
13  | PRE_KVM_RUN     | —    | 0         | 0x40023340
14  | POST_KVM_RUN    | 2    | 0         | 0xffffe...2158  ← stub `out`; cr2 lost
```

The `cr2: 0x40023340 → 0` transition between seq=9 and seq=10 — both
observed entries on the SAME vCPU on the SAME host CPU for the SAME
task — IS the bug.

## The fix

`arch/um/backend/kvm-v2/vcpu.c`: only zero `sregs->cr2` on cross-task
transitions; same-task re-entry preserves whatever KVM left in the
mmap (the architectural CR2 from the prior fault).

```diff
+ } else if (vcpu->last_task != current) {
        sregs->cr2 = 0;
  }
+ vcpu->last_task = current;
```

`arch/um/backend/kvm-v2/kvm_v2_backend.h`: add `struct task_struct
*last_task;` to `struct kvm_v2_vcpu`.

`arch/x86/um/asm/processor_64.h`: also reset all `kvm_v2.*` fields
in `arch_flush_thread()` for hygiene (no observed bug, but stale
`iotrap_fpu_valid` / `ist_pending` / `saved_cr2_valid` across
execve was a latent risk).

## Test results (T=8 ncpus=4)

|                            | Pre-fix       | Post-fix      |
|----------------------------|--------------:|--------------:|
| mt-mini × 200              | 198/200 (99%) | **300/300 (100%)** |
| mt-yieldonly × 30          | 30/30  (100%) | 30/30 (100%)  |
| mt-rawmmap × 30            | 30/30  (100%) | 30/30 (100%)  |
| mt-mmap-stress × 60        | 59/60  (98%)  | 59/60 (98%) — Bug B unrelated |
| substrate gate × 5         | PASS=25/3/3   | PASS=25/3/3   |
| cpython-parity gate (SMP)  | 21/21         | 21/21 (TBD)   |

mt-mmap-stress's 1/60 fail has a different signature (`segfault at
ffffe000000021c0` — wild kernel-half jump, kernel-half RIP). Tracked
separately as Bug B / task #165.

## State-audit framework value

This bug was the second non-trivial SMP race solved using the
state-audit framework (Layer 1–8). Specifically:

- **Layer 6 toolkit** had cscope/ast-grep/bpftrace recipes that helped
  the second agent navigate the SVM CR2 sync paths.
- **Layer 7 state-trace ring** (per-CPU snapshot ring with 14
  hookpoints) captured the EXACT seq=10 transition that exposed
  the unconditional zero. **Without the ring's per-event sregs.cr2
  capture, this would have been a multi-week hunt.**
- The two-agent investigation (Opus + general-purpose) ranked
  hypotheses correctly (#3 "internal NPF clobbering CR2 across
  IDT delivery" was the right answer).

The state-trace `kvm_v2_trace_enable` boot cmdline arg (added in
this commit's parent) makes the ring active from boot — necessary
for catching the race during pid=1's first KVM_RUN before debugfs
is even mounted.

## Investigation timeline

- **Layer 8 (T13)**: solved the headline `preempt_disable()` no-op
  bug. mt-mini went from ~50% → 99%.
- **Initial post-T13 measurement** (2026-05-02 morning): mt-mini
  showed residual ~1-7% flake under N=30 to N=200 sweeps.
- **First diagnostic round**: added DIAG2/DIAG3 dumps + tested the
  agent's `arch_flush_thread` reset hypothesis. Rate stayed at ~1%.
- **State-trace ring dispatched at boot**: captured the smoking-gun
  17-entry trace on the very first failing iteration.
- **Second agent**: correctly identified the
  `sregs->cr2 = 0` clobber + internal NPF mechanism.
- **Fix shipped + verified**: 300/300 mt-mini PASS, no regressions.

Total: ~3 hours of focused work from "1% flake" to "root cause +
patch + 100% PASS verification."

## Related cleanups (deferred)

- Bug B (`segfault at ffffe000000021c0` mid-test wild kernel-half
  jump in mt-mmap-stress) is a separate bug class — likely register-
  or stack-corruption rather than CR2 loss. Tracked as task #165.
- The `saved_cr2_valid` rescue path now sees less traffic since
  same-task CR2 is preserved by default. Could potentially be
  simplified, but defer until a workload demonstrates a clean
  case for removal.

## Cited file references

- arch/um/backend/kvm-v2/vcpu.c:1270-1290 (the fix)
- arch/um/backend/kvm-v2/kvm_v2_backend.h:357 (last_task field)
- arch/x86/um/asm/processor_64.h:127-135 (arch_flush_thread reset)
- arch/um/backend/kvm-v2/state_trace.c:520-555 (boot-arm cmdline)
- arch/x86/kvm/svm/svm.c:4457 (the line that loads VMCB.cr2 from
  vcpu->arch.cr2 every VMRUN — root mechanism)
- arch/x86/kvm/x86.c:8582-8593 (`emulator_get_cr(2)` returns
  vcpu->arch.cr2, not the architectural CR2)
- AMD APM Vol 2 §8.4 (Page Fault Exception #PF — saved RIP semantics
  for instruction-fetch faults)
