# UML kvm-v2 — Feature-Enablement Checklist (SMP-T77)

**Date:** 2026-05-19
**Status:** Reference. Read before un-masking any CPUID bit in
`arch/um/backend/kvm-v2/vcpu.c::kvm_v2_curate_cpuid`.

## Why this exists

Round 14 closed a 14-round CPython cache-flake bug (SMP-T73) whose
root cause was the legacy 512-byte `KVM_GET_FPU` / `KVM_SET_FPU`
per-dispatch save/restore failing to preserve YMM upper 128 bits
after **SMP-T57 Phase A** un-masked AVX in CPUID. SMP-T26/T27 fixed
the XMM-low-128 leak class for the pre-AVX scope; SMP-T57 enabled
AVX without a re-audit. The two commits compounded into a 1-2%
silent corruption rate that took five weeks to pin down.

The pattern: enabling a new CPU feature requires walking every
save/restore site (FPU + DEBUGREGS + VCPU_EVENTS + XCRS + …) to
confirm the per-task state machine covers the new component. This
file is the checklist that turns that walk into a recipe.

## How to use

Before merging any commit that un-masks a CPUID feature bit in
`kvm_v2_curate_cpuid` (vcpu.c lines ~187 / ~207 / ~221 / ~229),
work through the matching row. Each row lists (a) which other
files must change in lock-step, (b) which existing tests must
extend, and (c) the one specific test that must pass before
merge. If any item is "unknown" — stop and audit first.

---

## AVX-512 family (XCR0 bits 5 / 6 / 7 = OPMASK / ZMM_Hi256 / Hi16_ZMM)

**CPUID bits to un-mask:**
- Leaf 1 ECX bit 26 (XSAVE) — already un-masked since T57 Phase A.
- Leaf 7.0 EBX bits 16/17/21/26-31 (AVX512F/DQ/IFMA/PF/ER/CD/BW/VL).
- Leaf 7.0 ECX bits 1/6/11/12/14 (AVX512VBMI/VBMI2/VNNI/BITALG/VPOPCNTDQ).
- Leaf 7.0 EDX bits 2/3/8 (AVX512_4VNNIW/4FMAPS/VP2INTERSECT).

**Required edits:**
- `kvm_v2_curate_cpuid` — drop the bits from each mask.
- Bump XCR0 in `kvm_v2_install_xcrs` from `0x7` (FP|SSE|YMM) to
  `0xE7` (+ OPMASK | ZMM_Hi256 | Hi16_ZMM).
- T76: also un-mask leaf 0xD sub-leaves 5/6/7 in
  `kvm_v2_curate_cpuid` so the state-component descriptors are
  self-consistent with the un-masked feature bits.
- `arch/um/backend/kvm-v2/state_trace.c` — adjust the FNV-1a hash
  scope on `iotrap_fpu` if you want the cross-task ZMM upper to
  show up in the trace ring's hash column.

**Required tests:**
- An mt-zmm-probe analogue of `mt-xmmprobe.c` that splats ZMM0 with
  a pattern, takes a vmexit (signal + KVM_RUN cycle), and verifies
  the splat survives. Cross-task: run two threads on the same
  vCPU and confirm thread A's ZMM is not visible to thread B.
- Existing `cpython-tier1-pylibs` + `tier3-django` soaks: 0 cache
  aborts in n >= 240 (verified to NOT flake under the new state
  size).

**Memory cost:** Still fits in 4 KB `struct kvm_xsave` (full
AVX-512 XSAVE area ≈ 2.7 KB). No `KVM_GET_XSAVE2` switch needed.

---

## AMX (XCR0 bits 17 / 18 = XTILECFG / XTILEDATA)

**CPUID bits to un-mask:**
- Leaf 7.0 EDX bit 22 (AMX-BF16) — currently not in our mask
  because v1's mask didn't list it; check before un-masking.
- Leaf 7.0 EDX bit 24 (AMX-TILE).
- Leaf 7.0 EDX bit 25 (AMX-INT8).

**Required edits — MANDATORY before any guest AMX instruction:**
- Per-task `iotrap_fpu` storage in `arch_thread.kvm_v2` must
  migrate from fixed-size `struct kvm_xsave` (4 KB) to a
  dynamically-allocated buffer whose size is queried at boot via
  `KVM_CHECK_EXTENSION(KVM_CAP_XSAVE2)` (typically ~11 KB with
  AMX-TILEDATA).
- All 5 callsites that today use `KVM_GET_XSAVE` / `KVM_SET_XSAVE`
  must switch the GET side to `KVM_GET_XSAVE2` (the SET side
  already reads `uabi_size` bytes — see x86.c:6447 — and handles
  the larger buffer automatically). Sites are:
  - `arch/um/backend/kvm-v2/vcpu.c` lines ~2415 (SET install),
    ~2576 (GET capture), ~3025 (fork GET), ~3173 (switch-out GET),
    ~3270 (install_on_first_run SET).
  - `arch/um/backend/kvm-v2/snapshot.c` already uses XSAVE; verify
    it also handles XSAVE2 if KVM_CAP_XSAVE2 is available.
- Process-level `arch_prctl(ARCH_REQ_XCOMP_GUEST_PERM,
  XFEATURE_XTILEDATA)` must be called BEFORE the first vCPU is
  created (per AMX patch series prereq). Wire this into
  `kvm_v2_init_backend` or equivalent before `kvm_v2_vcpu_create`.
- XFD (eXtended Feature Disable) MSR programming — handled by KVM
  via `kvm_set_msr_common`, but verify the curated MSR list
  doesn't accidentally reject it.
- T76: un-mask leaf 0xD sub-leaves 17/18 in `kvm_v2_curate_cpuid`.

**Required tests:**
- Cross-task AMX leak test: thread A loads a tile via `TILELOAD`,
  takes a vmexit; thread B runs `TILEZERO`; thread A re-enters and
  verifies the original tile content. Failure = leak via the
  XSAVE pipe.
- Boot-time KUnit that asserts `KVM_GET_XSAVE2` succeeds with the
  expected buffer size and reads `xfeatures` field includes bits
  17/18.

**Memory cost:** ~11 KB per task_struct (vs T73's 4 KB). Material.
Consider a per-task dynamic allocation gated on the AMX-enabled
static branch.

---

## PKRU (XCR0 bit 9 = Memory Protection Keys)

**CPUID bit to un-mask:**
- Leaf 7.0 ECX bit 3 (PKU = Memory Protection Keys for user pages).

**Required edits:**
- `kvm_v2_curate_cpuid`: drop bit 3 from the leaf 7.0 ECX mask.
- `kvm_v2_install_xcrs`: bump XCR0 to include bit 9 (value 0x207
  if MPX kept off; 0xE7 + 0x200 = 0x2E7 with AVX-512).
- `kvm_v2_install_production_sregs` (or its CR4 setup site): set
  CR4.PKE so the PKRU register is meaningful.
- No new per-task storage — PKRU flows through the existing
  `KVM_GET_XSAVE` / `KVM_SET_XSAVE` pipe T73 already plumbed.

**Required tests:**
- A guest userspace test that uses `pkey_alloc` + `pkey_mprotect`
  + writes to a write-disabled-by-PKRU page (must SIGSEGV with
  `si_code = SEGV_PKUERR`).
- Cross-task PKRU leak test: thread A sets PKRU to deny-all,
  thread B inherits — verify thread B does NOT see thread A's
  PKRU.

**Memory cost:** 4 bytes added to XSAVE area. Trivially fits.

---

## CET (XCR0 bits 11 / 12 = CET_U / CET_S)

**CPUID bits to un-mask:**
- Leaf 7.0 ECX bit 7 (CET_SS = Shadow Stack).
- Leaf 7.0 EDX bit 20 (CET_IBT = Indirect Branch Tracking).

**Required edits:**
- `kvm_v2_curate_cpuid`: drop bit 7 from leaf 7.0 ECX mask, bit 20
  from leaf 7.0 EDX mask.
- `kvm_v2_install_xcrs`: bump XCR0 to include bits 11+12.
- CR4.CET arming (separate sregs edit).
- Per-task `IA32_PL3_SSP` MSR is in the CET_U XSAVE area;
  KVM_GET_XSAVE / KVM_SET_XSAVE will preserve it via T73's pipe.

**WARNING — same architectural shape as the YMM-upper leak:** the
shadow-stack pointer (`IA32_PL3_SSP`) is per-task. If CET is ever
enabled WITHOUT a per-task SHSTK leak test, you may ship a
critical security regression where thread A's shadow stack
pointer leaks to thread B and lets thread B forge return
addresses past CFI checks.

**Required tests:**
- Per-task SHSTK probe: mirror of `mt-xmmprobe.c` for the SSP MSR.
  Thread A runs `WRSSQ` to splat a pattern into its shadow stack,
  takes a vmexit, thread B enters via the same vCPU and reads its
  own SSP — must NOT see thread A's SSP value.
- IBT exception test: jump indirectly to a non-ENDBR target,
  verify the guest takes a `#CP` fault and routes through the
  configured handler.

**Memory cost:** ~24 bytes added. Trivially fits.

---

## AVX-VNNI (CPUID 7.1 EAX bit 4)

Uses the existing YMM XSAVE state (no new XCR0 bit). Already
covered by T73's `KVM_GET_XSAVE` pipe. **No code change beyond
un-masking the feature bit.**

**Required test:** an `mt-avxvnni-probe` that runs `VPDPBUSDS ymm,
ymm, ymm` cross-task and verifies no leak.

---

## AVX10 / APX

AVX10 reuses AVX-512 state components — when AVX-512 is un-masked
(see above), AVX10 is automatically along for the ride.

APX (Advanced Performance Extensions) adds R16-R31 GPRs and a new
EVEX-promoted ISA. The GPRs are tracked by KVM through the
existing `KVM_SYNC_X86_REGS` mmap mechanism but **only if KVM is
updated to advertise the larger register file**; current KVM
upstream (6.10+) handles APX via XSAVE state bit 19. Treat APX
the same as AMX in the checklist above (state size grows, need
XSAVE2 if total > 4 KB).

---

## Things NOT to un-mask

- **RDRAND** (leaf 1 ECX bit 30) and **RDSEED** (leaf 7.0 EBX bit
  18) — record-replay determinism breaks; keep masked.
- **FSGSBASE** (leaf 7.0 EBX bit 0) — separate workstream;
  CR4.FSGSBASE not yet enabled.
- **MPX** (XCR0 bits 3/4) — deprecated; KVM upstream masks them
  out of `supported_xcr0` anyway.
- **Intel PT** (XCR0 bit 8) — tracing feature, not relevant for
  UML guest.
- **PASID** (XCR0 bit 10) — ENQCMD/ENQCMDS; not in scope for UML.

---

## Latent-bug checklist (independent of XSAVE scope)

Two cross-task state-machine sites were closed alongside SMP-T73:

- **DEBUGREGS** (SMP-T74): pinned to zero at vCPU create. Re-audit
  if a future change ever needs HW breakpoints in guest.
- **VCPU_EVENTS** (SMP-T75): full per-task save/restore mirroring
  iotrap_fpu. Adds ~184 B per task. Required for any feature that
  uses #DB / #MC NMI / SVM intercept-inject — all of which can
  leave events pending across a vmexit.

If you add a NEW cross-task state-machine component (e.g.,
per-vCPU MSR state, segment descriptors, etc.), file a parallel
"iotrap_*" pattern in `arch/x86/um/asm/processor_64.h` and
mirror the GET-after-exit / SET-before-entry pair in `vcpu.c`.
The architectural shape is identical.

---

## References

- Round 14 root-cause memo:
  `Documentation/virt/uml/redesign/08-future-phases/
  50-kvm-v2-django-flake-investigation-summary.md`
- HW audit punch-list: same memo, `## Round 14 ...` section.
- SMP-T26/T27: Layer 15 memo (FPU cross-task leak).
- SMP-T57 Phase A: Layer 25 (XSAVE/AVX un-mask plan).
- SMP-T73: this commit chain (5 callsites GET/SET_FPU → XSAVE).
