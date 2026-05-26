# 02 — vCPU lifecycle / register state / CPU setup review

Architecture review #2: scrutinise the vCPU lifecycle path that takes
register state from KVM_GET_REGS, mutates it through UML kernel code,
and pushes it back via KVM_SET_REGS / `run->s.regs.regs`. Companion to
01-mm-architecture.md and 03-bootstrap-bytes.md. Reviewed against the
known wild-pointer failure mode under `backend=force=kvm` (NULL+small
offsets like `0xab`; random 45-bit addresses like `0x441f0f66e0ff`).

## Executive summary

Three concerns dominate the review. In rough order of likely
contribution to the wild-pointer pattern:

1. **`exec_regs` leaks HOST FS_BASE/GS_BASE into the GUEST vCPU on
   every `execve`.** `flush_thread → kvm_init_thread_regs →
   get_safe_registers` copies the seccomp-probed `exec_regs` (or zero
   under KVM_ONLY) into the freshly-execve'd task's `gp[]`. That
   includes `gp[HOST_FS_BASE]`/`gp[HOST_GS_BASE]`, which the next
   `kvm_enter_guest` then writes into `sregs.fs.base / gs.base` and
   pushes via `KVM_SET_SREGS`. For a SECCOMP-built KVM, the value is
   the host-process glibc TLS pointer (a 47-bit user-space VA like
   `0x7f...`); for KVM_ONLY it is 0. Either way, the FIRST
   `%fs:offset` access by the dynamic loader / libc startup —
   typically a stack-protector canary read at `%fs:0x28` — dereferences
   `(host_tls_or_zero) + 0x28` IN THE GUEST'S VA SPACE, which is
   unmapped, producing exactly the failure pattern the user
   described. Both halves of the symptom (`NULL + small offsets`
   under KVM_ONLY; `random 45-bit garbage` when seccomp is
   compiled-in) match the two possible `exec_regs` contents.
2. **`KVM_EXIT_INTR` (the exit-reason, not the syscall `-EINTR`)
   unconditionally marshals kregs back into `regs->gp[]` (line 3075,
   `kvm_run_userspace`) without the in-kernel guard the
   `if (rc == -EINTR)` path uses (line 4103). When a host signal
   interrupts the guest mid-LSTAR-trampoline / mid-IRETQ /
   mid-#PF-handler, `kregs.rip` is a kernel VA inside the bootstrap
   page; the marshal then poisons `regs->gp[HOST_IP]` with that
   kernel VA and the next `kvm_enter_guest` builds an IRETQ frame
   whose `frame[0]` is a kernel address, faulting on first user-mode
   instruction fetch (US=0 page → ec=0x15 → wild-RIP cascade).
3. **`MSR_FMASK = 0`** (`kvm_enter_guest_program_msrs`, line 1682).
   On SYSCALL entry the CPU clears `RFLAGS &= ~MSR_FMASK`. With
   FMASK=0 the kernel runs the LSTAR trampoline with the user's
   IF/TF/DF/AC etc. exactly as the user left them. Two concrete
   problems: (a) if user RFLAGS had DF=1, the kernel runs `swapgs;
   cmp; ...; out` with DF=1 — single-instruction VMEXITs are still
   correct, but ANY string instruction inserted into the LSTAR
   trampoline (none today, but a future gadget might) would step
   backwards; (b) trap flag (TF=1) leaking into ring-0 produces an
   in-guest #DB at the FIRST instruction after SYSCALL, with no IDT
   handler installed — `KVM_EXIT_EXCEPTION` cascade. Lower-priority
   than #1 / #2 for the immediate symptom but a latent landmine.

The smoking gun is #1 — it explains the symptom directly, fires on
every execve, and tracks both observed register-poisoning patterns.
Recommendation: zero (or sanitise) `gp[HOST_FS_BASE]/gp[HOST_GS_BASE]`
in `kvm_init_thread_regs` after `get_safe_registers` returns.

## KVM_EXIT_* × state-touch matrix

Read at top of switch (UNCONDITIONAL, line 3066-3075):
- kregs ← `KVM_GET_REGS` (or `run->s.regs.regs` via sync_regs)
- `kvm_regs_to_uml_regs(regs, &kregs)` — overwrites every GP slot
  (RAX/RBX/RCX/RDX/RSI/RDI/RBP/RSP/R8..R15/RIP/RFLAGS) plus
  HOST_EFLAGS from kregs
- exit_sregs ← `KVM_GET_SREGS` (or sync_regs)
- `regs->is_user = (cs.selector & 3) != 0`

The unconditional marshal is the surface that all per-case paths
build on. For exits where kregs carries USER state (SYSCALL VMEXIT
out of LSTAR fallback, MMIO from a user instruction, real ring-3
INTR), this is correct. For exits where kregs carries KERNEL state
(any VMEXIT mid-trampoline / mid-#PF-handler / mid-IRETQ-gadget),
this writes a kernel RIP/RSP into `regs->gp[HOST_IP/SP]` — and
unless the case-specific code FIXES those slots back up, the
poisoned values feed straight into the next `kvm_enter_guest`'s
IRETQ frame.

| exit_reason          | source of kregs          | regs->gp[*] after dispatcher          | regs->faultinfo | sregs / MSRs touched | Notes |
|----------------------|--------------------------|----------------------------------------|------------------|----------------------|-------|
| `KVM_EXIT_IO` 0xf4 (SYSCALL) | mid-LSTAR trampoline (post `out`) | HOST_IP ← HOST_CX (line 2505), HOST_EFLAGS ← HOST_R11 (line 2506); HOST_AX ← syscall return; arch_prctl pushes MSR_FS_BASE / MSR_GS_BASE via KVM_SET_MSRS | none | sregs FS/GS base via `kvm_propagate_fs_gs_base` on arch_prctl only | OK on the happy path. RFLAGS is the kernel's post-LSTAR value at marshal time; the HOST_EFLAGS overwrite from R11 fixes it. |
| `KVM_EXIT_IO` 0xfb (#PF) | mid-#PF-handler (post `out`) | HOST_IP/HOST_SP/HOST_EFLAGS overwritten from IST frame (lines 3274-3276); HOST_AX ← user RAX from kregs | trap_no=14, error_code=ec from IST+0, cr2=KVM_GET_SREGS.cr2 | sregs.cr2 read via sync_regs or KVM_GET_SREGS | OK if the IST math is right; `ist_off = kregs.rsp - (kvm_bootstrap_va + 3*PAGE_SIZE)`. **Subtle**: if EINTR fires AFTER the OUT but BEFORE we're in the case block, the unconditional marshal at line 3075 has already poisoned regs->gp; we'd recover via the case-specific overrides — but only if we actually entered this case. |
| `KVM_EXIT_IO` 0xf9 (#GP) | mid-#GP-handler | HOST_IP/HOST_SP/HOST_EFLAGS overwritten from IST frame (lines 3700-3702); SIGSEGV dispatched | trap_no=13, error_code, cr2=0 | none | OK (mirrors PF). |
| `KVM_EXIT_IO` 0xfa (#DF) | mid-#DF-handler | n/a — panics | n/a | sregs.cr2 read for diagnostic | Always fatal. |
| `KVM_EXIT_IO` 0xf5 (sysretq port) | n/a — panics | n/a | n/a | n/a | Indicates broken guest control flow. |
| `KVM_EXIT_HLT` | wherever guest was HLTed | left as marshalled (RIP advanced past HLT by KVM) | none | none | HLT shouldn't fire from user (privileged) — exit indicates kernel-side HLT or the #DF handler's defensive backstop. is_user from CPL is correct. |
| `KVM_EXIT_INTR` | wherever guest was interrupted (could be ring-3 OR mid-trampoline ring-0!) | **NO FIX-UP**. regs->gp = whatever the unconditional marshal stored. | none | none | **BUG candidate #2** — if the guest was at CPL=0 (mid-trampoline / #PF handler / IRETQ gadget), HOST_IP/HOST_SP are kernel addresses and the next kvm_enter_guest's IRETQ frame loads them as user RIP/RSP. The `rc == -EINTR` path at line 4103 has the in_kernel guard, but a `KVM_EXIT_INTR` exit (KVM_RUN returns 0 with run->exit_reason = INTR) does not. |
| `KVM_EXIT_MMIO` | user instruction faulting on EPT | HOST_IP = user RIP (correct); HOST_SP = user RSP (correct); SIGSEGV dispatched via faultinfo | trap_no=14, error_code from is_write/is_user, cr2=phys_addr+uml_physmem | none — user state, no fixup needed | OK. |
| `KVM_EXIT_SHUTDOWN` / `FAIL_ENTRY` / `INTERNAL_ERROR` / `EXCEPTION` | n/a — panics with diagnostic | n/a | n/a | KVM_GET_SREGS for diagnostic | Always fatal. |
| `default` | n/a — panics | n/a | n/a | n/a | Unknown exit reason. |

State NEVER touched anywhere in `kvm_run_userspace`'s switch:
- **DR0..DR7** (debug registers) — KVM keeps default; UML doesn't program them. Ptrace single-step / hw breakpoint does nothing under KVM today.
- **MSR_IA32_DEBUGCTL** — never set; KVM default.
- **MSR_IA32_SYSENTER_*** — never set; SYSENTER not used.
- **MSR_IA32_PAT** — never set; KVM default (likely WB everywhere).
- **MSR_TSC** — never adjusted; vCPU TSC is whatever KVM gives it.
- **XCR0** — never set; we suppress XSAVE via CPUID.
- **CR2** — read on #PF / SHUTDOWN; never written (CPU writes on faults).
- **CR8** (TPR) — never touched.
- **MSR_APIC_BASE** — never touched (no APIC needed for ncpus=1).
- **TSS.RSP0/RSP1/RSP2** — set to zero. Only IST[1] is populated.
- **IDT entries OTHER than [8]/[13]/[14]** — zero (a hit panics if the host is healthy).

## Specific bugs

### Proven

**P1. `kvm_init_thread_regs` leaks `exec_regs` FS/GS base into the
guest. (`arch/um/backend/kvm/thread.c:301-333`, in concert with
`arch/um/os-Linux/registers.c:37` and
`arch/um/kernel/exec.c:24-31`.)**

Mechanism:
1. `flush_thread()` (called on every `execve`) invokes
   `um_backend_dispatch(init_thread_regs, gp, fp)`.
2. `kvm_init_thread_regs` calls `get_safe_registers(gp, fp)` which
   `memcpy(gp, exec_regs, sizeof(exec_regs))`.
3. `exec_regs` was populated either (a) by the seccomp probe in
   `init_seccomp` (if seccomp is compiled-in), which captures the
   stub child's mcontext including its host glibc TLS in
   FS_BASE/GS_BASE, or (b) zero-initialised under KVM_ONLY.
4. After execve, `start_thread(regs, eip, esp)` overwrites only
   HOST_IP and HOST_SP. `gp[HOST_FS_BASE]` and `gp[HOST_GS_BASE]`
   stay at the `exec_regs` values.
5. On the next `kvm_run_userspace → kvm_enter_guest`, line 2231-2232:
   ```c
   sregs.fs.base = regs->gp[HOST_FS_BASE];
   sregs.gs.base = regs->gp[HOST_GS_BASE];
   ```
6. `KVM_SET_SREGS` pushes those into the vCPU's MSR_FS_BASE /
   MSR_GS_BASE.
7. The user's first FS-relative load reads from `(host_tls_or_zero)
   + offset` IN THE GUEST'S CR3 — typically unmapped → #PF cascade.

The classic stack-protector canary read `mov %fs:0x28, %rax` (emitted
by GCC on every `-fstack-protector` function entry, which includes
nearly all of glibc / libpython / libssl / etc.) is the exact
trigger. Symptom matches: under KVM_ONLY, `0 + 0x28 = 0x28` →
"NULL+small offsets like 0xab"; under SECCOMP-built KVM,
`(host_TLS_47bit) + 0x28` → "random 45-bit addresses like
0x441f0f66e0ff".

The seccomp/ptrace backends don't see this because their stub
process IS a host process — host TLS in FS_BASE works correctly until
the guest's `arch_prctl(ARCH_SET_FS, ...)` propagates the new value
through `set_stub_state`. The KVM backend has no equivalent
"bootstrap window where host TLS is meaningful".

Fix: in `kvm_init_thread_regs`, zero `gp[HOST_FS_BASE]` and
`gp[HOST_GS_BASE]` explicitly after `get_safe_registers`. The dynamic
loader's first arch_prctl will install the real values; in the
meantime, FS-relative loads fault at `0+offset` (small) which is
clearly distinguishable from real bugs and is what KVM_ONLY already
exhibits (modulo the latent issue that even small-offset faults are
unrecoverable, but that's the dynamic loader's problem to fix —
proper toolchains avoid `%fs:` access before the FS_BASE setup).

Stronger fix: ALSO zero CS / SS / DS / ES / FS / GS selectors and
HOST_R11 / HOST_CX, since none of those have a meaningful "guest
default" value either. The minimal patch is just the two FS/GS
bases; the broader patch establishes a clean "fresh execve" baseline.

### Suspected

**S1. `KVM_EXIT_INTR` (the exit-reason) path lacks the in-kernel
guard. (`arch/um/backend/kvm/thread.c:3776-3777`, with reference to
the working guard at lines 4115-4153.)**

The `if (rc == -EINTR)` branch in `out_read_regs` reads CPL via
KVM_GET_SREGS, sets `in_kernel = !regs->is_user`, and SKIPS the
`kvm_regs_to_uml_regs(regs, &kregs)` if in_kernel — preventing a
kernel RIP from poisoning regs->gp[HOST_IP].

But that guard only fires when `KVM_RUN` itself returns `-EINTR`.
When KVM_RUN returns 0 with `run->exit_reason = KVM_EXIT_INTR` (a
distinct case — KVM-handled signal completion), the switch statement
hits `case KVM_EXIT_INTR: goto out_read_regs;` AFTER the
unconditional marshal at line 3075 has already poisoned regs->gp[*]
with kregs (which can be a kernel RIP if the signal fired
mid-trampoline).

Then `out_read_regs` runs; `rc == 0`, so the EINTR-recovery branch
is not taken; regs->gp keeps the poisoned values; interrupt_end()
runs; the next `kvm_run_userspace` iteration calls `kvm_enter_guest`
which builds an IRETQ frame with `frame[0] = regs->gp[HOST_IP] =
kernel RIP`; IRETQ to ring-3 fetches at a kernel address; either
`#PF` (US=0 page) or `#GP` (kernel address might be canonical but
fetched from a US=0 mapping). The cascade is observable as
"unexpected user RIP at first faulting instruction".

Fix: lift the in-kernel guard out of the `if (rc == -EINTR)` block so
it also applies on `case KVM_EXIT_INTR`. The simplest shape: do the
CPL check BEFORE the unconditional marshal, and skip the marshal if
in-kernel. Or: re-do the case-specific override for this path
(restore HOST_IP/SP/EFLAGS from a saved pre-entry copy if in_kernel).

Probability: medium. Wild-pointer manifests on long workloads but
not at first instruction; SIGALRM is the most likely trigger and
fires every ~10ms. Whether INTR-mid-trampoline is hit often enough
to explain "100% failure on python3 import unittest" depends on host
preempt rate.

**S2. `MSR_FMASK = 0`. (`arch/um/backend/kvm/thread.c:1681-1683`.)**

SYSCALL clears `RFLAGS &= ~MSR_FMASK` on entry. With FMASK=0, no
flags are masked — IF, TF, DF, AC, etc. all leak from user RFLAGS
into the kernel-side LSTAR trampoline.

Concrete failure modes:
- TF=1 (single-step) → in-guest #DB on the first instruction after
  SYSCALL (the `swapgs` at +0). IDT[1] is unpopulated → cascade to
  #GP/#DF. `KVM_EXIT_IO` on #GP_PORT, host delivers SIGSEGV — but
  the user was just doing a normal SYSCALL. Or, worse, IDT[1] is
  truly absent (only IDT[8]/[13]/[14] are set), so any #DB
  cascades immediately to #DF and panics.
- AC=1 (alignment check) + CR0.AM is unset (we don't set CR0.AM
  either) → AC is moot, but if a future change sets CR0.AM the
  combination becomes hostile.
- DF=1 → string ops in the trampoline step backwards. Today the
  trampoline has no string ops, but the gadget design admits future
  in-guest `mov %gs:offset` patterns; LFS/LDS-style operations in
  newer gadgets could break.
- IF=1 in ring-0 LSTAR is technically allowed since there's no APIC
  delivering interrupts to the vCPU, but it's against convention
  (Linux clears IF on SYSCALL entry via FMASK |= 0x200).

Fix: program FMASK with at least
`0x247FD5 = TF|IF|DF|IOPL|NT|RF|VM|AC|VIF|VIP|ID|reserved` (the
standard Linux FMASK value). Minimum: TF (1<<8) and IF (1<<9).

Probability: low for the immediate symptom (the user's RFLAGS would
need TF=1, which doesn't happen organically). But it's a latent
correctness hole.

**S3. CPUID exposes features whose corresponding CR4 / OS-side
enable bits are NOT set, beyond the AVX/XSAVE/FSGSBASE family
already masked.**

Audit of the CPUID masking in `lifecycle.c:416-526`:
- AVX/XSAVE/F16C/AVX2/AVX512 family: masked. ✓
- FSGSBASE: masked. ✓
- RDRAND/RDSEED: masked (record/replay determinism). ✓

Not masked (and CR4 doesn't enable):
- **SMEP (CR4.SMEP, leaf 7.0 EBX bit 7)**: CR4.SMEP is NOT set, but
  CPUID still advertises SMEP. Modern glibc may probe and assume
  hardware SMEP. Generally benign for user-space (SMEP is a kernel
  protection); if the guest kernel ever consults CPUID.SMEP it could
  enable user-flag checks that fail without CR4.SMEP. Low risk.
- **SMAP (CR4.SMAP, leaf 7.0 EBX bit 20)**: CR4.SMAP is NOT set.
  Similar low-risk observation. The `clac`/`stac` instructions are
  no-ops without CR4.SMAP; they don't fault.
- **PCID (CR4.PCIDE, leaf 1 ECX bit 17)**: CR4.PCIDE is NOT set.
  INVPCID instructions would #UD. Low impact for UML's single-mm vCPU.
- **UMIP (CR4.UMIP, leaf 7.0 ECX bit 2)**: CR4.UMIP is NOT set. SLDT
  / STR / SGDT / SIDT / SMSW from ring-3 still work without #GP.
  Userspace code that probes CR4.UMIP via `getauxval(AT_HWCAP2)`
  might take a wrong branch. Very low risk.
- **CET (leaf 7.0 ECX bit 7 + EDX bit 20)**: CR4.CET unset. Modern
  glibc / dl_main may probe CET CPUID; with CR4.CET=0 the
  ENDBR64/ENDBR32 instructions are no-ops, and writes to CET MSRs
  would #GP. UML's userspace currently doesn't use CET, but
  Ubuntu 24+ ships glibc with CET-aware code paths.
- **TSC_DEADLINE / X2APIC (leaf 1 ECX bits 24/21)**: not relevant
  without an APIC.
- **SHA / AESNI (leaf 7.0 EBX bit 29 / leaf 1 ECX bit 25)**:
  instruction-level features that don't depend on CR4. Safe.

Overall low risk for the wild-pointer symptom; CET is the most
likely future landmine.

**S4. `kvm_propagate_fs_gs_base` doesn't update the SREGS-skip
cache. (`arch/um/backend/kvm/thread.c:1563-1599`.)**

`kvm_propagate_fs_gs_base` writes MSR_FS_BASE / MSR_GS_BASE via
KVM_SET_MSRS but does NOT update `ctx->cached_fs_base /
ctx->cached_gs_base`. On the next `kvm_enter_guest`, the cache check
at line 2160-2165 compares `regs->gp[HOST_FS_BASE] (= NEW)` against
`cached_fs_base (= OLD)`, sees a mismatch, takes the SREGS path and
re-pushes the new value via KVM_SET_SREGS. Net result: the cache
miss costs an extra ioctl per arch_prctl, but state IS correct.

Not a correctness bug, but the comment chain about "cached state"
is misleading. Documentation fix only.

**S5. The IRETQ-gadget comment says CS=0x33 / SS=0x2b but the code
uses CS=0x2b / SS=0x23. (`arch/um/backend/kvm/thread.c:1004-1009`
vs. lines 2296-2299.)**

Comment bug, not a code bug. The CODE matches the GDT layout
(idx 5 / DPL=3 = 0x2b for code, idx 4 / DPL=3 = 0x23 for data).
Documentation fix.

### Speculative

**SPEC1. Per-task FPU hash slot is keyed by `task_struct *`, not by
mm or user-state generation. After `execve`, the task_struct is the
same; the hash slot still holds the PRE-execve FPU state. The next
restore loads stale XMM/YMM into the freshly-execve'd task.
(`arch/um/backend/kvm/thread.c:97-178`.)**

POSIX dictates fresh FPU state after execve. The host kernel handles
this via `flush_thread → fpu__clear_user_states`; UML's KVM backend
has no equivalent. SSE-based memcpy / memset using stale XMM source
registers would corrupt destination. Not a wild-pointer trigger
directly, but a class of subtle misbehaviour matching the symptom
profile.

Fix: in `kvm_init_thread_regs` (or a new exec-time hook), call
`KVM_SET_FPU` with a zero-initialised `kvm_fpu` to reset the vCPU
FPU, AND drop the per-task hash slot for `current` so a stale slot
doesn't get restored on the next context switch.

**SPEC2. Segment selectors for DS/ES are hardcoded to 0x10 (kernel
data, DPL=0) in `kvm_fill_longmode_segments`. After IRETQ to CPL=3,
DS/ES retain selector 0x10. Per Intel SDM §6.8.3, the CPU may
NULL-out segment registers whose descriptors' DPL < CPL when
returning to user mode.**

In long mode, DS/ES are largely ignored (treated as flat with base
0). FS/GS use their explicit base registers. So the NULL-out
behaviour, if it fires, doesn't break user code — but `mov %ds,
%ax` from user would read 0 instead of 0x10, which a paranoid CPUID
probe might notice.

Not a wild-pointer trigger. Hardening fix: explicitly load CS/SS
ring-3 selectors via the IRETQ frame (already done) and DS/ES/FS/GS
via SREGS to ring-3-DPL descriptors (idx 4 = 0x20, DPL=3) before
the first cross-CPL transition. Or accept the NULL-out as benign.

**SPEC3. `CR0.MP=1` and CR0.TS unset. CR0.MP=1 says "monitor
coprocessor present", which means FWAIT will trap if CR0.EM=1. We
don't set EM, so FWAIT is fine. CR0.TS=0 means the FPU is always
"available" — no #NM on first FPU access. This is intentional but
inconsistent with how Linux normally lazy-saves FPU. Probably not a
bug; documenting only.**

**SPEC4. The `cmp $0x18` for sched_yield at LSTAR offset +50 hijacks
ANY syscall whose low byte is 0x18 — but the upper-byte guard at +15
catches them. Verified the guard fires correctly. No bug.**

## Round-trip data flow diagram

```
+-----------------------------------------------------------------+
|                       UML kernel context                        |
|                                                                 |
|  task A's regs (struct uml_pt_regs in task A's kernel stack)    |
|     gp[HOST_AX]        ← syscall return value                   |
|     gp[HOST_BX..R15]   ← user GPRs (read at last VMEXIT)        |
|     gp[HOST_IP]        ← user RIP (set by kvm_decode_syscall    |
|                          from HOST_CX = post-SYSCALL RIP)       |
|     gp[HOST_SP]        ← user RSP                               |
|     gp[HOST_EFLAGS]    ← user RFLAGS (set from HOST_R11)        |
|     gp[HOST_FS_BASE]   ← from arch_prctl OR exec_regs (BUG #1)  |
|     gp[HOST_GS_BASE]   ← from arch_prctl OR exec_regs (BUG #1)  |
|     gp[HOST_CS/SS]     ← never used by KVM backend              |
+--------------------------+--------------------------------------+
                           |
                           |  kvm_enter_guest:
                           |    kvm_uml_regs_to_kvm_regs(&kregs, regs)
                           |    [marshals 16 GPRs + RIP + RFLAGS]
                           |
                           |    sregs.fs.base = gp[HOST_FS_BASE]    ← BUG #1 hits here
                           |    sregs.gs.base = gp[HOST_GS_BASE]
                           |    sregs.cs/ds/es/fs/gs/ss ← static template (DPL=0)
                           |    sregs.cr3 = shadow PGD GPA
                           |
                           |    Build IRETQ frame on IST stack:
                           |      frame[0] = gp[HOST_IP]            ← BUG #2 hits here
                           |      frame[1] = 0x2b                   (CS)
                           |      frame[2] = build_sysret_r11(gp[HOST_EFLAGS])
                           |      frame[3] = gp[HOST_SP]
                           |      frame[4] = 0x23                   (SS)
                           |
                           |    kregs.rip = bootstrap_va + IRETQ_OFFSET
                           |    kregs.rsp = bootstrap_va + IST_PAGE_OFFSET
                           |    kregs.rflags = 0x2 (ring-0 RFLAGS)
                           |
                           |    KVM_SET_SREGS (or skip if cached match)
                           |    KVM_SET_REGS  (or sync_regs)
                           |    KVM_SET_MSRS for STAR/LSTAR/FMASK   (FMASK=0 — BUG #3)
                           |    KVM_SET_MSRS for MSR_KERNEL_GS_BASE (one-shot)
                           v
+--------------------------+--------------------------------------+
|                       KVM (host kernel)                         |
|  vCPU MSRs:                                                     |
|    LSTAR = bootstrap_va + 0x40                                  |
|    STAR  = (0x18 << 48) | (0x08 << 32)                          |
|    FMASK = 0                                ← BUG #3 latent     |
|    KERNEL_GS_BASE = gadget_state_va                             |
|    FS_BASE / GS_BASE = sregs.fs.base / gs.base                  |
|                                                                 |
|  vCPU GPRs ← kregs                                              |
|  vCPU SREGS ← sregs (cached)                                    |
|  vCPU FPU   ← per-task hash slot (or stale)                     |
+--------------------------+--------------------------------------+
                           |
                           |  KVM_RUN → enter long-mode ring-0
                           v
                  IRETQ at bootstrap_va + 0x4d0
                  Pops [RIP, CS, RFLAGS, RSP, SS] from
                    bootstrap_va + IST_OFFSET
                  Loads CS=0x2b (ring-3 code), SS=0x23 (ring-3 data),
                    sets CPL=3, jumps to RIP.
                           |
                           v
+-----------------------------------------------------------------+
|                       guest user mode (CPL=3)                   |
|  RAX..R15 = whatever kregs had (preserved by IRETQ)             |
|  RIP = user RIP   (BUG #1: very first %fs-relative load faults  |
|                    if FS_BASE was leaked from exec_regs)        |
|  RFLAGS = user RFLAGS                                           |
|  RSP = user RSP                                                 |
|  CS = 0x2b, SS = 0x23                                           |
|  DS/ES = 0x10 (or NULL after CPL transition)                    |
|  FS = 0x10 (selector), .base = gp[HOST_FS_BASE]                 |
|  GS = 0x10 (selector), .base = gp[HOST_GS_BASE]                 |
|                                                                 |
|  Eventually: SYSCALL or fault → VMEXIT to host                  |
+--------------------------+--------------------------------------+
                           |
                           v
                  kvm_run_userspace: KVM_RUN returns
                  Read kregs (UNCONDITIONAL marshal at line 3075)
                  Read sregs (for is_user via CPL)
                  Switch on exit_reason:
                    KVM_EXIT_INTR: NO FIX-UP    ← BUG #2 hits here
                    KVM_EXIT_IO 0xfb (#PF): override IP/SP/EFLAGS from IST
                    KVM_EXIT_IO 0xf4 (SYSCALL): override IP from CX
                    ...
```

## "Comprehensive vCPU state checklist" — proposed additions

Today's reset / programming surface (verified above):

- **Programmed at first kvm_enter_guest** (and never re-programmed
  unless invalidated):
  - LSTAR (= bootstrap_va + 0x40)
  - STAR (= 0x0018'0000'0008'0000)
  - FMASK (= 0)               ← should be at least `0x340` (TF|IF|DF)
  - MSR_KERNEL_GS_BASE (= gadget_state_va)
  - CPUID (host passthrough w/ AVX/XSAVE/FSGSBASE/RDRAND/RDSEED masked)
- **Programmed on every kvm_enter_guest UNLESS cached match**:
  - cs/ds/es/fs/gs/ss segment descriptors (full sregs.{cs,ds,...})
  - GDT base + limit
  - IDT base + limit
  - TR (TSS selector + descriptor)
  - CR0 / CR3 / CR4 / EFER
  - sregs.fs.base ← from gp[HOST_FS_BASE]
  - sregs.gs.base ← from gp[HOST_GS_BASE]
- **Programmed on every kvm_enter_guest** (no skip):
  - kregs.rip / kregs.rsp / kregs.rflags ← bootstrap IRETQ shape
  - kregs.r* ← marshalled from regs->gp[*]
  - IRETQ frame on IST stack page (5 quadwords)
- **Programmed on syscall return only**:
  - MSR_FS_BASE / MSR_GS_BASE (only on arch_prctl)
- **NEVER programmed**:
  - DR0..DR7
  - MSR_IA32_DEBUGCTL
  - MSR_IA32_SYSENTER_CS/EIP/ESP
  - MSR_IA32_PAT
  - MSR_TSC / MSR_TSC_AUX
  - XCR0
  - CR2 (CPU writes; host reads on #PF)
  - CR8 (TPR)
  - APIC base
  - TSS.RSP0/1/2 (only IST[1])
  - IDT entries other than [8]/[13]/[14]
  - DR registers

Recommended additions to the checklist:

| What | Why | When to program |
|------|-----|-----------------|
| `MSR_FMASK = 0x340` (or 0x247FD5) | Mask TF/IF (and DF) on SYSCALL entry | Once, with STAR/LSTAR |
| Zero `gp[HOST_FS_BASE]` / `gp[HOST_GS_BASE]` after `get_safe_registers` | Don't leak host TLS into guest | In `kvm_init_thread_regs` |
| `KVM_SET_FPU` with zeroed kvm_fpu on execve | POSIX clean-FPU semantic | In `kvm_init_thread_regs` (or new exec hook) |
| Drop per-task FPU hash slot on execve | Don't restore stale FPU after exec | Same hook |
| In-kernel CPL guard for `KVM_EXIT_INTR` exit-reason path | Avoid kernel-RIP poisoning regs->gp | Before switch's INTR case OR in switch's INTR case |
| Update `cached_fs_base` / `cached_gs_base` in `kvm_propagate_fs_gs_base` | Avoid extra ioctl on next entry | After successful KVM_SET_MSRS |
| Reset DR0..DR7 (especially DR7=0x400) on first entry | Defensive — avoid host-debug-state leakage | Once at vCPU creation or first enter |
| Set CR0.AM = 0 explicitly | Today implicit; document | Already implicit, no change needed |

## What I would do first

**Zero `gp[HOST_FS_BASE]` and `gp[HOST_GS_BASE]` in
`kvm_init_thread_regs` after the `get_safe_registers` call.**

Single-line patch with the highest probability of resolving the
wild-pointer symptom. If the bug stems from `exec_regs` leaking host
TLS into the guest's FS_BASE (proven mechanism + matching symptom
pattern), this fix removes it. If the bug is elsewhere, the patch is
harmless: the dynamic loader's first arch_prctl will install the
correct value, and any FS-relative access before that point would
have faulted under the prior code anyway (just at a host TLS address
instead of zero).

```c
void kvm_init_thread_regs(unsigned long *gp, unsigned long *fp)
{
    get_safe_registers(gp, fp);

    /*
     * Bug fix: get_safe_registers() copies exec_regs (the
     * seccomp-probed host-stub baseline). For the SECCOMP and
     * PTRACE backends those host-TLS FS/GS bases are valid
     * because the stub IS a host process; for the KVM backend
     * they are host-VA pointers that don't map anything in the
     * guest's CR3. The first %fs-relative load by the dynamic
     * loader (typically the stack-protector canary at %fs:0x28)
     * faults at (host_TLS + 0x28) IN THE GUEST'S VA SPACE,
     * which is unmapped, producing the wild-pointer symptom.
     *
     * Zero the bases here so the dynamic loader's first
     * arch_prctl(ARCH_SET_FS, real_tls) installs the right
     * value. Pre-arch_prctl FS access still faults, but at a
     * deterministic NULL+offset rather than a 45-bit garbage
     * address — and properly-built dynamic loaders don't make
     * such accesses.
     */
    gp[HOST_FS_BASE] = 0;
    gp[HOST_GS_BASE] = 0;

#ifdef CONFIG_UM_BACKEND_KVM_ONLY
    if (!gp[HOST_IP])
        gp[HOST_IP] = STUB_START;
#endif
}
```

Verification:
- Boot UML with `backend=force=kvm` and `init=/bin/echo`. If echo
  succeeds, the regression is in the FS_BASE leak path.
- If echo fails the same way, follow with bug #2 (KVM_EXIT_INTR
  in-kernel guard) — the next-most-likely contributor.
- For confirmation, instrument `kvm_enter_guest` to log
  `sregs.fs.base / gs.base` before every KVM_SET_SREGS; correlate
  the host-VA pattern against the wild-pointer faults observed.

If the patch above doesn't resolve it, prioritise S1 (KVM_EXIT_INTR
guard) next, then revisit S2 (FMASK) for latent-issue hardening.

## File references

Primary code paths reviewed:
- `arch/um/backend/kvm/thread.c` lines 82-178 (per-task FPU
  save/restore), 192-299 (kvm_context_switch), 301-333
  (kvm_init_thread_regs), 410-545 (bootstrap layout constants),
  618-958 (LSTAR trampoline bytes), 991-1093 (other handler bytes),
  1125-1437 (kvm_enter_guest_init_bootstrap), 1498-1518
  (kvm_gadget_fault_nr), 1563-1599 (kvm_propagate_fs_gs_base),
  1618-1659 (kvm_enter_guest_program_kernel_gs_base), 1662-1719
  (kvm_enter_guest_program_msrs), 1733-1801 (regs marshalling),
  1844-2379 (kvm_enter_guest), 2457-2911 (kvm_decode_syscall),
  2922-4094 (kvm_run_userspace).
- `arch/um/backend/kvm/sregs.c` lines 100-138 (GDT layout), 219-303
  (long-mode segment + control register setup).
- `arch/um/backend/kvm/lifecycle.c` lines 71-333 (kvm_init), 363-545
  (kvm_ensure_cpuid_done).
- `arch/um/backend/kvm/kvm_backend.h` lines 34-150 (struct kvm_um),
  221-248 (kvm_gadget_state).
- `arch/um/backend/kvm/snapshot.c` lines 57-91 (kvm_snapshot,
  msr index list), 99-201 (capture).
- `arch/um/kernel/exec.c` lines 24-39 (flush_thread, start_thread).
- `arch/um/kernel/process.c` lines 149-189 (copy_thread).
- `arch/x86/um/syscalls_64.c` lines 16-46 (sys_arch_prctl).
- `arch/x86/um/user-offsets.c` lines 41-69 (HOST_* register slot
  numbering).
- `arch/x86/um/shared/sysdep/ptrace_64.h` lines 20-21 (HOST_FS_BASE
  / HOST_GS_BASE).
- `arch/um/os-Linux/registers.c` lines 17-43 (exec_regs,
  init_pid_registers, get_safe_registers).
- `arch/um/os-Linux/start_up.c` lines 241-366 (init_seccomp →
  exec_regs population), 528-615 (os_early_checks).
- `arch/um/backend/seccomp/trap_user.c` (comparison reference).
- `arch/um/backend/ptrace/trap_user.c` (comparison reference).
