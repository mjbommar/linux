# Next Investigation Playbook

Status: 2026-04-26 evening, after memo 17 Phases A-K + memo 18 Phases 1+2+3-fix.

Companion control/state model:
[01-control-state-model.md](01-control-state-model.md).

## Empirical baseline (verified this session)

- cpython parity gate: **17/21 median** across 4+ trials. Failed modules
  rotate (test_struct, test_decimal, test_int, test_float, test_array,
  test_typing, test_list, test_math, test_hashlib, test_bytes, test_set,
  test_abc, test_itertools — all have failed at least once).
- `import test.test_decimal` reproducer: **86% reliability** (43/50).

## Dominant failure mode (verified N=30 with full pf_mini_regs capture)

**5 of 8 failures (62.5%) are IDENTICAL**:
```
rip=0x400265c3 cr2=0x470 ec=0x4
rax=0x0 rbx=0x4003cac0 rcx=0x400299eb rdx=0x1 rsi=0x4000 rdi=0x403d3000
r8=0x0 r12=<varies> r13=0x0 r14=0x0 r15=0x0
```

Instruction at 0x265c3 in ld-linux: `cmpq $0x0, 0x470(%r15)`.
Function: `_dl_mcount@@GLIBC_2.2.5+0x4a03` (likely
`_dl_relocate_object` merged via LTO). Preceding call at 0x265be: to a
private helper at 0x14560 (no public symbol; offset 0x14560 lies between
`_dl_fatal_printf@0x101c0` and `_dl_get_tls_static_info@0x16020` —
likely `_dl_signal_error` / `_dl_catch_exception` body).

**Pattern interpretation**: r13/r14/r15 all-zero matches `ELF_PLAT_INIT`
(arch/x86/um/asm/elf.h:118-120) which zeroes all regs at execve. The
0x265c3 site is reached via setjmp/longjmp exception unwinding — the
helper at 0x14560 catches an exception, longjmp restores callee-save
regs from a jmpbuf captured at process startup (when r13/r14/r15 were 0).
Optimized code at 0x265c3 assumes r15 is non-NULL → NULL deref.

**The actual bug is upstream of the SIGSEGV**: something causes glibc
to raise an exception under KVM that doesn't fire under seccomp.

## What's been ruled out empirically

| Hypothesis | Test | Result |
|------------|------|--------|
| TLB invalidation race | Disabled SREGS-skip cache → CR4.PGE flush every entry | 13/15 (no improvement) |
| Shadow-PT staleness | Forced full PGD-walk every entry | 13/15 (no improvement) |
| IRETQ frame leaf clobber by fill | Phase 3-fix (post-fill install) | 17/21 (committed; structurally cleaner) |
| Direct sync vs fill race | cmpxchg(0→new) install-if-absent (Phase 4-fix v1/v2) | 16/21, 15/21 (both regressed; reverted) |
| KVM_SYNC_X86_REGS staleness | Disabled sync_regs_caps | 15/21 (no improvement; reverted) |
| ASLR | norandmaps boot flag | 16/20 (no change) |
| Cross-mm contamination | only 1 mm (single python3 PID 1) | n/a |
| Cross-task contamination | only 1 user task | n/a |

## Helper at ld-linux+0x14560 IDENTIFIED 2026-04-26

Disassembly of /lib64/ld-linux-x86-64.so.2 at offset 0x14560:

```
14560: endbr64
14564: testb $0x8, 0x354(%rdi)        ; flag check at link_map+0x354
1456b: je 14570                       ; if zero, do work
1456d: ret                            ; ELSE: early return (no-op)
14570: push %rbp
14571: mov %rsp, %rbp
14574: push %rbx
14575: mov %rdi, %rbx                 ; rbx = link_map
1457c: call 0x107a0                   ; private helper
14581: mov 0x4a8(%rbx), %rax          ; link_map field
14588: test %rax, %rax
1458b: je 145b2                       ; if zero, return
... (compute relro region)
145cb: call 0x299e0                   ; ← SYSCALL: mprotect (NR=10)
145d0: test %eax, %eax
145d2: jns 145b2                      ; if mprotect succeeded, return
... (error path: load error info, would call _dl_signal_error /
     _dl_fatal_printf which can longjmp)
```

`0x299e0` is `mov $0xa,%eax; syscall` — inline mprotect(2) syscall
stub. So the helper at 0x14560 is `_dl_protect_relro` (or equivalent
relro-protection helper). It:

  1. Skips if relro already protected (flag bit 3 of link_map+0x354)
  2. Computes the relro VA range from link_map fields
  3. Calls mprotect() to make the range read-only
  4. On mprotect failure, takes error path that can longjmp via
     _dl_signal_error / _dl_fatal_printf

**This means the dominant 62.5% failure mode is**: `mprotect(2)` is
failing under KVM during ld-linux's relro setup. The error path
longjmps back to a setjmp earlier in ld-linux startup (when
r13/r14/r15 were 0 per ELF_PLAT_INIT). Code at 0x265c3 then
NULL-derefs.

**Why mprotect would fail under KVM but not seccomp**:

  (a) The link_map fields used to compute the relro range read
      WRONG values under KVM (memory-content corruption). mprotect
      called with bad addr/len → -EFAULT or -EINVAL.

  (b) The mprotect call itself behaves differently — UML's
      syscall handler routes mprotect through do_mprotect → which
      calls kvm_mm_unmap (under KVM). If kvm_mm_unmap has any
      path that fails on relro semantics, mprotect could return
      an error.

Both (a) and (b) point to **memory-content / host-VA aliasing**
class bugs — squarely in Phase 4 (KVM-native uaccess / per-mm
host worker) territory. Per-task vCPU (Phase 5) does NOT address
these.

## BREAKTHROUGH 2026-04-26: bug is TIMING-DEPENDENT

Empirical test sequence (each N=30 trials of the test_decimal
import reproducer, classified by failure mode):

| Variant | Total ok | ldlinux failures | other |
|---------|----------|------------------|-------|
| Baseline (no instrumentation)            | 43/50  | 4 | 3 |
| `printk` for every mprotect call         | 30/30  | **0** | 0 |
| Confirm `printk`                         | 28/30  | **0** | 2 |
| `cpu_relax×100k` instead of printk       | 27/30  | **0** | 3 |
| `mb()` instead of cpu_spin               | 26/30  | 3 | 1 |
| `um_tlb_sync` instead of cpu_spin        | 27/30  | 3 | 0 |
| `cpu_spin×100k` AFTER um_tlb_sync (every entry) | 28/30 | **0** | 2 |

**The ld-linux NULL deref failure mode is purely TIMING-DEPENDENT.**
A ~50-100 microsecond delay (from printk, cpu_relax×100k, or any
similar latency) at *either* of these locations eliminates it:

1. After `sys_mprotect` returns inside `handle_syscall`
2. After `um_tlb_sync(current->mm)` and before `kvm_enter_guest` in
   `kvm_run_userspace`

**mb() and um_tlb_sync alone DO NOT close the race** — only true
*latency* does. So it's NOT a memory ordering issue and NOT a TLB
sync ordering issue. Something asynchronous needs ~50us of CPU
time to complete.

### Candidates for the asynchronous work

- **Pending signal delivery**: SIGALRM driving UML's scheduler.
  cpu_relax doesn't disable interrupts, so during the spin, queued
  signals fire and are processed. Without the spin, we re-enter
  guest immediately and signals are deferred.
- **Host kernel workqueue**: some PTE-related cleanup is queued via
  workqueue / softirq. The spin gives scheduler time to drain it.
- **KVM internal state settling**: KVM's MMU notifier path has
  callbacks that fire asynchronously. mprotect triggers
  invalidate_range which may schedule MMU notifier work; without
  the spin, the next KVM_RUN may enter before notifiers complete.

### Threshold-finding empirical data (2026-04-26)

| Delay | ldlinux failures (N=30) |
|-------|------|
| udelay(1)  | 2 |
| udelay(10) | 2 |
| udelay(25) | 2 |
| udelay(50) | **0** |

Threshold is between 25us and 50us. Below 50us, race window stays open.

### Parity gate impact of udelay(50) (2 trials)

- Trial 1: 18/21 (test_struct, test_dict, test_tuple DIVERGE)
- Trial 2: 17/21 (test_math, test_bisect, test_bytes, test_int DIVERGE)

**Average ~17.5/21 vs 17/21 baseline = within variance.** The fix
reduces ld-linux NULL deref at the unit-test level (30/30 vs
14% baseline) but other failure modes dominate the gate. Not
committed because:
  1. Marginal gate-level improvement (within noise)
  2. ~5% perf cost (50us per kvm_run_userspace iteration)
  3. Band-aid without root cause understanding

The udelay(50) has been REVERTED. Diagnostic-only finding for
next-session investigation.

### Highest-leverage next-session investigation

1. **Identify what the 50us is doing**: the diagnostic ruled out
   memory ordering (mb), TLB sync (um_tlb_sync), scheduler
   (cond_resched), workqueue (flush_workqueue), and RCU
   (synchronize_rcu). Only pure latency works. Candidates that
   need direct testing:
     - HW write buffer drain (try `wbinvd`?)
     - Cache line settling (try sequential reads/writes to a
       cacheline?)
     - KVM internal MMU state propagation (check if there's
       a kvm_for_each_vcpu / vcpu_kick equivalent in our path)
     - Pending signal that needs to deliver via udelay-driven
       interrupt processing

2. **Add tracing in `kvm_run_userspace`** between um_tlb_sync and
   kvm_enter_guest to count: pending signals, queued workqueue
   items, KVM MMU notifier callbacks pending. Confirm WHICH
   asynchronous work needs the 50us window.

3. **Investigate the OTHER residual failure modes** (test_struct/
   dict/tuple/math/bisect/bytes/int — the ~3-4 modules that fail
   even with udelay(50)). These are likely a DIFFERENT bug class
   than the ld-linux NULL deref. Capture their pf_mini_regs
   patterns and classify.

4. Once identified, add proper synchronization to eliminate the
   race without the spin. The fix is likely a MISSING
   synchronization point in `kvm_run_userspace` — some "wait
   until all pending mm mutations are visible to KVM" call.

## Earlier hypothesis (now contradicted by data)

~~mprotect failing under KVM~~ — the captured failure traces show
all mprotect calls return 0 (success). The bug is NOT mprotect
failing; it's something asynchronous that happens AFTER mprotect's
syscall return and that needs time to settle before the next
guest entry.

2. **Singleton MSR state**: LSTAR / FMASK / STAR / EFER / KERNEL_GS_BASE
   programmed once at bootstrap. Audit whether ANY code path can
   overwrite them silently. Phase H caught FPU + VCPU_EVENTS but not
   these MSRs.

3. **Signal-delivery host-VA writes**: when the kernel sets up a
   sigframe on the user stack via `setup_signal_stack_si`, does it
   write through host VA in a way the guest sees stale through shadow
   PT? Phase 4 (KVM-native uaccess) addresses this class.

4. **Phase 5 per-task vCPU**: would eliminate ALL cross-task/cross-mm
   vCPU state contamination including FPU/SREGS/MSRs. 5-10 days work.
   For single-task scenarios (current dominant failure mode) this
   wouldn't help directly — but it'd eliminate a class of bugs.

5. **Phase 4 per-mm host worker (Option B)**: eliminates the host-VA
   aliasing class entirely. Multi-week work. The most likely
   structural fix for memory-content corruption.

## Recommended next-investigation sequence

Priority order (cheapest first):

1. **Disassemble ld-linux+0x14560**: `objdump -d /lib64/ld-linux-x86-64.so.2`
   and look at instructions starting at 0x14560. Identify what helper
   is being called. If it's a catch-exception body, check what
   exception types it handles.

2. **GDB-attach to UML process** at the failure point. With a `wait`
   loop around the SIGSEGV signal, attach gdb to the host UML process
   and inspect the user task's regs + stack to capture the
   pre-call register state. UML supports gdb attach via the
   `umconsole`/`mconsole` interface; details in
   `Documentation/virt/uml/`.

3. **Audit MSR singleton state** (open hypothesis #2). Read every
   site in arch/um/backend/kvm/*.c that does
   `KVM_SET_MSRS`/`KVM_GET_MSRS` and confirm there's no per-entry
   programming that could clobber the bootstrap MSRs.

4. **Add tracepoints** at glibc-suspect events: every `KVM_EXIT_IO`
   with port matching the gadget set, every `KVM_EXIT_HLT`,
   every signal delivery. Buffer in a circular log so a failure
   dumps the last 1000 events.

5. **Phase 5 per-task vCPU** (5-10 days) — only justifies investment
   if next-session work shows multi-task failures dominating, not
   single-task glibc exceptions.

6. **Phase 4 per-mm host worker** (multi-week) — structural fix
   for memory-content corruption. Biggest investment but eliminates
   the largest class of remaining bugs.

## Commits this session

- `1bb6c7b7637a` Phase 1.1+1.2 — CPL-aware marshal gate
- `6f2dc2a14e2e` Phase 2 — per-mm IRETQ frame + signal-block window
- `8de84913b847` Phase 3-fix — post-fill IRETQ install ordering
- `01d4f323c0c9` STATUS.md — memo 18 follow-up
- `f66318121e1d` STATUS.md — user-recommended items audit
- `24267298f026` STATUS.md — failure-mode dissection insight

## Reverted experiments (kept in this doc for archive)

- Phase 3 partial (per-mm SREGS cache without per-mm vCPU): 14/21,
  reverted. False hits on singleton vCPU.
- Phase 4-fix v1 (cmpxchg install-if-absent for ALL slots): 16/21,
  reverted. Stale kernel-half leaves became sticky.
- Phase 4-fix v2 (cmpxchg for user-half only): 15/21, reverted.
  No measurable improvement.
- KVM_SYNC_X86_REGS=0 diagnostic: 15/21, reverted. sync-regs not
  the bug source.

## What to NOT re-investigate

These have been thoroughly tested; don't burn cycles re-testing:

- TLB invalidation discipline (Phase B keystone + my N=15 test)
- Shadow-PT staleness via full-fill (forced full PGD walk every entry)
- Cross-mm IRETQ frame collision (Phase 2 fixed)
- Per-mm cache without per-mm vCPU (Phase 3 partial regressed)
- Direct sync vs fill race via simple cmpxchg (Phase 4-fix v1/v2)
- KVM_SYNC_X86_REGS as bug source (no improvement when disabled)

## CRITICAL UPDATE 2026-04-26: KVM mmu_notifier path is NOT involved

Verified empirically with diag ring + audit:

1. **Memslot range** (`um: kvm memslot: guest_phys=0 host_va=60000000
   size=20000000`): KVM memslot covers `[0x60000000, 0x80000000)` —
   the UML physmem region only. ~512MB.

2. **mprotect HVAs in failures** are at user VAs like `0x4003b000`,
   `0x401c2000`, `0x403d3000`, `0xa86000`. **All BELOW 0x60000000.
   None overlap the memslot.**

3. **kvm_mmu_unmap_gfn_range with `flush_on_ret=true`** in
   virt/kvm/kvm_main.c line 730 only fires `kvm_flush_remote_tlbs`
   when `kvm_handle_hva_range` returns `r.found_memslot=true` (line
   637 `if (range->flush_on_ret && r.ret) kvm_flush_remote_tlbs(kvm)`).
   Since user-VA mprotects don't overlap our memslot, **found_memslot
   is false → no KVM TLB flush request, no KVM internal work**.

So the prior hypothesis "KVM async work needs ~50us to complete" is
**WRONG**. The 50us delay is doing something UML-side, NOT KVM-side.

## SMOKING-GUN trace evidence (diag ring, commit 76376bd0afa3)

Captured FAIL vs PASS diff for `import test.test_decimal`:

| Event | PASS | FAIL (2/2 captured) |
|-------|------|---------------------|
| DIAG[20] | mprotect 0x403d3000 len=0x4000 prot=R | mprotect 0x403d3000 len=0x4000 prot=R |
| DIAG[21] | mprotect **0x401c2000** len=0x2000 prot=R | mprotect **0x403d3000** len=0x4000 prot=R |
| Inter-mprotect gap | ~100us | ~57us |

In FAIL: ld-linux's `_dl_protect_relro` loop iterator visits the
SAME link_map TWICE in succession. In PASS: it correctly advances
to the next library.

**Audit at fault**: shadow PT for `0x403d3000` (rdi at fault) IS
in sync with UML pgd:
```
um: kvm audit[pf_rdi]: va=0x403d3000 um_pte=0x7c01c1
    expected=0x7c0065 shadow=0x7c0065 EQUAL synced=1
```
Both UML pgd and shadow PT map this VA to PFN 0x7c0. So the
shadow-PT-staleness hypothesis at the link_map address is also
WRONG.

**The remaining hypothesis**: ld-linux's iterator is reading WRONG
DATA from the link_map struct (l_next or array index), causing it to
revisit the same DSO. Despite shadow PT being correct, the page
content visible to the guest may differ from what ld-linux wrote.
This could be:

  (a) Cache coherency issue between kernel-side writes (via host VA
      to user memory) and guest-side reads (via shadow PT → GPA → EPT).
      Unlikely on x86 with coherent caches.

  (b) ld-linux writes go to a DIFFERENT physical page than the guest
      reads. Would happen if shadow PT entry's GPA doesn't match
      os_map_memory's offset for the same VA. Audit shows PFN
      matches, so unlikely here unless there's an intermediate
      table mismatch.

  (c) Some UML-side state machine (signals, scheduler, interrupt-
      driven set_pte_at) is firing incorrectly under tight timing,
      corrupting the guest's view of memory in a way that takes
      ~50us to settle.

Tooling notes:
  - printk-per-syscall, gdb breakpoints, strace ALL suppress the bug
    via overhead.
  - Diag ring + on-panic dump preserves the bug enough to capture
    the discriminator.
  - Need a sub-microsecond, on-panic-dumpable trace mechanism for
    deeper investigation. Linux ftrace's `trace_printk` could work
    if accessible from UML — needs investigation.

## RESOLVED 2026-04-26 evening: snapshot KVM exit state before unblock_signals

User-identified race: `kvm_run_userspace` called `unblock_signals()`
IMMEDIATELY after `KVM_RUN` returned, BEFORE reading `run->exit_reason`,
`run->s.regs.*`, `run->io.port`, `run->mmio.*`, and the singleton IST
stack contents. `unblock_signals()` can synchronously deliver SIGALRM
→ timer handler → schedule() → another UML task gets the singleton
vCPU/run mmap and runs its own KVM_RUN, overwriting the exit state.
When the original task resumes, it decodes wrong state.

**Fix landed**: commit `b516bee62eb2`. Snapshot exit_reason, kregs,
exit_sregs, io.port, mmio.{phys_addr,len,is_write}, cr2 (via exit_sregs),
and 40 bytes of IST stack BEFORE unblock_signals. All downstream code
uses the snapshots.

Empirical impact:
  - test_decimal import unit-test: 5/100 ldlinux (5%) vs 4/50 baseline
    (8%) — ~40% reduction
  - cpython parity gate: 17/21 (same count as baseline, but failure
    MIX changed: some k=?/? crashes converted to k=N/False assertion
    failures — suite ran further before failing)

The fix is structurally correct independent of empirical magnitude:
all singleton-vCPU exit state must be captured before any code path
that could yield to another task.

## NEW OBSERVATION 2026-04-26: shadow-PT/TLB staleness during long computation

After the snapshot fix, captured a NEW failure mode in test_int's
suite (k=52/False instead of k=?/?). Verbose run shows ONE assertion
failure: a 12-million-character integer-as-string output where the
first ~2.4M characters MATCH the expected, then diverge with
corrupted bytes:

```
Expected: "12345...01234567890123456789012345..."  (clean repeating)
Got:      "12345...3456774763587453814521543465852917..."  (corrupted)
```

This is NOT a crash — Python's bignum arithmetic ran, produced output,
but some byte range got the wrong content. Classic signature of
**shadow PT or guest TLB staleness during long-running computation**.
Distinct from the ld-linux NULL deref bug class (which is at startup
and was timing-sensitive to ~50us).

Candidates for this bug class:
  1. Shadow PT entry has wrong PFN (set_pte_at didn't propagate)
  2. Guest TLB caches old PFN, CR4.PGE toggle doesn't fire (dirty
     flag race)
  3. Same physical page mapped to two GPAs → guest writes to one,
     reads from other (memslot/host-VA aliasing)
  4. KVM's TDP MMU has a bug where its own EPT cache becomes stale
     for a GPA whose backing host PT was changed (BUT host VAs in
     our captures are outside the memslot, so this shouldn't apply
     to user mprotect — only to UML's own physmem-region mprotects)

Next-session priorities:
  1. Reproduce the test_int wrong-answer pattern reliably (capture
     which test method fails, what arguments it uses).
  2. Add a CONTENT-LEVEL audit: at fault-or-divergence time, dump
     the actual page contents at the diverging byte range to
     compare guest view vs kernel view.
  3. Investigate whether the shadow_dirty flag is being set for
     ALL set_pte_at-driven shadow updates — particularly during
     heavy malloc/free cycles in big-int arithmetic.
