# 03 — Bootstrap bytecode review (KVM backend)

Audit perspective: **kernel-mode bytes**. Every byte in the bootstrap
page (LSTAR trampoline, IDT/GDT/TSS setup, #PF/#DF/#GP handlers,
SYSRETQ/IRETQ gadgets) executes in ring-0 of the guest. An off-by-one,
wrong selector, or stale offset is silently catastrophic — the CPU
will faithfully execute corrupted bytes and return wild RIPs, NULL+
small offsets, or 45-bit garbage to user code. That is exactly the
failure mode the import-unittest workload exhibits under
`backend=force=kvm`.

This document walks the bytes of every gadget, verifies offsets,
selector encoding, displacement math, and the iretq frame layout,
then ranks the findings by likelihood of producing the observed
wild-pointer pattern.

---

## Executive summary — top-3 risks in the bootstrap bytecode

1.  **Install-pass clobber of bootstrap aliases (HIGH, plausible cause
    of observed bug).**  `kvm_shadow_fill_from_uml_pgd`'s **clear pass**
    explicitly preserves `[bootstrap_va, +4*PAGE_SIZE)` (lifecycle.c
    line 1213-1252), but the **install pass** at lines 1266-1331 walks
    the UML pgd and unconditionally calls `kvm_shadow_map_page(va,...)`
    for every present leaf. There is **no `alias_lo`/`alias_hi` check
    in the install loop**.

    Consequences if any UML pgd entry (init_mm or per-mm) is present
    at any of `bootstrap_va`, `+0x1000` (gadget state), `+0x2000`
    (vvar), `+0x3000` (IST stack):

    - The bootstrap shadow leaf is overwritten with whatever the UML
      pgd PTE says (different PFN, different flags — could become
      RW|US|NX, removing P, etc).
    - Next #PF (or any access) reads garbage. The gadget state read
      via `%gs:0x08` returns whatever bytes happened to live in the
      page UML mapped there → guest code dereferences a random
      32-bit value as a `pid_t` — and any subsequent libc/Python
      logic that uses the pid (e.g. as an array index or a shared-
      memory key) carries the corruption forward.
    - This matches the failure shape: small workload (no syscalls
      that issue gadget-handled NRs) is fine; first `getpid` /
      `clock_gettime` / `time` returns garbage; importing
      `unittest` does many such calls during init → wild pointers.

    `bootstrap_va` is allocated via `get_zeroed_page(GFP_KERNEL)` so
    it lands somewhere above `uml_physmem` (UML binary base) — i.e.
    in PGD slot 0 (UML's "kernel VA region" is in the canonical low
    half because UML runs as a host user-space process). User mmaps
    in the same slot CAN collide if the allocator chooses an
    overlapping address, especially under cumulative-imports load
    where heap/anon-mmap pressure pushes mappings into adjacent
    slots.

    The comment block at lifecycle.c lines 1180-1183 explicitly
    flags this as a "verify with kvm_diag_audit_pgd_skip if
    collisions are suspected" open question — and there is no
    runtime guard, only a diagnostic.

2.  **`KERNEL_GS_BASE` is one-shot primed and never re-loaded
    (HIGH, latent).**  `kvm_enter_guest_program_kernel_gs_base`
    (thread.c 1644) checks `kernel_gs_base_primed` and skips the
    `KVM_SET_MSRS` if true. That MSR is per-vCPU state that KVM
    persists in the vCPU. **But KVM_SET_SREGS and KVM_SET_MSRS in
    other paths CAN clobber it.** In particular:

    - The FPU save/restore on context switch (`kvm_fpu_save_for_task`
      / `kvm_fpu_restore_for_task` at thread.c 132/150) does NOT
      touch MSRs — but a guest `arch_prctl(ARCH_SET_GS)` from
      user-mode code re-enters via the syscall path and the host
      handler may rewrite MSR_GS_BASE (which is paired with
      KERNEL_GS_BASE in KVM's sync). Inspect: under SYNC_X86_SREGS
      whether a `regs->gs.base = ...` write side-effects MSR_KERNEL
      _GS_BASE — KVM's documentation of the sync semantics is
      ambiguous on this in older host kernels.
    - If KERNEL_GS_BASE drifts, the very first `swapgs` at LSTAR
      entry reads from wrong base. `%gs:0x08` then reads the
      arbitrary VA at (whatever-old-base + 0x08), returns bytes
      from there, sysretq returns to user, user has a garbage
      "pid" / "tgid" that flows downstream as a pointer or index.

    A robust fix is to drop the `kernel_gs_base_primed` short-circuit
    or to re-prime once per kvm_enter_guest entry; the per-entry
    cost is one ioctl (~5 µs) which is the same order as
    `KVM_SET_REGS`.

3.  **#PF/#DF/#GP all share `IST=1` (MEDIUM, latent).**  IDT[8]
    (#DF), IDT[13] (#GP), IDT[14] (#PF) are programmed with `e[4]
    = 0x01` in `kvm_enter_guest_init_bootstrap` (thread.c 1357,
    1378, 1393). They all reload RSP from `TSS.IST[1]` =
    `bootstrap_va + 0x4000` (the IST stack page top). A nested
    fault — say, #PF during #PF delivery, or #GP during #PF
    (wrong selector poked into the iretq frame), or any cascade
    — re-loads RSP from the same TSS.IST[1] and pushes the inner
    fault frame at `0x3fc8..0x3ff8`, **overwriting the outer
    fault's frame at the same range**.

    The host's #PF recovery reads the iretq frame at offsets
    `+8/+24/+32` to recover `RIP/RFLAGS/RSP` (thread.c 3236-
    3276). If the frame has been overwritten by a cascade, the
    "user RIP" the host recovers is the inner-fault's RIP (a
    bootstrap address like `0x...4d2`) and the host then builds
    the IRETQ frame with that, sending the user back to the
    middle of the IRETQ gadget instead of user code. Result:
    silent re-entry into ring-0 code at a non-canonical or
    misaligned offset, eventually triple-faulting or producing
    a wild RIP for SIGSEGV delivery.

    Mitigation: separate IST slots — IST[1]=#PF, IST[2]=#DF,
    IST[3]=#GP — and patch `e[4]` accordingly. The TSS already
    has room for IST[1..7].

---

## Reverse-engineered annotated dump

### Bootstrap page layout (4 KiB, mapped at `bootstrap_va`)

```
+0x000  GDT      (8 entries × 8 B = 64 B; entries 0..5 from
                  kvm_setup_harness_gdt, entries 6+7 = TSS desc
                  patched in by kvm_enter_guest_init_bootstrap)
+0x040  LSTAR    (gadget body, 394 B; ends at +0x1ca)
+0x200  TSS      (104 B, ends at +0x267; IOPB off at +102 = 104)
+0x280  IDT      (33 entries × 16 B = 528 B; ends at +0x48f)
+0x4a0  PF_HANDLER  (8 B: out $fb; add $8,%rsp; iretq)
+0x4b0  SYSRET   (3 B: 48 0f 07; DEAD CODE — kept for ABI)
+0x4c0  DF_HANDLER  (3 B: out $fa; hlt)
+0x4d0  IRETQ    (2 B: 48 cf)
+0x4d8  GP_HANDLER  (3 B: out $f9; hlt)
+0x4e0..+0xfff  (unused, zero-filled)
```

Adjacent-page layout in the per-mm shadow PT (mapped from `bootstrap_va`):

```
bootstrap_va + 0x0000  bootstrap (RO X)        ← code+tables
bootstrap_va + 0x1000  gadget state (RO NX)    ← KERNEL_GS_BASE points here
bootstrap_va + 0x2000  gadget vvar (RW NX)     ← clock budget decremented
bootstrap_va + 0x3000  IST stack (RW NX)       ← TSS.IST[1] = +0x4000
```

Note: the IST stack page MUST be RW (CPU pushes the iretq frame
from ring-0). The bootstrap page MUST be RO (immutable code +
descriptor tables; segment-load A-bit pre-set in the GDT entries
to avoid descriptor write-back).

### LSTAR trampoline byte-by-byte (verified)

I machine-verified all offsets and displacements; full trace in
the appendix. Summary:

| Offset | Bytes                              | Mnemonic                               | Notes                |
|--------|-------------------------------------|----------------------------------------|----------------------|
| +0     | `0f 01 f8`                          | swapgs                                 | enter kernel GS      |
| +3     | `3d 35 01 00 00`                    | cmp $0x135, %eax                       | NR_getcpu pre-check  |
| +8     | `75 05`                             | jne +5 → +15                           |                      |
| +10    | `e9 3b 01 00 00`                    | jmp +315 → +330 (getcpu_body)          | rel32 OK             |
| +15    | `a9 00 ff ff ff`                    | test $0xffffff00, %eax                 | upper-byte guard     |
| +20    | `75 28`                             | jne +40 → +62 (fallback)               |                      |
| +22    | `3c 27 74 2c`                       | cmp $0x27 / je +44 → +70 getpid        | rel8 OK              |
| +26    | `3c ba 74 36`                       | cmp $0xba / je +54 → +84 gettid        | rel8 OK              |
| +30    | `3c 6e 74 40`                       | cmp $0x6e / je +64 → +98 getppid       | rel8 OK              |
| +34    | `3c 66 74 4a`                       | cmp $0x66 / je +74 → +112 getuid       | rel8 OK              |
| +38    | `3c 6b 74 54`                       | cmp $0x6b / je +84 → +126 geteuid      | rel8 OK              |
| +42    | `3c 68 74 5e`                       | cmp $0x68 / je +94 → +140 getgid       | rel8 OK              |
| +46    | `3c 6c 74 68`                       | cmp $0x6c / je +104 → +154 getegid     | rel8 OK              |
| +50    | `3c 18 74 08`                       | cmp $0x18 / je +8 → +62 (sched_yield demoted; lands at fallback) |
| +54    | `3c e4 74 76`                       | cmp $0xe4 / je +118 → +176 clock stub  | rel8 OK              |
| +58    | `3c c9 74 77`                       | cmp $0xc9 / je +119 → +181 time stub   | rel8 OK              |
| +62    | `0f 01 f8 / e6 f4 / 48 0f 07`       | swapgs ; out $f4 ; sysretq             | fallback path        |
| +70…+154 | `65 8b 04 25 OFF 0f 01 f8 48 0f 07` (×7) | mov %gs:OFF,%eax ; swapgs ; sysretq | pid family — 14 B each |
| +168   | `31 c0 0f 01 f8 48 0f 07`           | xor %eax,%eax ; swapgs ; sysretq       | sched_yield (orphan) |
| +176   | `e9 05 00 00 00`                    | jmp +5 → +186 clock body               |                      |
| +181   | `e9 67 00 00 00`                    | jmp +0x67 → +289 time body             |                      |
| +186   | `65 83 2c 25 28 10 00 00 01`        | sub $1,%gs:0x1028 (BUDGET)             |                      |
| +195   | `0f 88 75 ff ff ff`                 | js −139 → +62 fallback                 | rel32 OK             |
| +201   | `83 ff 01`                          | cmp $1,%edi (clockid==MONOTONIC?)      |                      |
| +204   | `0f 85 6c ff ff ff`                 | jne −148 → +62 fallback                | rel32 OK             |
| +210   | `65 8b 14 25 00 10 00 00`           | mov %gs:0x1000,%edx (SEQ)              |                      |
| +218   | `f6 c2 01`                          | test $1,%dl                            | seqlock parity check |
| +221   | `0f 85 5b ff ff ff`                 | jne −165 → +62 fallback                | rel32 OK             |
| +227   | `65 4c 8b 14 25 08 10 00 00`        | mov %gs:0x1008,%r10 (MONO_SEC)         |                      |
| +236   | `65 4c 8b 04 25 10 10 00 00`        | mov %gs:0x1010,%r8  (MONO_NSEC)        |                      |
| +245   | `65 3b 14 25 00 10 00 00`           | cmp %gs:0x1000,%edx (re-read SEQ)      |                      |
| +253   | `0f 85 3b ff ff ff`                 | jne −197 → +62                         | rel32 OK             |
| +259   | `65 48 39 34 25 30 10 00 00`        | cmp %rsi,%gs:0x1030 (TASK_SIZE_CAP)    |                      |
| +268   | `0f 86 2c ff ff ff`                 | jbe −212 → +62                         | rel32 OK             |
| +274   | `4c 89 16 / 4c 89 46 08`            | mov %r10,(%rsi); mov %r8,8(%rsi)       | store ts             |
| +281   | `31 c0 / 0f 01 f8 / 48 0f 07`       | xor %eax,%eax ; swapgs ; sysretq       | return 0             |
| +289   | `65 48 8b 14 25 18 10 00 00`        | mov %gs:0x1018,%rdx (REAL_SEC)         | time body            |
| +298   | `48 85 ff`                          | test %rdi,%rdi                         | tloc NULL?           |
| +301   | `74 12`                             | je +18 → +321 (skip store; mov rdx→rax)|                      |
| +303   | `65 48 39 3c 25 30 10 00 00`        | cmp %rdi,%gs:0x1030                    |                      |
| +312   | `0f 86 00 ff ff ff`                 | jbe −256 → +62                         | rel32 OK             |
| +318   | `48 89 17`                          | mov %rdx,(%rdi)                        |                      |
| +321   | `48 89 d0`                          | mov %rdx,%rax                          | return REAL_SEC      |
| +324   | `0f 01 f8 / 48 0f 07`               | swapgs ; sysretq                       |                      |
| +330   | `48 85 ff / 74 19`                  | test %rdi ; je +25 → +360              | getcpu body          |
| +335   | `65 48 39 3c 25 30 10 00 00`        | cmp %rdi,%gs:0x1030                    |                      |
| +344   | `0f 86 e0 fe ff ff`                 | jbe −288 → +62                         | rel32 OK             |
| +350   | `65 8b 14 25 04 00 00 00`           | mov %gs:0x04,%edx (CPU_ID)             |                      |
| +358   | `89 17`                             | mov %edx,(%rdi)                        |                      |
| +360   | `48 85 f6 / 74 15`                  | test %rsi ; je +21 → +386              |                      |
| +365   | `65 48 39 34 25 30 10 00 00`        | cmp %rsi,%gs:0x1030                    |                      |
| +374   | `0f 86 c2 fe ff ff`                 | jbe −318 → +62                         | rel32 OK             |
| +380   | `45 31 d2 / 44 89 16`               | xor %r10d,%r10d ; mov %r10d,(%rsi)     | node = 0             |
| +386   | `31 c0 / 0f 01 f8 / 48 0f 07`       | xor %eax,%eax ; swapgs ; sysretq       | return 0             |

**Total LSTAR body = 394 B.**  Region budget at `+0x40..+0x200` =
448 B; 54 B headroom remaining.

Every gadget handler:

- restores user GS via `swapgs` before `sysretq` ✓
- preserves `RCX` (post-SYSCALL user RIP, consumed by sysretq) ✓
- preserves `R11` (saved user RFLAGS, consumed by sysretq) ✓

The clock body uses `%edx`/`%r10`/`%r8` for working state and only
writes `%rax` at the very end (`xor %eax, %eax` for the return
value). The time body uses `%rdx`. The getcpu body uses `%edx` and
`%r10d`. All three avoid `%rcx` and `%r11`. ✓

### Selector / SYSRETQ math (verified against AMD64 SDM Vol 3 §6.4.4)

GDT (after bootstrap-init extends it from 6 to 8 entries):

| idx | sel  | descriptor                  | DPL | use                         |
|-----|------|-----------------------------|-----|-----------------------------|
| 0   | 0x00 | null                        | -   |                             |
| 1   | 0x08 | code, L=1                   | 0   | ring-0 CS (SYSCALL loads)   |
| 2   | 0x10 | data                        | 0   | ring-0 DS/ES/FS/GS/SS       |
| 3   | 0x18 | unused (anchor)             | -   | SYSRETQ base offset slot    |
| 4   | 0x20 | data                        | **3** | **ring-3 DATA** (sel for SS) |
| 5   | 0x28 | code, L=1                   | **3** | **ring-3 CODE** (sel for CS) |
| 6+7 | 0x30 | TSS desc (16 B)             | 0   | TR                          |

The user's brief had (4=CS, 5=DS) — that's reversed from what the
code actually does (4=DS, 5=CS). The code's layout is
**SDM-compliant**: SYSRETQ in 64-bit mode loads CS = STAR[63:48] +
**16** = 0x18 + 16 = **0x28** (idx 5 = ring-3 code) and SS =
STAR[63:48] + **8** = 0x18 + 8 = **0x20** (idx 4 = ring-3 data).
Verified — `MSR_STAR = (0x18 << 48) | (0x08 << 32)` at thread.c
1673-1674.

The IRETQ frame at thread.c 2293-2300 uses CS = `0x2b` (sel 0x28
| RPL 3, idx 5 = ring-3 code) and SS = `0x23` (sel 0x20 | RPL 3,
idx 4 = ring-3 data). Consistent with SYSRETQ's derivation. ✓

### IRETQ frame builder (thread.c 2291-2303)

```c
frame[0] = regs->gp[HOST_IP];                              /* user RIP */
frame[1] = 0x2bULL;                                        /* ring-3 CS */
frame[2] = kvm_build_sysret_r11(regs->gp[HOST_EFLAGS]);    /* RFLAGS */
frame[3] = regs->gp[HOST_SP];                              /* user RSP */
frame[4] = 0x23ULL;                                        /* ring-3 SS */
```

`kvm_build_sysret_r11` ORs in `(1<<1)|(1<<9)|(3<<12)` = bit-1
reserved-1, IF=1, IOPL=3. That's the same RFLAGS shape SYSRETQ
would produce post-FMASK. Sound for IRETQ.

vCPU RIP set to `bootstrap_va + 0x4d0` and RSP to `bootstrap_va +
0x3000` (IST page base). IRETQ pops the 5-quadword frame from RSP
upward. The frame ends 40 B into the IST page, well below the
inner-#PF push range at the top.

---

## Byte-level errors found

**None in the LSTAR / IRETQ / #PF / #DF / #GP byte arrays
themselves.** I traced every offset, verified every rel8/rel32
displacement, and matched every `%gs:disp32` against the struct
field tables in `kvm_backend.h`. The bytecode is internally
consistent.

**One byte-adjacent semantic concern:**

- The **#PF handler** at `+0x4a0` is `e6 fb / 48 83 c4 08 / 48 cf`
  (out, add $8 rsp, iretq). The intent is clearly "OUT triggers
  VMEXIT, KVM advances RIP +2, then on KVM_RUN re-entry execute
  add+iretq". **In the actual flow the host never re-enters this
  way** — it always re-enters via the `KVM_BOOTSTRAP_IRETQ_OFFSET`
  gadget at `+0x4d0` with a freshly-built iretq frame on the IST
  stack base (offsets 0..0x28). The bytes after `out $fb` are dead
  code, only present as defense-in-depth. That's documented but
  worth flagging because someone reading the handler in isolation
  would be misled into thinking the `add $8, %rsp` actually pops
  the CPU-pushed error code.

- The **#DF/#GP handlers** are `out ; hlt` with **no error-code
  cleanup**. Consistent with the host treating both as fatal —
  the hlt is a backstop for a buggy host that doesn't kill the
  vCPU. ✓

- The `KVM_BOOTSTRAP_SYSRET_OFFSET` (0x4b0) bytes are documented
  as dead code (post-task #272). Confirmed nothing in
  `kvm_enter_guest` sets RIP to that offset. ✓

---

## Risk assessment

| Path                                  | "Loud if broken" | "Silent if broken" |
|---------------------------------------|------------------|--------------------|
| GDT load / SREGS                      | Triple-fault on first KVM_RUN — loud | — |
| LSTAR trampoline byte corruption      | — | **Wrong syscall return value, no exception** |
| Gadget state struct offset drift      | — | **Wrong pid/uid/etc returned, no exception** |
| IRETQ frame builder fields swapped    | #GP from IDT[13] → host port 0xf9 — semi-loud | If frame survives canonical checks: silent jump to wrong VA |
| KERNEL_GS_BASE drift                  | — | **All gadget reads return random bytes** |
| Bootstrap shadow leaf overwritten by fill | First gadget call faults at random VA — semi-loud (host #PF handler) but mis-classified as user fault | **If page happens to be readable: silent garbage read** |
| #PF/#DF/#GP IST sharing               | — under normal load | **Silent frame clobber on cascade** |
| TSS.IST[1] mis-encoding               | Triple-fault on first #PF — loud | — |
| GDT TSS descriptor (16 B)             | Triple-fault on KVM_SET_SREGS — loud | — |
| GDT user CS/SS swap (RPL collision)   | #GP on first SYSRETQ — loud | — |

The "silent if broken" rows are exactly the failure category that
matches the observed bug pattern. Of those, **bootstrap shadow
leaf overwritten by fill** and **KERNEL_GS_BASE drift** are the
two paths that can produce sustained, repeatable wild-pointer
behaviour without triggering an obvious diagnostic.

---

## Bootstrap page diagram (annotated)

```
                    GUEST VA                HOST VA              flags
  bootstrap_va  → +0x0000 ─ GDT[0..7]   ─── kvm_bootstrap_page  P
                  +0x0040 ─ LSTAR body  ───                     P
                  +0x0200 ─ TSS         ───                     P
                  +0x0280 ─ IDT[0..32]  ───                     P
                  +0x04a0 ─ #PF handler ───                     P
                  +0x04b0 ─ SYSRETQ (DEAD)                      P
                  +0x04c0 ─ #DF handler                         P
                  +0x04d0 ─ IRETQ gadget                        P
                  +0x04d8 ─ #GP handler                         P
                  +0x04e0..+0x0fff  zero pad                    P
                  +0x1000 ─ gadget state ─── kvm_gadget_state   P NX
                  +0x2000 ─ gadget vvar  ─── kvm_gadget_vvar    P RW NX
                  +0x3000 ─ IST stack    ─── kvm_bootstrap_page_stack
                                                                P RW NX
                  +0x3ff8     ┃         ← first push lands here on #PF
                  +0x4000 ─ STACK_TOP (exclusive; TSS.IST[1] = this)

  KERNEL_GS_BASE ─→ +0x1000 (after swapgs in LSTAR; %gs:0x08 = TGID,
                              %gs:0x1000+ reaches vvar via disp32)
  STAR base      ─→ 0x18 (CS=0x28|3=0x2b, SS=0x20|3=0x23)
  LSTAR          ─→ +0x0040
  TR (TSS sel)   ─→ 0x30 (GDT idx 6/7)
  IDT base       ─→ +0x0280, limit=0x20f
  GDT base       ─→ +0x0000, limit=0x3f
  CR3            ─→ kvm_shadow_mm_current()->pgd_gpa (per-mm)
```

The four pages that compose the bootstrap region (code+tables,
state, vvar, IST stack) are installed into every per-mm shadow PT
on every `kvm_enter_guest` (thread.c 1939-2044). The host writes
all four via the kernel-VA alias (outside the shadow PT). The
guest reads them via its CR3-walk through the shadow PT.

---

## Recommended changes

### Immediate (try first)

1.  **Add an alias-range guard to the install pass** in
    `kvm_shadow_fill_from_uml_pgd` (lifecycle.c around line 1306).
    Mirror the clear-pass guard already present at line 1248-1252:

    ```c
    /* before kvm_shadow_map_page(...): */
    if (alias_lo && va >= alias_lo && va < alias_hi)
        continue;
    ```

    This is a one-line defensive change that is identical in spirit
    to what the clear pass already does. It costs nothing on the
    common path (the conditional is a few cycles per PTE) and it
    closes the highest-risk silent-corruption hole in the bootstrap
    map.

2.  **Re-prime KERNEL_GS_BASE every kvm_enter_guest** (thread.c
    1644). Drop the `kernel_gs_base_primed` short-circuit, or move
    the prime call to immediately before `KVM_RUN` so any side-
    effecting write since the last `KVM_RUN` is repaired. Cost is
    one extra `KVM_SET_MSRS` (one MSR) — sub-µs.

3.  **Add a runtime LSTAR checksum.** On every kvm_enter_guest
    (or every Nth — say 64th — to amortise), recompute a CRC32 of
    `(char *)kvm_bootstrap_page + KVM_BOOTSTRAP_LSTAR_OFFSET` for
    `sizeof(kvm_bootstrap_lstar_bytes)` and compare against a
    boot-time-captured reference. Panic on mismatch with a hex
    dump of the divergent bytes. Catches:
    - in-guest ring-0 escape that rewrites the trampoline (would
      need to bypass W=0 — unlikely but possible via shadow
      ASID/IPI race);
    - host-side cosmic-ray / FPU-unaligned / wild-write corruption;
    - page-table aliasing where the bootstrap GPA gets remapped
      to the wrong host page (collision with another KVM memslot,
      etc).

### Medium-term

4.  **Replace hand-rolled byte arrays with assembled-from-source
    blobs.** Move the LSTAR body to a `.S` file in
    `arch/x86/um/kvm_backend/`, assemble at build time, and
    reference the symbols + sizes via linker exports. This:
    - eliminates byte-level transcription errors in future
      modifications;
    - makes `objdump -d` against the kernel image directly readable;
    - lets us emit `.byte` directives for any register/offset that
      truly must be fixed (e.g. `%gs:KVM_GADGET_OFF_TGID` becomes
      a symbolic reference);
    - lets a test in the build (run `objdump -d --no-show-raw-insn`
      and grep for expected mnemonics) catch toolchain-version
      surprises.

5.  **Separate IST slots for #PF/#DF/#GP.** Use IST[1]=#PF,
    IST[2]=#DF, IST[3]=#GP. Each gets its own dedicated stack
    page in the bootstrap region (or a contiguous 3-page IST
    stack with three TSS entries pointing at distinct tops).
    Eliminates the cascade-overwrite class of bugs.

6.  **Add IDT[1] (#DB) and IDT[6] (#UD) handlers.** Currently
    bare null entries — a guest TF=1 single-step or `ud2`
    triple-faults. Same shape as #GP: `out $f8 ; hlt` etc.

7.  **KUnit byte-checksum test.** Add a contract test that
    compares the in-memory LSTAR bytes against a golden array
    and fails the build if they drift. Boot-time KUnit already
    runs under integrated KVM; this is a 50-line addition.

### Long-term

8.  **Use a single VA reservation for the bootstrap region.**
    Plumb `kvm_bootstrap_va` through UML's mm allocator so user
    mmaps cannot collide. Today the only collision avoidance is
    "the alloc_page allocator happens to give us a kernel-region
    VA that user code rarely picks". An explicit reservation
    (e.g. `vm_area_struct` marker in init_mm at boot) makes the
    invariant load-bearing and verifiable.

---

## What I would do first

(In rough order, expecting the bug to reproduce on each step
unless the fix is decisive.)

1.  **Add the alias-range guard to the install pass** (item 1
    above). 5-line change. Boot integrated KVM, run
    `python3 -c "import unittest"`. If it passes, you've found
    the bug. If it fails, capture the new failure mode and go
    to step 2.

2.  **Add KERNEL_GS_BASE re-prime per entry** (item 2). Re-run.
    If passes, you've found the bug.

3.  **Add the LSTAR checksum** (item 3) and run the workload
    until first failure. The panic will tell you whether the
    bytes have actually been overwritten in the bootstrap page
    versus whether everything is intact and the bug is somewhere
    in the shadow PT / MSR drift / IRETQ frame setup.

4.  If checksum stays green, instrument the install pass with
    a `pr_warn_once` whenever the UML pgd has a present leaf at
    a VA in `[bootstrap_va, +4*PAGE_SIZE)`. That tells you
    whether the collision actually fires under the failing
    workload.

5.  If 1-3 don't repro the fix, the bug is most likely in the
    KVM_SYNC_X86_REGS marshalling path (perf-lever #2 at thread.c
    2311) or in `kvm_decode_syscall`'s handling of class-D /
    class-E syscalls — neither of which is bytes-in-the-bootstrap-
    page territory and would belong to a different review.

---

## Appendix — verification trace

I machine-walked the LSTAR byte array (thread.c 618-958), summing
the size of each emit and comparing against the documented offset.
Result: total = 394 B, all 30 documented offsets land at the
correct cumulative position, all 10 dispatch `je rel8` jumps land
at the documented handler entry, all 8 inner `jne/jbe rel32`
displacements land at the +62 fallback, and the 3 inner `je rel8`
jumps in the time/getcpu bodies all land at their documented
post-bounds-check entries. No off-by-one in any displacement.

The cmp/test pre-checks at +3 (NR_getcpu) and +15 (upper-byte
guard) correctly route 3-byte and 4-byte NRs out of the dispatch
path; the upper-byte guard prevents the documented
utimensat/preadv/mount_setattr aliasing.

The clock-body seqlock retry uses `mov %gs:SEQ,%edx ; test $1,%dl ;
... ; cmp %gs:SEQ,%edx`. Pre-read of SEQ goes into RDX (not RAX,
so RAX stays = NR=228 across a fallback). Post-read compares.
Standard seqlock pattern. ✓

The TASK_SIZE_CAP guard (`cmp %rsi/%rdi, %gs:0x1030 ; jbe →
fallback`) uses `jbe` because `cmp DST, SRC` sets flags from
`SRC - DST`; "below or equal" means SRC ≤ DST i.e. cap ≤ ptr i.e.
ptr ≥ cap. That's the correct direction for routing out-of-range
pointers to the fallback. ✓

For `time(2)` the NULL-tloc path takes `je +18` from offset +301
and lands at +321 = `mov %rdx, %rax` — correctly skips both the
bounds check and the store but still returns REAL_SEC in RAX. ✓

For `getcpu` the NULL-cpu and NULL-node paths take `je` to the
respective post-store labels, correctly skipping their bounds
checks and stores while still completing the other half of the
syscall. The deferred `xor %eax, %eax` at +386 ensures the return
value is 0 only after all bounds checks have either passed or
fallen through to fallback. ✓
