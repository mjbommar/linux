# Next Investigation Playbook

Status: 2026-04-26 evening, after memo 17 Phases A-K + memo 18 Phases 1+2+3-fix.

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

### Highest-leverage next-session investigation

1. Add tracing in `kvm_run_userspace` between um_tlb_sync and
   kvm_enter_guest to count: pending signals, queued workqueue
   items, KVM MMU notifier callbacks pending. Confirm WHICH
   asynchronous work needs the 50us window.

2. Once identified, add proper synchronization (e.g., explicit
   wait-for-MMU-notifiers, or signal-pending check + block, or
   workqueue flush) to eliminate the race without the spin.

3. The fix is likely a MISSING synchronization point in
   `kvm_run_userspace` — some "wait until all pending mm
   mutations are visible to KVM" call.

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
