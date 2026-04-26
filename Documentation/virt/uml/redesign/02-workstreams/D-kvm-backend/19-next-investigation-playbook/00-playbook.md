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

## Open hypotheses (NOT yet investigated)

1. **`_dl_catch_exception` body inspection**: disassemble what's at
   ld-linux+0x14560 to confirm it's the catch-exception helper. If yes,
   what conditions cause a longjmp here?

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
