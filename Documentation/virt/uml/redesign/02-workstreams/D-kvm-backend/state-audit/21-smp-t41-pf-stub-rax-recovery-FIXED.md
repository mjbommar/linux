# SMP-T41 — EINTR-mid-PF-stub user-RAX recovery — FIXED 2026-05-03 (af659ad4297d)

## TL;DR

The 3-week-hunted **mt-mini SMP T=8 STRICT_MEMSET_FAIL byte[0]=0**
residual was caused by `kvm_v2_handle_pf_eintr_inline` not
recovering user RAX from the IST stack when EINTR caught the guest
mid-PF-stub between `mov %cr2, %rax` and `pop %rax`. The user
resumed at `frame.user_rip` with `RAX = CR2`. For the user
instruction `mov %al, (%rdx)` (mt-mini's strict_memset write), AL
was the low byte of CR2 (= 0, since CR2 is page-aligned), so the
first store on the just-installed page wrote `0x00` instead of
`tid`. Read-back returned 0. STRICT_MEMSET_FAIL.

Fix: at handler entry, capture `stub_rip_at_eintr` BEFORE
overwriting `gp[HOST_IP]` from the IST iretq frame. If the stub's
first byte (`push %rax`) has executed, recover user RAX from
`*(u64 *)(top - 56)`.

Validation:

| Run | PASS | STRICT_MEMSET_FAIL | Wilson 95% CI |
|---|---|---|---|
| T39 baseline N=100 | 88/100 | 11 | [80%, 93%] |
| **T41 fix N=400** | **397/400** | **0** | **[97.8%, 99.7%]** |

Substrate gate (kvm-v2): PASS=25 FAIL=3 EXPECTED_FAIL=3 —
bit-identical to seccomp baseline. No regression.

## Mechanism

### The PF stub layout (exception.c:213, kvm_v2_handler_stub_pf)

```
+0x00  50                       push %rax            ; RSP -= 8; mem[RSP] = user RAX
+0x01  48 c7 44 24 e8 ff ...    movq $-1, -24(%rsp)  ; sentinel (DIAG #121)
+0x0a  0f 20 d0                 mov %cr2, %rax       ; RAX = CR2
+0x0d  48 89 44 24 f8           mov %rax, -8(%rsp)   ; capture CR2 to top-64
+0x12  48 89 54 24 f0           mov %rdx, -16(%rsp)  ; capture RDX to top-72
+0x17  58                       pop %rax             ; restore user RAX
+0x18  e6 f6                    out %al, $UM_KVM_TRAP_PF
+0x1a  48 83 c4 08              add $8, %rsp
+0x1e  48 cf                    iretq
```

The stub modifies RAX (`mov %cr2, %rax` at offset 0xa) and restores
it (`pop %rax` at offset 0x17). RDX is read but not modified.
RFLAGS is preserved by all instructions in the stub (control-reg
moves don't update FLAGS; mov-to-mem doesn't either).

### IST stack layout after IDT delivery + push %rax

The CPU's IDT delivery for #PF (vector 14, has error code) pushes
the iretq frame downward from the IST stack top, leaving:

```
top - 8:   SS
top - 16:  RSP
top - 24:  RFLAGS
top - 32:  CS
top - 40:  RIP        ← user RIP at fault (e.g., the MOV at 0x401d4b)
top - 48:  error_code
                       ↓ push %rax (stub byte 0) decrements RSP by 8 here
top - 56:  user RAX   ← preserved across all subsequent stub instructions
top - 64:  captured CR2 (written by stub byte 0xd: mov %rax, -8(%rsp))
top - 72:  captured RDX (written by stub byte 0x12: mov %rdx, -16(%rsp))
top - 80:  sentinel -1 (written by stub bytes 1..9: movq $-1, -24(%rsp))
```

Critically, **none of the stub's writes after `push %rax` touch
top-56**. The original push value sits there for the full window
[stub+1, stub+0x18) — until `pop %rax` reads it back into RAX.

### The bug

When EINTR caught the guest with RIP in `[stub+0xd, stub+0x18)`,
`eintr_regs.rax = CR2` (set by `mov %cr2, %rax`), not user RAX.

The previous `kvm_v2_handle_pf_eintr_inline`:

```c
int kvm_v2_handle_pf_eintr_inline(...)
{
    struct kvm_v2_ist_frame frame;

    kvm_v2_ist_frame_read(vcpu, &frame, true);

    regs->gp[HOST_IP]     = frame.user_rip;
    regs->gp[HOST_SP]     = frame.user_rsp;
    regs->gp[HOST_EFLAGS] = frame.user_rflags;
    regs->is_user         = 1;
    /* gp[HOST_AX] still has eintr_regs.rax = CR2, unchanged */

    regs->faultinfo.error_code = (int)frame.error_code;
    regs->faultinfo.cr2        = cr2;
    regs->faultinfo.trap_no    = 14;

    segv_handler(SIGSEGV, NULL, regs, NULL);  /* installs the page */
    interrupt_end();

    kvm_v2_ist_frame_write(vcpu, regs, true);
    kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
    /* writes ALL 16 GPRs including bad RAX = CR2 to kvm_run.s.regs */
    run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;

    return 0;
}
```

`marshal_to_kvm_regs` writes all 16 GPRs from `regs->gp[]` into
`kvm_run.s.regs.regs`. With `KVM_SYNC_X86_REGS` set in
`kvm_dirty_regs`, the next `KVM_RUN` applies all 16 to
`vcpu->arch.regs[]`. The guest resumes at `frame.user_rip` with
`RAX = CR2`.

For mt-mini's `mov %al, (%rdx)` instruction at `0x401d4b`:
- `RDX = 0x4403d000` (correct: vp + i)
- `RAX = 0x4403d000` (BAD: was supposed to be `tid` from `movzbl`
  at `0x401d47`, but is now CR2)
- `AL = 0x00` (low byte of CR2 — page-aligned)

The store writes `0x00` to byte at `0x4403d000`. strict_memset's
read-back at the same address returns 0. The function returns
`first_mismatch = 0x5000`. mt-mini reports `STRICT_MEMSET_FAIL`.

### State-trace dump from a real failure

From `logs/t41-discrim/p0_default/w0/run-6.log`, decoded around
the failing event (cpu=2, pid=62, cr2=0x4403d000):

```
seq=158  POST_KVM_RUN exit=10 (-EINTR)  rax=4403d000 rip=ffffe0000000214d
seq=159  EINTR_PATH                      rax=4403d000 rip=ffffe0000000214d
seq=160  EINTR_INLINE_PF                 rax=4403d000 rip=ffffe0000000214d
seq=164  POST_TLB_SYNC (next dispatch)   rax=4403d000 rip=401d4b
                                          ^^^^^^^^^^^^^^^^^^^^^^
                                          RAX still = CR2, not user RAX
```

`RIP at EINTR = 0xffffe0000000214d = KVM_V2_HANDLERS_GVA + 0x14d`
= PF stub start (`HANDLERS_GVA + 0x140`) + `0x0d`. The stub had
just executed `mov %cr2, %rax` (offset `0x0a`, 3 bytes) and was
about to execute `mov %rax, -8(%rsp)` (offset `0x0d`).

## Fix

```c
int kvm_v2_handle_pf_eintr_inline(struct uml_pt_regs *regs,
                                  struct kvm_run *run,
                                  struct kvm_v2_vcpu *vcpu,
                                  u64 cr2)
{
    struct kvm_v2_ist_frame frame;
    u64 stub_rip_at_eintr = regs->gp[HOST_IP];
    u8 *top = (u8 *)vcpu->ist_stack_kva + PAGE_SIZE;

    kvm_v2_ist_frame_read(vcpu, &frame, true);

    regs->gp[HOST_IP]     = frame.user_rip;
    regs->gp[HOST_SP]     = frame.user_rsp;
    regs->gp[HOST_EFLAGS] = frame.user_rflags;
    regs->is_user         = 1;

    /* SMP-T41: recover user RAX from IST top-56 if push %rax has
     * executed (stub_rip > stub_start). The push wrote user RAX
     * to top-56 and no subsequent stub instruction modifies that
     * slot. */
    if (stub_rip_at_eintr > KVM_V2_HANDLERS_GVA + 0x140)
        regs->gp[HOST_AX] = *(u64 *)(top - 56);

    /* ... rest unchanged ... */
}
```

### Why the `> stub_start` (strict greater-than) check matters

Two cases:

1. `stub_rip_at_eintr > stub_start` (RIP past byte 0): `push %rax`
   has executed. User RAX is at IST top-56. Recover it. RAX in
   `eintr_regs` MAY be `CR2` (if `mov %cr2, %rax` has also
   executed) or MAY still be user RAX (if only `push` has
   executed) — either way, IST top-56 is correct.

2. `stub_rip_at_eintr == stub_start` (RIP exactly at byte 0):
   `push %rax` has NOT executed. RSP = IST top - 48. IST top-56
   contains stale data from a prior fault (last task's pushed
   RAX, or zeros). RAX in `eintr_regs` IS user RAX. Doing the
   recovery would CORRUPT user RAX with the stale data. Skip.

The CPU sets RIP=stub_start at the cycle of IDT delivery; the
next host check for pending signals happens at instruction
boundaries. EINTR can fire at exactly RIP=stub_start
(probability low but not zero) or anywhere inside the stub.

## Why this bug took 3 weeks to find

The investigation across SMP-T31 .. SMP-T40 chased nine wrong
hypotheses:

| # | Hypothesis | Outcome |
|---|---|---|
| T31 | TDP coherence ablation | Wrong (NPT not the cause) |
| T32 | handle_mm_fault page-alloc race | Wrong |
| T33 | KVM prev_roots cache (3 commits) | Partial fix (+15pp from changed timing); actual mechanism wrong |
| T34 | mmu_notifier_invalidate_range | Wrong |
| T35 | jitter sweep | Confirmed not timing-dependent |
| T36 | mmu_gather batch-skip | Real audit fix; not the byte[0]=0 cause |
| T37 | host pthread pinning | Neutral |
| T38 | capture-at-failure diag | Surfaced byte[0]=0 / page-aligned-offset signature |
| T39 | PT-page recycle | Wrong (PFN zones disjoint) |
| T40 | prefault MADV_POPULATE_WRITE test | **CONFIRMED** bug is in #PF resume path |
| T41 | state-trace dump on STRICT_MEMSET_FAIL | **ROOT CAUSE found in 1 day** |

Lessons:

1. **Diagnostic-first, not fix-first.** T31..T39 were "design a
   fix for the suspected mechanism, run it, see if it helped"
   loops. Each took ~3 days. T41 was "enable the existing trace
   ring at boot, dump on a real failure, look at what it says" —
   1 day to root cause.

2. **Use existing tooling.** The state-trace ring (per-CPU 2 MB,
   covering every dispatch phase including HANDLE_IO_PF /
   EINTR_INLINE_PF / EINTR_RAW_SNAPSHOT op codes) was built for
   exactly this kind of investigation. mt-mini already triggered
   the dump on STRICT_MEMSET_FAIL. The first dump showed
   `RAX = CR2` at the EINTR boundary, immediately localizing the
   bug.

3. **Statistically-conclusive isolation experiments first.** T40
   (MADV_POPULATE_WRITE) narrowed the suspect surface from "all
   of v2" to "3 functions" in one experiment. Should have
   designed that test BEFORE T31..T39's hypothesis-testing rounds.

4. **EINTR-mid-stub is a fundamental issue, not just for PF.**
   Any in-guest stub that modifies caller-saved registers between
   IDT delivery and vmexit is vulnerable to EINTR-mid-stub
   register clobbering. Currently only the PF stub does this
   (because it captures CR2 to RAX before vmexit). NM/GP/UD/DE/OF
   stubs are pure `out + iretq` — safe. If a future stub adds
   intermediate register modification, it MUST also have an EINTR
   recovery path.

5. **The IST top-56 slot is "user-RAX-after-PF-stub-push" by
   convention.** The stub bytes' invariant is now documented in
   the T41 fix comment in `handle_pf_eintr_inline`. Future stub
   modifications must preserve this slot through their entire
   EINTR-able window.

6. **Read-back-of-store as a write-validation primitive.**
   Without `strict_memset`'s write+readback (vs the cheaper
   `memset`), this bug would have surfaced as occasional silent
   corruption far from the write site, taking weeks more to
   localize. Tests for memory ops should include immediate-
   readback discipline.

## Files touched

- `arch/um/backend/kvm-v2/syscall_trap.c` — `handle_pf_eintr_inline`
  RAX recovery + comment
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/`
  `state-audit/20-smp-t33-vcpu-pool-sharing-diary.md` — diary
  entries 30 and 31 (root cause + N=400 confirmation)
- This memo

## Validation matrix (T41-fix kernel)

- mt-mini SMP T=8 ncpus=4: 397/400 (Wilson 95% [97.8%, 99.7%]),
  0 STRICT_MEMSET_FAIL events, 3 init.sh-hang timeouts (different
  bug class — see SMP-T54 follow-up).
- substrate gate kvm-v2: PASS=25 FAIL=3 EXPECTED_FAIL=3
  (bit-identical to seccomp baseline).
- TODO additional gates run as part of T41-extended stress soak.
