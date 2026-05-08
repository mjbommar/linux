# SMP-T57 — XSAVE/AVX enable: fix-plan memo

**Date:** 2026-05-07
**Author:** Claude (planning agent)
**Status:** PLAN. No code changes proposed in this memo; the only
artefact is this file.
**Build base:** `d28a74cae98d` (HEAD on `umlctl-deploy`).
**Predecessor memos:**
- `state-audit/24-smp-t57-vmmethod-bisect.md` (root cause)
- `state-audit/23-smp-t55-perf-regression-plan.md` (per-vCPU FPU-dirty
  epoch — already landed at `fd2f9639b0ce`).
- `state-audit/15-smp-t26-t27-fpu-cross-task-leak-FIXED.md` (always-
  GET_FPU revert of H.2 lazy-FPU; cross-task XMM leak guarantee).
- `state-audit/22-smp-t41-stress-and-perf.md` (perf debt context).

---

## 1. TL;DR

`stress-ng --vm --verify` SIGILLs on kvm-v2 because the VEX-encoded
`vpxor %xmm6,%xmm6,%xmm6` at user RIP `0x550000e9d71e` raises #UD —
on a host CPU that fully supports AVX. Memo `24` §7.6-H proved this
byte-for-byte against `objdump -d /usr/bin/stress-ng` (kernel
`copy_from_user` returns `bytes_at_ip-8 = 00000826860f3ef9
0008bd41f6efc9c5`, identical to the on-disk binary). A valid AVX
opcode raising #UD is, on AVX-capable silicon, a near-deterministic
signature of `CR4.OSXSAVE=0` ∨ `XCR0.YMM=0` ∨ `XSAVE-header.xstate_bv`
missing YMM at decode time.

The chosen fix path is **enable AVX/XSAVE in kvm-v2** rather than
"mask harder." Empirical evidence shows the curated CPUID mask
(`kvm_v2_curate_cpuid` at `arch/um/backend/kvm-v2/vcpu.c:139-195`)
DOES suppress AVX from `__builtin_cpu_supports` (verified via
`AT_HWCAP=0x178bfbff` in userland), but stress-ng's
`__attribute__((target_clones(...)))` IFUNC dispatcher landed on the
AVX variant anyway — either because IFUNC resolution caches before
KVM_SET_CPUID2 commits, because text-segment fall-through lands inside
an AVX variant's bytes, or because glibc's CPU detection peeks at
`/proc/cpuinfo` (UML leaks the host's full feature flags via
`arch/um/os-Linux/start_up.c:222-251` → `boot_cpu_data.x86_capability`
→ `arch/um/kernel/um_arch.c:88-92`). Whatever the proximate path,
"keep masking AVX off" is fragile against the next stress-ng-class
binary; the structural fix is to back the user-visible AVX feature
with real architectural state.

Estimated scope: ~5 small site changes (CR4 init bit, one new
KVM_SET_XCRS call at vcpu_create_one, three CPUID-mask-relaxation
edits, and switching the `iotrap_fpu` slot from `struct kvm_fpu`
(legacy 512 B) to `struct kvm_xsave` (4096 B). Risk class: **medium**
— the FPU/XSAVE marshal touches all the surfaces SMP-T26/T27 and
SMP-T55 spent a week debugging; correctness regression on cross-task
YMM leakage is the dominant residual risk.

A follow-up `§4-A` audit (below) shows we can land the fix in **two
phases**: Phase A (XCR0+CR4 only, keep CPUID mask, keep `kvm_fpu`)
restores AVX exception architecture without changing the marshal
struct; Phase B (full XSAVE area + CPUID un-mask) is needed only if
post-Phase-A stress-ng still fails. Phase A is the minimum-risk
landing; Phase B is the substrate-correct steady state.

---

## 2. Audit of current state

Each row below cites the live code at HEAD `d28a74cae98d`. File:line
tags are absolute paths inside `arch/um/backend/kvm-v2/`.

### 2.1 CR4 setup

`vcpu.c:546` (inside `kvm_v2_install_production_sregs`, the
vcpu_create_one helper):

```c
sregs.cr4  = X86_CR4_PAE | X86_CR4_OSFXSR | X86_CR4_OSXMMEXCPT;
```

Also `vcpu.c:1400` (per-dispatch, inside `kvm_v2_load_user_sregs`):

```c
sregs->cr4 ^= X86_CR4_PGE;
```

The per-dispatch line toggles bit 7 (PGE) for the TLB-flush forcing
trick (see SMP-T11 / SMP-T33 territory); it preserves all other CR4
bits. So whatever CR4 mask is set at `vcpu_create_one` time
propagates forward. Bit 18 (`X86_CR4_OSXSAVE`) is **NOT** set today.

The header comment at `vcpu.c:25-33` is explicit about this:

> `family because v1's sregs setup never enabled CR4.OSXSAVE / set`
> `XCR0; FSGSBASE because CR4.FSGSBASE was off).  v2's sregs setup is`
> `not yet written — it lives in Phase C.2/C.3. Until then matching`
> `v1's mask exactly avoids re-discovering the same #UD/#GP cliffs...`
> `Phase C may revisit when proper XSAVE/XCR0 plumbing lands.`

This memo IS the "Phase C may revisit" event the original comment
forecast. The fix has been an explicit deferred-work item since
init.

**Change required:** add `X86_CR4_OSXSAVE` (bit 18) to the create-
time mask AND ensure the per-dispatch PGE toggle preserves it. Since
the per-dispatch line is `^= PGE`, a one-liner CR4 init change
suffices — the toggle naturally preserves OSXSAVE.

### 2.2 XCR0

`grep -rn "KVM_SET_XCRS\|KVM_GET_XCRS\|XCR0" arch/um/backend/kvm-v2/`
returns four hits, all in source comments (`vcpu.c:28`, `vcpu.c:131`).
**There is NO KVM_SET_XCRS call anywhere in the kvm-v2 backend.** The
guest's XCR0 is whatever KVM defaulted at vcpu_create — typically
`bit 0 (X87) = 1`, all other bits zero. With XCR0.YMM (bit 2) clear,
VEX-encoded AVX instructions raise #UD even when CR4.OSXSAVE is set.

The header comment at `vcpu.c:130-131` documents the absence:

> `XSAVE/AVX/AVX2/AVX512 family because guest CR4.OSXSAVE is unset`
> `and XCR0 is unprogrammed; FSGSBASE because CR4.FSGSBASE is off).`

KVM exposes both ioctls — `KVM_GET_XCRS` and `KVM_SET_XCRS`, gated on
`KVM_CAP_XCRS` (UAPI: `include/uapi/linux/kvm.h:1411-1412`; struct
`kvm_xcrs` in `arch/x86/include/uapi/asm/kvm.h:404-417`). UML kvm-v2
never calls them. The `init.c:125-204` cap-negotiation pass probes
`KVM_CAP_SYNC_REGS` + `KVM_CAP_SET_GUEST_DEBUG` + `KVM_CAP_HYPERV` and
nothing else; `KVM_CAP_XCRS` and `KVM_CAP_XSAVE` are not even
queried.

**Change required:** add a one-time `KVM_SET_XCRS` call (and an
optional cap probe in init.c). Site: end of `kvm_v2_vcpu_create_one`
(after `kvm_v2_install_production_sregs`, before `kvm_v2_install_
signal_mask`). Set XCR0 to at minimum `XSTATE_FP | XSTATE_SSE |
XSTATE_YMM` = bits 0, 1, 2 = `0x7`. This pairs with the CR4.OSXSAVE
add at §2.1.

### 2.3 CPUID curation

`vcpu.c:139-195` is the full curation function. Its current behaviour
(per HEAD):

```c
static void kvm_v2_curate_cpuid(struct kvm_cpuid2 *cpuid)
{
    unsigned int i;

    for (i = 0; i < cpuid->nent; i++) {
        struct kvm_cpuid_entry2 *e = &cpuid->entries[i];

        if (e->function == 1 && e->index == 0) {
            /* Leaf 1 ECX:
             *   bit 12 = FMA, 26 = XSAVE, 27 = OSXSAVE,
             *   28 = AVX, 29 = F16C, 30 = RDRAND.
             */
            e->ecx &= ~((1U << 12) | (1U << 26) | (1U << 27) |
                        (1U << 28) | (1U << 29) | (1U << 30));
        }
        if (e->function == 7 && e->index == 0) {
            /* Leaf 7.0 EBX: FSGSBASE, AVX2, AVX512F/DQ/...,
             *   RDSEED, AVX512IFMA, AVX512PF/ER/CD, SHA, AVX512BW/VL.
             */
            e->ebx &= ~((1U << 0)  | (1U << 5)  | (1U << 16) |
                        (1U << 17) | (1U << 18) | (1U << 21) |
                        (1U << 26) | (1U << 27) | (1U << 28) |
                        (1U << 29) | (1U << 30) | (1U << 31));
            /* Leaf 7.0 ECX: AVX512VBMI/2, GFNI, VAES, VPCLMULQDQ,
             *   AVX512VNNI, AVX512BITALG, AVX512VPOPCNTDQ.
             */
            e->ecx &= ~((1U << 1)  | (1U << 6)  | (1U << 8)  |
                        (1U << 9)  | (1U << 10) | (1U << 11) |
                        (1U << 12) | (1U << 14));
            /* Leaf 7.0 EDX (matches v1 archive). */
            e->edx &= ~((1U << 2) | (1U << 3) | (1U << 8));
        }
        if (e->function == 0xD)
            e->eax = e->ebx = e->ecx = e->edx = 0;
    }
}
```

Bits cleared today (relevant for AVX/XSAVE):
- Leaf 1 ECX bit 12 (FMA), 26 (XSAVE), 27 (OSXSAVE), 28 (AVX), 29
  (F16C). [bit 30 = RDRAND, deterministic-replay concern, stays
  cleared.]
- Leaf 7.0 EBX bit 0 (FSGSBASE), 5 (AVX2). [Plus all AVX-512.]
- Leaf 7.0 ECX bit 9 (VAES), 10 (VPCLMULQDQ). [Plus all AVX-512.]
- Leaf 0xD (full XSAVE state-component descriptor) zeroed.

**Change required:** un-mask the bits whose architectural state we
are now committing to back. Concretely:
- Leaf 1 ECX: keep masking RDRAND (bit 30) for replay determinism;
  un-mask 26 (XSAVE), 27 (OSXSAVE), 28 (AVX). FMA (12) and F16C (29)
  are AVX-128 derivatives — un-mask if we're committing to
  YMM-enabled XSAVE; safe to keep.
- Leaf 7.0 EBX: un-mask bit 5 (AVX2). Keep 0 (FSGSBASE — separate
  workstream), 18 (RDSEED — replay determinism), and all AVX-512
  bits (16, 17, 21, 26-31) initially. Higher tiers can be enabled
  in a follow-up.
- Leaf 0xD: this is the XSAVE state-component descriptor — leaf 0xD
  sub-leaf 0 reports XCR0-supported components, sub-leaf 1 reports
  XSS-supported components, sub-leaves 2..63 report individual
  state-component sizes. Today it's blanket-zeroed. With YMM
  enabled, glibc and userland want sub-leaf 0 EAX bits 0/1/2 set
  (X87/SSE/YMM) and sub-leaf 2 (offset/size of YMM) populated
  correctly. The cleanest path is to **stop** zeroing leaf 0xD and
  let KVM's `KVM_GET_SUPPORTED_CPUID`-curated values pass through.
  KVM curates leaf 0xD against the active XCR0 mask, so this
  becomes correct as soon as XCR0 is set (§2.2).

The order matters: KVM_SET_XCRS before KVM_SET_CPUID2 may not
matter (CPUID is a data-only ioctl), but to be safe, install CPUID
last per dispatch initialisation order.

### 2.4 KVM_GET_FPU / KVM_SET_FPU sites

The relevant call-sites (already audited in memos 15 and 23):

| Site | File:line | Direction | Trigger | Already gated |
|---|---|---|---|---|
| pre-run install of per-task snapshot | `vcpu.c:2025-2036` | SET | `iotrap_fpu_valid==true` | yes |
| post-run capture into per-task slot | `vcpu.c:2122-2138` | GET | `fpu_dirty || fpu_owner_task != current` (T55 epoch) | yes |
| fork-time parent-snapshot capture | `vcpu.c:2500-2539` | GET (into `to->kvm_v2.fpu`) | every `arch_copy_thread` | one-shot |
| switch-out capture | `vcpu.c:2570-2651` | GET (into `from->thread.kvm_v2.fpu`) | UML scheduler `_switch_to` | per-switch |
| install on first run after fork/switch | `vcpu.c:2720-2767` (`kvm_v2_fpu_install_on_first_run`) | SET (from `a->kvm_v2.fpu`) | `fpu_valid==true` | one-shot |

Five SET/GET sites in total, plus the `KVM_GET_FPU` in
`syscall_trap.c:2061` (the `kvm_v2_handle_io_nm` #NM handler — not
visited by lazy-FPU dispatches). All operate on `struct kvm_fpu`
(legacy 512-byte FXSAVE area).

The struct kvm_fpu byte layout (`arch/x86/include/uapi/asm/kvm.h:
175-187`):

```c
struct kvm_fpu {
    __u8  fpr[8][16];   /* x87 ST0-ST7 */
    __u16 fcw, fsw;
    __u8  ftwx, pad1;
    __u16 last_opcode;
    __u64 last_ip, last_dp;
    __u8  xmm[16][16];  /* XMM0-XMM15 (low 128 bits of YMM0-YMM15) */
    __u32 mxcsr, pad2;
};
```

Total: 416 bytes (the "512 byte" colloquialism is loose; struct kvm_fpu
is the FXSAVE area subset). `xmm[16][16]` covers only the lower 128
bits of YMM0-YMM15 — bits 255:128 of every YMM register are
**unaddressable** through `KVM_GET_FPU`/`KVM_SET_FPU`. As a result,
in the current Phase A path (CR4.OSXSAVE+XCR0.YMM enabled, but still
using `kvm_fpu`), the upper 128 bits of YMM are saved/restored via
KVM's internal guest_fpu mechanism — but our cross-task per-task
snapshot only carries the lower 128 bits. **Cross-task YMM upper-half
leak is structurally possible** in Phase A.

This is the key Phase-A → Phase-B distinction.

### 2.5 XSAVE area

`KVM_GET_XSAVE` / `KVM_SET_XSAVE` are listed in
`include/uapi/linux/kvm.h:1408-1412` and documented in
`Documentation/virt/kvm/api.rst:1660-1705`:

> `KVM_GET_XSAVE2 and KVM_SET_XSAVE write and read as many bytes`
> `as are returned by KVM_CHECK_EXTENSION(KVM_CAP_XSAVE2),`
> `respectively, when invoked on the vm file descriptor.`
>
> `The size value returned by KVM_CHECK_EXTENSION(KVM_CAP_XSAVE2)`
> `will always be at least 4096.`

UAPI (`arch/x86/include/uapi/asm/kvm.h:386-402`):

```c
struct kvm_xsave {
    __u32 region[1024];          /* 4096 bytes — XSAVE area */
    __DECLARE_FLEX_ARRAY(__u32, extra);
};
```

UML kvm-v2 issues neither ioctl. **Phase B requires switching the
FPU marshal struct from `struct kvm_fpu` (416 B in
`arch_thread.kvm_v2.iotrap_fpu` and `arch_thread.kvm_v2.fpu`) to
`struct kvm_xsave` (4096 B + KVM_CAP_XSAVE2 dynamic extras).** The
storage cost is ~+3.6 KB per task struct in the kvm-v2 build,
matched by ~+3.6 KB per pool member (16 vCPUs in the maximal config
== ~58 KB extra). Acceptable.

### 2.6 /proc/cpuinfo leak

UML's `/proc/cpuinfo` handler is `show_cpuinfo` at
`arch/um/kernel/um_arch.c:72-99`. It iterates
`boot_cpu_data.x86_capability[]` via the standard `cpu_has` macro and
emits any flag the host had at boot. The capability bitmap is
populated by `get_host_cpu_features` in
`arch/um/os-Linux/start_up.c:222-251`, which `fopen`s the host
`/proc/cpuinfo` and parses the `flags:` line. **No filtering against
the curated KVM CPUID happens at any layer** — the UML kernel
believes it has every host feature for purposes of `/proc/cpuinfo`
display.

This is unrelated to KVM_SET_CPUID2 (that controls what `cpuid`
instruction returns inside the guest; `/proc/cpuinfo` is sourced from
`boot_cpu_data` populated at host-startup time). Userland tools that
read `/proc/cpuinfo` (likely lscpu, possibly some glibc code paths)
get a wrong answer either direction:

- Pre-fix: `/proc/cpuinfo` says "yes AVX," CPUID/HWCAP say "no AVX,"
  any AVX execution faults → SMP-T57.
- Post-fix (after this memo): `/proc/cpuinfo` and CPUID/HWCAP both
  say "yes AVX," AVX executes correctly. Out of step temporarily for
  whatever Phase doesn't yet enable bit-X, but at least no SIGILL.

**Change required (defer to a separate task):** none for SMP-T57. The
leak is orthogonal — it's userland reading the wrong source of truth.
We can land a `show_cpuinfo` patch later that filters
`boot_cpu_data.x86_capability` against an `nm v2_curated_cpu_mask[]`
table; not in this fix.

### 2.7 Audit summary table

| Site | File:line | Today | Phase A | Phase B (if needed) |
|---|---|---|---|---|
| `kvm_v2_install_production_sregs` CR4 | `vcpu.c:546` | `PAE\|OSFXSR\|OSXMMEXCPT` | add `OSXSAVE` | (same) |
| `kvm_v2_load_user_sregs` per-dispatch CR4 toggle | `vcpu.c:1400` | `^= PGE` | unchanged (preserves OSXSAVE) | (same) |
| `kvm_v2_curate_cpuid` leaf 1 ECX | `vcpu.c:152-153` | masks 12,26,27,28,29,30 | un-mask 26,27,28 | un-mask 12,29 too |
| `kvm_v2_curate_cpuid` leaf 7.0 EBX | `vcpu.c:165-168` | masks 0,5,16-31 | un-mask 5 (AVX2) | (same; AVX-512 still off) |
| `kvm_v2_curate_cpuid` leaf 0xD | `vcpu.c:192-193` | zeroed | leave alone (KVM curates) | (same) |
| `KVM_SET_XCRS` at vcpu_create_one | none | none | **NEW**: set XCR0=`0x7` | (same) |
| `arch_thread.kvm_v2.iotrap_fpu` | `processor_64.h:76-77` | `struct kvm_fpu` (416 B) | unchanged | switch to `struct kvm_xsave` |
| `arch_thread.kvm_v2.fpu` | `processor_64.h:30-31` | `struct kvm_fpu` (416 B) | unchanged | switch to `struct kvm_xsave` |
| pre-run install at `vcpu.c:2025-2036` | `vcpu.c:2025` | `KVM_SET_FPU` | unchanged | `KVM_SET_XSAVE` |
| post-run capture at `vcpu.c:2127-2138` | `vcpu.c:2131` | `KVM_GET_FPU` | unchanged | `KVM_GET_XSAVE` |
| fork-time capture at `vcpu.c:2523-2524` | `vcpu.c:2523` | `KVM_GET_FPU` | unchanged | `KVM_GET_XSAVE` |
| switch-out capture at `vcpu.c:2635-2636` | `vcpu.c:2635` | `KVM_GET_FPU` | unchanged | `KVM_GET_XSAVE` |
| install_on_first_run at `vcpu.c:2727-2728` | `vcpu.c:2727` | `KVM_SET_FPU` | unchanged | `KVM_SET_XSAVE` |
| SMP-T55 dirty-epoch | `kvm_v2_backend.h:422-423` | gates `KVM_GET_FPU` | gates same | gates `KVM_GET_XSAVE` |
| `init.c` cap probe | `init.c:125-204` | probes SYNC_REGS, GUEST_DEBUG, HYPERV | optional: add `KVM_CAP_XCRS` | + `KVM_CAP_XSAVE` / `XSAVE2` |

---

## 3. Fix design — Option A (enable AVX/XSAVE)

The plan splits cleanly into two phases. **Phase A is the primary
target** of this memo and is sufficient to unblock SMP-T57's
deterministic SIGILL repro (the AVX opcode goes from #UD to executing
correctly). **Phase B** is the substrate-correct steady state and
closes the upper-128-bit-YMM cross-task leak corner case; it lands
only if Phase A leaves a residual cross-task-YMM bug.

### 3.1 Phase A — minimum-change AVX enable

Per-site changes:

#### A.1: CR4

`arch/um/backend/kvm-v2/vcpu.c:546` — change:

```c
sregs.cr4 = X86_CR4_PAE | X86_CR4_OSFXSR | X86_CR4_OSXMMEXCPT;
```

to:

```c
sregs.cr4 = X86_CR4_PAE | X86_CR4_OSFXSR | X86_CR4_OSXMMEXCPT |
            X86_CR4_OSXSAVE;
```

Cross-task / first-run edge cases:

- **First KVM_RUN:** the seed mmap (`run->s.regs.sregs = sregs;` at
  `vcpu.c:571`) carries OSXSAVE forward. KVM's `kvm_is_valid_sregs`
  validator (`arch/x86/kvm/x86.c:12426-12449`) does not reject
  CR4.OSXSAVE=1 with EFER.LMA=1; that combination is the normal
  long-mode-with-XSAVE state.
- **Per-dispatch:** `kvm_v2_load_user_sregs` at `vcpu.c:1400` does
  `sregs->cr4 ^= X86_CR4_PGE` only; OSXSAVE is preserved across the
  toggle.
- **Cross-task arrival:** the cross-task full-`KVM_SET_SREGS` at
  `vcpu.c:1548-1568` (SMP-T33) ships the sync-regs mmap value, which
  carries OSXSAVE forward.
- **EINTR snapshot replay:** any future replay path that re-marshals
  sregs into the mmap must include OSXSAVE — but today there is no
  such path that bypasses the sync-regs mmap.

Pseudocode at the only site:

```c
// vcpu.c:544..547 (kvm_v2_install_production_sregs)
sregs.cr0  = X86_CR0_PE | X86_CR0_MP | X86_CR0_NE |
             X86_CR0_WP | X86_CR0_PG;
sregs.cr4  = X86_CR4_PAE | X86_CR4_OSFXSR | X86_CR4_OSXMMEXCPT |
             X86_CR4_OSXSAVE;            // <— ADDED
sregs.efer = EFER_SCE | EFER_LME | EFER_LMA | EFER_NX;
```

#### A.2: XCR0 — new KVM_SET_XCRS at vcpu_create_one

Add a new helper `kvm_v2_install_xcrs(int vcpu_fd)` and call it from
`kvm_v2_vcpu_create_one` between `kvm_v2_install_production_sregs`
(line 991) and `kvm_v2_install_signal_mask` (line 1007).

```c
/*
 * SMP-T57: enable XSAVE state for X87/SSE/YMM. Pairs with
 * CR4.OSXSAVE in install_production_sregs and the AVX/XSAVE
 * un-mask in kvm_v2_curate_cpuid. Without this set, VEX-encoded
 * AVX instructions raise #UD even when CR4.OSXSAVE=1 and CPUID
 * advertises AVX — XCR0 is the architectural enable for the
 * extended state-save area.
 *
 * Bits set:
 *   bit 0 (X87)  : always required
 *   bit 1 (SSE)  : XMM0-XMM15 (already implicit via OSFXSR but
 *                  required by XCR0 ABI when bit 2 is set)
 *   bit 2 (YMM)  : upper 128 bits of YMM0-YMM15 (AVX/AVX2)
 *
 * Higher tiers (AVX-512 = bits 5/6/7, AMX = bits 17/18) stay clear
 * pending a follow-up that also un-masks the corresponding CPUID
 * bits.
 */
static int kvm_v2_install_xcrs(int vcpu_fd)
{
    struct kvm_xcrs xcrs = {
        .nr_xcrs = 1,
        .xcrs[0] = { .xcr = 0, .value = 0x7 },
    };
    int rc = os_ioctl_generic(vcpu_fd, KVM_SET_XCRS,
                              (unsigned long)&xcrs);
    if (rc < 0) {
        pr_err("um: kvm-v2 install_xcrs: KVM_SET_XCRS(vcpu_fd=%d) failed (%d)\n",
               vcpu_fd, rc);
        return rc;
    }
    pr_info("um: kvm-v2 install_xcrs: XCR0=0x7 (FP|SSE|YMM) on vcpu_fd=%d\n",
            vcpu_fd);
    return 0;
}
```

Cross-task / first-run edge cases:

- **First KVM_RUN:** XCR0 is a vCPU-state register, not a per-task
  register. Setting it once at vcpu_create_one means every task that
  dispatches on that vCPU sees the same XCR0 (bits 0/1/2). That is
  the correct behaviour — XCR0 is per-vCPU not per-task.
- **Cross-task arrival:** no change. KVM does not let the guest write
  XCR0 (XSETBV traps to userspace as `KVM_EXIT_X86_RDMSR`-like; we
  don't handle that exit). The architectural enable stays where we
  set it. If the guest tries XSETBV from CPL=3, it #GPs (CPL>0
  XSETBV is illegal). If a future guest helper at CPL=0 tries
  XSETBV, KVM exits and we'd see it — currently no such helper
  exists in v2's gadget.
- **`init.c` cap probe (optional):** add a `KVM_CAP_XCRS` probe in
  the optional-cap pass. The cap is universally supported on
  KVM/x86; failure is highly unlikely. If the probe fails, panic at
  init time rather than silently shipping a half-broken setup.

#### A.3: CPUID un-mask (Leaf 1 ECX bits 26/27/28; Leaf 7.0 EBX bit 5; Leaf 0xD passthrough)

Edit `kvm_v2_curate_cpuid` at `vcpu.c:139-195`:

```c
static void kvm_v2_curate_cpuid(struct kvm_cpuid2 *cpuid)
{
    unsigned int i;
    for (i = 0; i < cpuid->nent; i++) {
        struct kvm_cpuid_entry2 *e = &cpuid->entries[i];

        if (e->function == 1 && e->index == 0) {
            /*
             * SMP-T57: un-mask bits 26 (XSAVE), 27 (OSXSAVE),
             * 28 (AVX). FMA (12) and F16C (29) are AVX-128
             * derivatives that work with YMM enabled — keep
             * un-masked too. RDRAND (30) stays masked for
             * record-replay determinism.
             */
            e->ecx &= ~(1U << 30);   // RDRAND only
        }
        if (e->function == 7 && e->index == 0) {
            /*
             * SMP-T57: un-mask bit 5 (AVX2). Keep FSGSBASE (0)
             * — separate workstream. Keep RDSEED (18) — replay
             * determinism. Keep AVX-512 (16,17,21,26-31) for
             * follow-up.
             */
            e->ebx &= ~((1U << 0)  | (1U << 16) |
                        (1U << 17) | (1U << 18) | (1U << 21) |
                        (1U << 26) | (1U << 27) | (1U << 28) |
                        (1U << 29) | (1U << 30) | (1U << 31));
            /* Leaf 7.0 ECX: keep AVX-512 + GFNI/VAES/VPCLMULQDQ
             * masked (Phase B candidate, not Phase A). */
            e->ecx &= ~((1U << 1)  | (1U << 6)  | (1U << 8)  |
                        (1U << 9)  | (1U << 10) | (1U << 11) |
                        (1U << 12) | (1U << 14));
            /* Leaf 7.0 EDX (matches v1 archive): keep AVX-512_4VNNIW
             * / 4FMAPS / VP2INTERSECT masked. */
            e->edx &= ~((1U << 2) | (1U << 3) | (1U << 8));
        }
        /*
         * SMP-T57: leaf 0xD is the XSAVE state-component
         * descriptor. With OSXSAVE+XCR0.YMM live, KVM curates this
         * leaf against the active XCR0 mask — let it pass through
         * so glibc / libcrypto get correct sub-leaf 0/2 sizes. The
         * old "zero everything" policy was correct only when
         * OSXSAVE was off.
         */
        /* (no leaf 0xD edit) */
    }
}
```

Edge cases:
- **Pre-init guest CPUID call:** the curated mask is installed lazily
  at first `KVM_RUN` (per `vcpu.c:198-228`, the buddy-allocator
  comment). Until then KVM-default CPUID is live — which advertises
  AVX. Userland binaries won't hit CPUID before first KVM_RUN
  (init/0 has not even been launched). Safe.
- **Cross-VM:** v2 is single-VM; not relevant.
- **CPUID in guest from CPL=0 trampoline:** the gadget body is
  hand-written and does not execute CPUID; safe.

#### A.4: Marshal — KEEP `struct kvm_fpu`

Phase A leaves the marshal struct unchanged. The `iotrap_fpu` and
`fpu` fields stay as `struct kvm_fpu`; the SET/GET ioctls stay as
`KVM_SET_FPU`/`KVM_GET_FPU`.

**Why this works for AVX:** KVM's vcpu->arch.guest_fpu manages the
full XSAVE area internally and hands out the FXSAVE-shaped subset on
KVM_GET_FPU. With XCR0.YMM enabled, the upper 128 bits of YMM0-YMM15
are tracked in KVM's internal state but not visible through
KVM_GET_FPU. For same-task back-to-back dispatches on the same vCPU,
KVM's internal save-restore preserves the upper 128 bits. For
**cross-task** dispatches, our snapshot captures only the lower 128
bits — KVM keeps whatever the previous task left in the upper 128
bits.

In Phase A, the cross-task YMM upper-128 leak is **structurally
present** but only matters if:
1. Task X writes YMM upper-128 (via VEX-encoded AVX).
2. KVM_RUN exits while upper-128 holds X's data.
3. Task Y enters on the same vCPU. Pre-run SET_FPU restores task
   Y's lower-128 + x87 + MXCSR but does NOT touch upper-128.
4. Task Y reads YMM upper-128 (e.g., `vmovaps %ymm0, (%mem)`)
   without first writing it.

This is the same class of bug as SMP-T26/T27 but on a different
register span. The SMP-T55 dirty-epoch flag (`vcpu->fpu_dirty`,
`vcpu->fpu_owner_task`) gates `KVM_GET_FPU` correctly for the
lower-128 leak but **does not address the upper-128**.

**Mitigation:** require that all AVX/AVX2 callers in tested
workloads either initialise YMM before reading (the standard
`vpxor %ymmN,%ymmN,%ymmN` zero-idiom — exactly what stress-ng
attempts at the SIGILL site) or accept that a fresh task arriving
on a vCPU starts with YMM in some prior state. glibc, libstdc++,
and stress-ng's IFUNC variants are well-behaved here (they zero-
init YMM before use). If a Phase-A test surfaces a real leak, fall
through to Phase B.

#### A.5: SMP-T55 dirty-epoch interaction

The per-vCPU `fpu_dirty` flag (added at `fd2f9639b0ce`, plumbing in
`kvm_v2_backend.h:395-422`) gates the post-vmexit `KVM_GET_FPU` at
`vcpu.c:2127`:

```c
if (vcpu->fpu_dirty || vcpu->fpu_owner_task != current) {
    fpu_rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_FPU, ...);
    ...
}
```

In Phase A, this is correct as-is — `KVM_GET_FPU` reads the legacy
FXSAVE subset. The optimization (skip when dirty=false &&
owner==current) preserves correctness because KVM's internal guest_fpu
is already coherent with our last GET, by the same epoch reasoning
that proved out for FXSAVE state.

In Phase B (below), the flag still gates `KVM_GET_XSAVE` on the same
two predicates — no logic change, only the ioctl number changes.

### 3.2 Phase B — full XSAVE area + extended CPUID surface (deferred / contingent)

Phase B is contingent on Phase A residual failures. Diagnostic gate:
the cross-task YMM probe in §4.3. If that probe shows zero leaks
post-Phase-A, Phase B is **structural cleanup, not correctness fix**
and can defer to a separate landing window.

#### B.1: Switch storage to `struct kvm_xsave`

`arch/x86/um/asm/processor_64.h:30-31, 76-77`:

```c
struct {
    struct kvm_xsave fpu;          // was: struct kvm_fpu fpu;
    bool fpu_valid;
    ...
    struct kvm_xsave iotrap_fpu;   // was: struct kvm_fpu iotrap_fpu;
    bool iotrap_fpu_valid;
    ...
};
```

Storage cost: ~+3.6 KB per `arch_thread` (2 × 4096 - 2 × 416 =
~7360 bytes; modern x86 task_struct is on the order of 10 KB so this
is ~70% bloat). Acceptable for the kvm-v2 build, but worth a
follow-up to refactor (e.g., embed only one `struct kvm_xsave` and
recompute fork-snapshot from `iotrap_fpu` on next dispatch).

The `INIT_ARCH_THREAD` macro (`processor_64.h:134-138`) and
`arch_flush_thread` / `arch_copy_thread` need no edit — they only
touch `*_valid` booleans, not the FPU bytes.

#### B.2: Switch ioctls

All five sites (table at §2.7 rightmost column):
- `vcpu.c:2026` `KVM_SET_FPU` → `KVM_SET_XSAVE`
- `vcpu.c:2131` `KVM_GET_FPU` → `KVM_GET_XSAVE`
- `vcpu.c:2523` `KVM_GET_FPU` → `KVM_GET_XSAVE`
- `vcpu.c:2635` `KVM_GET_FPU` → `KVM_GET_XSAVE`
- `vcpu.c:2727` `KVM_SET_FPU` → `KVM_SET_XSAVE`

Plus `syscall_trap.c:2061` (in `kvm_v2_handle_io_nm`) if it touches
FPU directly — verify in audit.

The ioctls are size-stable as long as `KVM_CAP_XSAVE2`'s reported
size matches `sizeof(struct kvm_xsave) = 4096`. With AMX disabled in
CPUID (which Phase B does NOT change), KVM_CAP_XSAVE2 returns 4096
and the legacy ioctls (without trailing extra[]) work fine.

#### B.3: Header `xstate_bv` + `xcomp_bv` validation

The XSAVE area at offset 512 contains the XSAVE header:

```
offset 512..519:  xstate_bv  (which components are present in this save)
offset 520..527:  xcomp_bv   (compaction format flag + present mask)
offset 528..575:  reserved (must be zero)
```

When `KVM_SET_XSAVE` consumes our buffer, KVM validates that
`xstate_bv` is a subset of XCR0. For Phase B, `xstate_bv` must have
bits 0/1/2 set whenever the corresponding components are non-trivial
(i.e., on a fresh task we set `xstate_bv = 0` so KVM uses init
state for all three; on a captured task we trust whatever
`KVM_GET_XSAVE` returned).

The "fresh task" path is `kvm_v2_fpu_install_on_first_run` at
`vcpu.c:2720-2767`. The current `else` branch of the
`if (a->kvm_v2.fpu_valid)` test is empty (relies on KVM-default
init state); in Phase B it should explicitly install a zero-ed
`struct kvm_xsave` with `xstate_bv = 0` so KVM clears all components
to init state. Equivalent to today's behaviour, just made explicit.

#### B.4: CPUID Leaf 0xD pass-through (already in Phase A.3)

Phase B doesn't add to the CPUID changes from Phase A.3 — the leaf
0xD pass-through is the same edit. But Phase B may also un-mask FMA
(Leaf 1 ECX bit 12) and F16C (Leaf 1 ECX bit 29) if they were
deferred in Phase A.

### 3.3 init.c cap probe addition

Add to `kvm_v2_init` (between current required-cap probes and the
`HYPERV` optional probe):

```c
rc = probe_cap(fd, KVM_CAP_XCRS, "KVM_CAP_XCRS",
               true, KVM_V2_CAP_XCRS, &caps);
if (rc) goto err_close;
rc = probe_cap(fd, KVM_CAP_XSAVE, "KVM_CAP_XSAVE",
               true, KVM_V2_CAP_XSAVE, &caps);
if (rc) goto err_close;
```

(Plus `#define KVM_V2_CAP_XCRS (1ULL << 2)` and
`KVM_V2_CAP_XSAVE (1ULL << 3)` in the bitmap header.)

Both caps are universally available on KVM/x86; failing the probe
means we are running on a non-x86 KVM port (UML kvm-v2 doesn't
support that today anyway) or a heavily stripped KVM build. Either
way, panic is correct.

---

## 4. Test plan

### 4.1 Pre-fix repro (already on file)

Per memo `24` §7.6-H: `stress-ng --vm 1 --vm-bytes 16M
--vm-method=mscan --verify --timeout 5s` under `ncpus=1, mem=512M`
reproduces SIGILL at `0x550000e9d71e`. The repro is bit-deterministic
across boots. Captured at `/tmp/t57-7-6-h-probe.log` previously;
re-run pre-fix to anchor the post-fix delta.

### 4.2 Post-fix gate

| Probe | Pre-fix | Post-Phase-A target | Post-Phase-B target |
|---|---|---|---|
| `stress-ng --vm 1 --vm-method=mscan --verify --timeout 5s` | SIGILL | PASS | PASS |
| `stress-ng --vm --verify` (full 38-method matrix) | 16/38 PASS | 38/38 PASS | 38/38 PASS |
| substrate gate kvm-v2 | PASS=25/3/3 | PASS=25/3/3 | PASS=25/3/3 |
| cpython-parity | 21/21 | 21/21 | 21/21 |
| mt-mini SMP T=8 ncpus=4 N=30 | 30/30 | 30/30 | 30/30 |
| perf-py-startup ratio | 1.10 (max 1.20) | ≤ 1.20 | ≤ 1.25 (XSAVE > FXSAVE) |
| threaded-fork-malloc 24k forks × 6 boots | 0 CHILD_FAIL | 0 CHILD_FAIL | 0 CHILD_FAIL |
| bench-py speedup (Zen 4) | 4.00× | ≥ 3.7× | ≥ 3.5× |
| bench-micro getpid | ~1050× | ~1050× | ~1050× |

The perf bands above tolerate a small XSAVE > FXSAVE cost in Phase B
(~3-5% per dispatch on cross-task transitions). Phase A should be
perf-neutral against the pre-fix baseline.

### 4.3 Cross-task YMM leak probe (NEW)

Equivalent of `mt-xmmprobe.c` (memo §15) but on YMM upper-128. Tiny
C reproducer:

```c
// repro: kvm-v2 cross-task YMM upper-128 isolation
//
// Pattern: 4 forks × 1000 iterations; each child writes a known
// pattern into YMM0..YMM7's upper-128 (via vinsertf128 from a
// per-pid magic value); does a syscall (forces vmexit and
// per-host-CPU vCPU yield); reads back the upper-128 (via
// vextractf128 to memory) and verifies against the magic value.
// Parent does the same with its own magic. Cross-task interleaving
// on the per-host-CPU vCPU pool means children dispatch on a vCPU
// that may have just run a different child — if YMM upper-128
// leaks across that boundary, the read-back differs from the magic.
//
// Pass criterion: 0 mismatches across N=24000 iterations × 6 boots.
//                 (Same statistical bar as threaded-fork-malloc.)

#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <immintrin.h>

#define ITER 1000
#define KIDS 4

static int probe(unsigned magic)
{
    __m256i v;
    int mismatches = 0;
    for (int i = 0; i < ITER; i++) {
        // build a 256-bit value; lower 128 = i, upper 128 = magic
        __m128i lo = _mm_set1_epi32(i);
        __m128i hi = _mm_set1_epi32(magic);
        v = _mm256_set_m128i(hi, lo);
        // force a syscall (vmexit + dispatch return)
        getpid();
        // read back upper-128
        __m128i ext;
        _mm_storeu_si128(&ext, _mm256_extracti128_si256(v, 1));
        if (_mm_extract_epi32(ext, 0) != (int)magic)
            mismatches++;
    }
    return mismatches;
}

int main(void)
{
    pid_t pids[KIDS];
    for (int k = 0; k < KIDS; k++) {
        pids[k] = fork();
        if (pids[k] == 0) {
            unsigned magic = 0xdeadbeef ^ (unsigned)getpid();
            int m = probe(magic);
            _exit(m == 0 ? 0 : 1);
        }
    }
    int fails = 0;
    for (int k = 0; k < KIDS; k++) {
        int s; waitpid(pids[k], &s, 0);
        if (!WIFEXITED(s) || WEXITSTATUS(s) != 0) fails++;
    }
    return fails;
}
```

Run as `init=` for 6 boots × N=4000 forks each = 24 000 forks total,
matching the SMP-T26/T27 statistical bar (Wilson 95% upper bound
0.015%). Pre-Phase-A: this probe is expected to fail (the SIGILL
fires before the probe gets to run). Post-Phase-A: must
demonstrate 0 mismatches OR Phase B is mandatory.

Build the test with `-mavx -O2`. Compiler will use VEX encodings for
the `_mm256_*` intrinsics. The `getpid()` syscall is the kvm-v2
gadget fast path (no real vmexit on the gadget shape) — augment to
`syscall(SYS_getppid)` (slow path, real vmexit) if the gadget short-
circuits the cross-task transition the test depends on.

### 4.4 Other regression guards (carry-forward)

- `tools/testing/selftests/um/soak/` Phase J pilot — once T57 passes,
  enable stress-ng on the IPC profile (memo §24 §8 closes this loop).
- `Documentation/virt/uml/redesign/STATUS.md` row T57 → DONE.

---

## 5. Risk register

The three highest-likelihood risks, ranked by combined
likelihood × impact:

### 5.1 (a) XSAVE area size change breaks marshal struct alignment

**Likelihood:** medium (Phase B only).
**Impact:** boot-time panic if KVM_CAP_XSAVE2 returns >4096 bytes
(e.g., AMX dynamically enabled on the host). Symptom: `KVM_SET_XSAVE`
returns -EINVAL, fpu_install_on_first_run panics ("kvm-v2: fpu install
(cpu=%d) failed: %d").
**Mitigation:** during Phase B init, check
`KVM_CHECK_EXTENSION(KVM_CAP_XSAVE2)` and refuse to initialise if it
exceeds `sizeof(struct kvm_xsave)`. Document the AMX-pinning
prerequisite in commit message; add a one-line `pr_warn` if
KVM_CAP_XSAVE2 reports > 4096 with explicit XCR0 mask check.

### 5.2 (b) Cross-task YMM upper-128 leak — same class as T26/T27 on bigger area

**Likelihood:** medium-high (Phase A with `kvm_fpu` marshal).
**Impact:** sporadic test failures in workloads that read YMM upper-
128 without first writing it. SMP-T26/T27 saw deterministic glibc
SIGSEGV at `_int_malloc+0xed` from the analogous lower-128 bug; the
upper-128 case is harder to trigger because well-behaved compilers
generate `vpxor %ymm,%ymm,%ymm` zero-idioms before any non-init use.
Worst case: the new YMM-probe in §4.3 surfaces failures, forcing
Phase B.
**Mitigation:** ship Phase A with the §4.3 probe enabled by default
in soak (24k forks × 6 boots); if it shows non-zero failures, escalate
to Phase B. The SMP-T55 dirty-epoch flag continues to gate the
ioctl; the migration to `KVM_GET_XSAVE` in Phase B is mechanical.

### 5.3 (c) Performance hit — XSAVE > FXSAVE per dispatch

**Likelihood:** high (Phase B).
**Impact:** ~3-5% per dispatch; visible on perf-py-startup ratio
and bench-micro getpid. Could push perf-py-startup ratio from 1.10
back over the 1.20 ceiling.
**Mitigation:** the SMP-T55 dirty-epoch (`fpu_dirty=false &&
fpu_owner_task==current`) skip-path applies unchanged — so syscall-
hot paths that don't touch FPU pay zero ioctl cost regardless of
FXSAVE-vs-XSAVE. The hit lands only on cross-task dispatches that
actually used FPU. The expected delta on perf-py-startup is well
under 5% because Python-startup is mostly CPython interpreter loop
(no AVX, mostly-syscall-bound).

If post-Phase-B benchmarks regress, fall back to a hybrid: keep
`kvm_fpu` for same-task dispatches (where YMM upper-128 is preserved
by KVM internally) and only `kvm_xsave` on cross-task arrivals
(the SMP-T33 / SMP-T55 cross-task flag exists already). One-line
gate in vcpu.c:2127.

### 5.4 Lesser risks

- (d) `KVM_SET_XCRS` order vs `KVM_SET_CPUID2`: KVM rejects XCR0
  values with bits set that aren't supported by guest CPUID. If we
  call XCRS before CPUID, the fresh KVM-default CPUID may not yet
  advertise YMM. Mitigation: install_xcrs is called from
  vcpu_create_one BEFORE the CPUID lazy-install at first KVM_RUN —
  but KVM-default CPUID at vcpu create time DOES advertise YMM
  (KVM_GET_SUPPORTED_CPUID is a superset). So ordering is safe.
  Sanity-check this on the gate machine via a probe at install_xcrs
  return.

- (e) `/proc/cpuinfo` already-leaks-host: orthogonal; not a
  regression. Filed separately in §2.6.

- (f) glibc's IFUNC resolution timing relative to KVM_SET_CPUID2: if
  ld-linux's CPU detection runs before our lazy CPUID install, it
  caches the WRONG (KVM-default = full-featured) feature set. With
  Phase A live, this is no longer wrong — we WANT AVX advertised.
  So Phase A also fixes the latent IFUNC-resolution-too-early bug.

---

## 6. Open questions

These were not resolvable from the source alone; they need
experiment, KVM-doc cross-check, or KVM-source dive.

1. **Does `KVM_GET_FPU` capture YMM upper-128 alongside XMM lower-128
   when CR4.OSXSAVE+XCR0.YMM are live?**
   The UAPI struct (`struct kvm_fpu` at
   `arch/x86/include/uapi/asm/kvm.h:175-187`) has only `xmm[16][16]`
   = 256 bytes for XMM0-XMM15, no provision for YMM upper. KVM source
   inspection (`arch/x86/kvm/x86.c` `kvm_arch_vcpu_ioctl_get_fpu`)
   would confirm whether the ioctl falls back to legacy FXSAVE
   regardless of XCR0, OR whether it transparently extends.
   Empirical resolution: implement Phase A, then add a probe that
   GETs fpu, modifies the upper-128, runs guest code that touches
   YMM, GETs fpu again, compares. Cheap.

2. **Is there a cleaner enable path than `KVM_SET_XCRS`?**
   Survey of KVM userspace clients (qemu, crosvm, kvmtool) shows all
   of them issue `KVM_SET_XCRS` exactly once at vcpu_create. There
   is no "auto-enable XSAVE from CPUID" path. So `KVM_SET_XCRS` is
   the canonical answer. The qemu source at
   `target/i386/kvm/kvm.c:kvm_arch_init_vcpu` does it before
   `KVM_SET_CPUID2`, which is the inverse of our planned order
   (we'd call it after install_production_sregs but the CPUID
   lazy-install runs at first KVM_RUN). Check that this ordering
   is acceptable; if not, hoist CPUID install to vcpu_create_one
   too (which obsoletes the buddy-allocator-not-up workaround at
   `vcpu.c:198-228` — the workaround is needed only if init runs
   before mm_init, but we're now in vcpu_create_one which runs
   later from `kvm_v2_init` → `kvm_v2_vcpu_create` → ... — verify).

3. **Does `KVM_CAP_XSAVE2`'s reported size on Zen 4 match
   `sizeof(struct kvm_xsave) = 4096`?**
   Zen 4 supports AVX-512 (we leave that masked) and AMX is
   AMD-uncommon, but if the host kernel has any `arch_prctl` AMX
   pinning live, KVM_CAP_XSAVE2 may return >4096. Probe at init
   time and panic with a clear message.

4. **Does `KVM_SET_XCRS` with XCR0.YMM=1 on a host that supports it
   require KVM_CAP_XSAVE_FEATURES to be checked first?**
   `KVM_CAP_XSAVE` is the umbrella cap; `KVM_CAP_XSAVE2` reports the
   size. There is no separate `KVM_CAP_XSAVE_FEATURES` advertising
   which XCR0 bits are settable. Empirical resolution: try the SET,
   observe the return.

5. **How does the SMP-T56 LSTAR-EINTR gadget body interact with
   AVX-enabled state?**
   The hand-written gadget at `lstar_gadget.S` (94 bytes per
   memo §SMP-T56) does not execute any AVX/SSE instructions. With
   CR4.OSXSAVE on, the gadget body should be unchanged in behaviour.
   But: if EINTR catches the gadget mid-flight at a point where the
   kernel host-side path uses AVX (e.g., glibc memcpy in the EINTR
   handler stack), KVM's internal save/restore must cycle correctly.
   This is a well-trod KVM path; expected to "just work" but worth
   a look during Phase A landing.

6. **Does enabling AVX expose new SIGFPE / #MF / #XF classes that
   v2's IDT vectoring doesn't yet handle?**
   The v2 IDT covers vectors 0/1/3/6/7/13/14 (per memo `26` Phase E).
   #XF (vector 19, SIMD floating-point exception) is **NOT** in
   that list. If a guest issues an AVX op that triggers #XF (e.g.,
   denormal handling with MXCSR.MM=0), exception delivery would land
   at an unmapped IDT entry → triple fault. Mitigation: add IDT[19]
   in a follow-up; for SMP-T57's deterministic stress-ng path, #XF
   is unreachable (mscan does integer ops).

---

## 7. References

### 7.1 Memo cross-refs

- `state-audit/22-smp-t41-stress-and-perf.md` — perf debt context.
- `state-audit/23-smp-t55-perf-regression-plan.md` — per-vCPU
  FPU-dirty epoch; landed at `fd2f9639b0ce`.
- `state-audit/24-smp-t57-vmmethod-bisect.md` — root cause; §7.6-H is
  the byte-match probe that nailed the AVX-disabled hypothesis.
- `state-audit/15-smp-t26-t27-fpu-cross-task-leak-FIXED.md` — the
  cross-task FPU-leak class; YMM-upper-128 in Phase A is in the same
  class.
- `state-audit/14-smp-t26-glibc-heap-corruption-CHARACTERIZED.md` —
  the original `_int_malloc` MOVUPS reproducer pattern.
- `STATUS.md` row T57 (line 122) — narrows P=0.85 to XCR0/CR4.OSXSAVE
  / xstate_bv.

### 7.2 Commit hashes

- `fd2f9639b0ce` — SMP-T55 per-vCPU FPU-dirty epoch (memo 23
  option a).
- `76b1d98b2006` — SMP-T26/T27 always-`KVM_GET_FPU` revert of H.2
  lazy-FPU.
- `467aa7d142c0` — SMP-T54 worker socketpair fd leak fix.
- `db9170b5a7b3` — SMP-T56 LSTAR-EINTR carve-out for gadget body.
- `602e9a27625c` — SMP-T47 WARN_ON_ONCE cross-task SET_SREGS.
- `d28a74cae98d` — current HEAD (STATUS row T57 = root-caused).
- `b91d49857892` — memo `24` §7.6-H probe (the byte-match evidence).

### 7.3 KVM API references

- `Documentation/virt/kvm/api.rst`:
  - §4.42 `KVM_GET_XSAVE` (line 1660-1677)
  - §4.43 `KVM_SET_XSAVE` (line 1680-1705)
  - §4.44 `KVM_GET_XCRS` (line 1708-1732)
  - §4.45 `KVM_SET_XCRS` (line 1735-1759)
  - §4.134 `KVM_GET_XSAVE2` (line 6106-6126)
- UAPI:
  - `include/uapi/linux/kvm.h:1366-1367` (`KVM_GET_FPU`/`KVM_SET_FPU`)
  - `include/uapi/linux/kvm.h:1408-1412` (XSAVE/XCRS)
  - `arch/x86/include/uapi/asm/kvm.h:175-187` (`struct kvm_fpu`)
  - `arch/x86/include/uapi/asm/kvm.h:386-417` (`struct kvm_xsave`,
    `struct kvm_xcrs`).

### 7.4 Intel SDM references

- Vol. 1 §13 — XSAVE feature set (XCR0 layout, XSAVE area layout,
  init / modified state, save-and-restore semantics).
- Vol. 1 §13.4 — XSAVE area layout (legacy region 0..511, header
  512..575, extended region 576+).
- Vol. 1 §13.5 — XSAVE/XRSTOR semantics (which components save based
  on `xstate_bv` AND XCR0).
- Vol. 3 §2.6 — CR4.OSXSAVE bit (must be set to allow CPL=3 access
  to XGETBV; XCR0 is the architectural enable).
- Vol. 2A §VPXOR (VEX.128/256 encoding; raises #UD if
  CR4.OSXSAVE=0 OR XCR0.YMM=0; this is exactly the SMP-T57 fault).

### 7.5 v1 archive cross-refs

- `arch/um/backend/kvm-v1-archive/lifecycle.c:411-547` — the v1
  curated CPUID mask we lifted verbatim. v1 also did not enable
  XSAVE/AVX (per `vcpu.c:24-33` header comment) — Phase A is the
  first divergence from v1 on this surface.

---

## 8. Recommendation

Land Phase A (CR4.OSXSAVE + KVM_SET_XCRS + Leaf 1 ECX 26/27/28 +
Leaf 7.0 EBX 5 + leaf 0xD pass-through). Verify §4.2 + §4.3 gate.
Phase B is contingent on §4.3 surfacing a YMM upper-128 leak —
not on a schedule. The Phase A landing is small (~5 site changes,
all in `vcpu.c` and the new XCRS helper), reverse-bisectable, and
unblocks every Phase J Tier-1 stress-ng workload that uses AVX
intrinsics through libc/libstdc++.

---

*Author: Claude (planning agent), 2026-05-07. No code changes
proposed; this memo is plan-only. Audit cross-checked file:line
against HEAD `d28a74cae98d` for every cited site. The most
uncertain link in the chain is §6 open question 1 — whether
`KVM_GET_FPU` captures YMM upper-128 transparently when XCR0.YMM
is live, or whether it strictly returns the FXSAVE subset. The
answer determines whether Phase A is sufficient or whether
Phase B is mandatory.*
