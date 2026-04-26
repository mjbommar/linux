# 17 — Per-task vCPU state isolation under integrated KVM

Date: 2026-04-26
Author: Investigation grounded in the post-keystone state at
`uml-redesign-plan` (commit `901213a8d2d1`) and the architecture
review at `16-architecture-review/`.
Audience: future contributors closing the residual 4-5 module
divergence on the cpython-parity gate that keystone moved from
0/21 → 17/21.

This memo is the per-task-vCPU-state companion to memo 16's
`02-vcpu-state.md`. Memo 16 catalogued state that leaks **into**
the guest at the start of execution (host TLS, exec_regs,
FMASK=0). This memo catalogues state that drifts **across** UML
tasks that time-multiplex the single vCPU.

The keystone fix landed; same-CR3 same-mm same-task workloads now
pass. The 5 modules that still diverge (`test_struct.test_endian_
table_init_subinterpreters`, `test_decimal` doctest 731, `test_
bytes.FreeThreadingTest.test_free_threading_bytearrayiter`,
`test_list.test_repr_deep`, `test_set.TestWeirdBugs.test_merge_
and_mutate`) all spawn pthreads, subinterpreters or do deep
recursion on user pointers — i.e. workloads that distinguish
"task A's vCPU state" from "task B's vCPU state" and notice
when they leak into each other.

## Executive summary

The single vCPU0 holds **substantial guest-architectural state
that SHOULD be per-UML-task but isn't**. The current implementation
saves/restores only:

1. **FPU legacy 512 B region** (XMM0..XMM15, x87 FP, MXCSR) —
   via per-task hash table, kvm_fpu_save/restore_for_task,
   `arch/um/backend/kvm/thread.c:132-177` + `292-295`.
2. **GPRs** (RAX..R15, RIP, RSP, RFLAGS) — via the
   `kvm_uml_regs_to_kvm_regs` round-trip, sourced from the per-task
   `current->thread.regs.regs.gp[]`. Re-installed on every
   `kvm_enter_guest` from the marshalled gp[] and re-extracted
   via `kvm_regs_to_uml_regs` after VMEXIT.
3. **CR3** — set per-mm via `kvm_shadow_mm_current()`'s `pgd_gpa`,
   keystone'd by the XOR-bit-12 sentinel toggle that flushes TLB
   when the value happens to match.
4. **MSR_FS_BASE / MSR_GS_BASE** — re-pushed from
   `regs->gp[HOST_FS_BASE/HOST_GS_BASE]` on every entry where the
   sregs cache misses.

What is **NOT** saved/restored across UML task switches but **is**
mutable per-thread on real x86:

- AVX / AVX-512 / opmask / MPX bounds (XSAVE state beyond legacy
  FXSAVE)
- DR0..DR7 (debug registers, hw breakpoints, DR6 status)
- MSR_KERNEL_GS_BASE (per-task on real Linux, programmed once
  per-vCPU here to a static gadget page — but gadget reads pid/uid
  from this base and pid IS per-task)
- MSR_TSC_AUX (carries CPU id for `RDTSCP`; per-CPU on real
  Linux but per-vCPU here)
- Pending exception/interrupt state (`KVM_GET/SET_VCPU_EVENTS`)
- Segment selectors / descriptors beyond the static long-mode
  template
- Reserved/padding selectors that long-mode ignores anyway

For a single-threaded process, the task's GPRs/CR3/FS_BASE
trio is sufficient — the rest stays at "whatever the last entry
left there" and that's the same task's state. **For a process
that spawns pthreads (i.e. clone(CLONE_VM | CLONE_THREAD)), each
thread is a distinct UML task with its own gp[] and its own
FS_BASE, but they share CR3 (same mm) and they share AVX/DR/
KERNEL_GS_BASE state on the vCPU**. When task A writes XMM12
in the middle of an AVX2 memcpy, then SIGALRM preempts to task
B which does its own AVX2 memcpy, B reads A's XMM12 as its
source and writes garbage. The single vCPU has no notion of
"thread B is running now, switch the AVX state."

This memo identifies every gap, maps each to the suspected
failing test, recommends an architectural fix, and proposes a
proof-of-concept C reproducer.

## Section 1 — concrete vCPU state inventory

Audit was performed by walking every KVM_SET_* / KVM_GET_* call
site in `arch/um/backend/kvm/` and matching against the x86 vCPU
state that KVM exposes via UAPI (`include/uapi/linux/kvm.h`,
`arch/x86/include/uapi/asm/kvm.h`). Table is exhaustive for the
ioctls UML calls; "never-touched" entries are state KVM holds
but we've never written or read in the integrated backend.

Legend for "Scope":
- **per-task** — varies per UML task (kernel thread, user
  process, pthread within a process)
- **per-mm** — shared across all UML tasks attached to a given
  `mm_struct`; drifts when CR3 swaps
- **per-vCPU** — shared by every task running on that vCPU
  (i.e. all UML tasks under ncpus=1)
- **never set** — KVM keeps the default; we never write or
  re-read

### 1a. General-purpose registers and instruction pointer

| Field | Owning ioctl | Set where (file:line) | Restored where | Per-task? | Notes |
|-------|--------------|----------------------|----------------|-----------|-------|
| RAX..R15 | `KVM_SET_REGS` / `KVM_SYNC_X86_REGS` | `kvm_enter_guest` thread.c:2409 (marshal from `regs->gp[*]`) | `kvm_run_userspace` thread.c:3193 (`kvm_regs_to_uml_regs(regs, &kregs)`) | YES (correct) — `regs->gp[]` lives on `current->thread.regs.regs` |  |
| RIP | same | thread.c:2419 (set to bootstrap IRETQ gadget; user RIP comes from frame[0]) | thread.c:3193 | YES (correct) |  |
| RSP | same | thread.c:2420 (IST stack page); user RSP from frame[3] | thread.c:3193 | YES (correct) |  |
| RFLAGS | same | thread.c:2421 (kernel RFLAGS=0x2); user RFLAGS via R11 from `kvm_build_sysret_r11` | thread.c:3193 | YES (correct) |  |

GPRs are correct. They round-trip through `current->thread.regs.regs.gp[]` exactly the same way ptrace and seccomp do.

### 1b. Segment registers

| Field | Owning ioctl | Set where | Per-task? | Notes |
|-------|--------------|-----------|-----------|-------|
| CS, DS, ES, FS, GS, SS (selectors + descriptor caches) | `KVM_SET_SREGS` | `kvm_enter_guest` → `kvm_setup_production_sregs` → `kvm_fill_longmode_segments` (sregs.c:219-251). All 6 selectors loaded with the long-mode kernel template (CS=0x08, DS/ES/FS/GS/SS=0x10). | per-vCPU template | Long-mode treats CS only via L=1 bit; DS/ES/SS effectively flat-base 0 with selector=0x10 ignored for memory access. FS/GS use their explicit base registers. |
| `sregs.fs.base` (= MSR_FS_BASE) | `KVM_SET_SREGS` (cached) or `KVM_SET_MSRS` (post-arch_prctl) | thread.c:2306 from `regs->gp[HOST_FS_BASE]` | YES (correct) — gp[HOST_FS_BASE] is per-task; arch_prctl writes it (syscalls_64.c:23); arch_set_tls writes it on clone(CLONE_SETTLS) (tls_64.c:15) |  |
| `sregs.gs.base` (= MSR_GS_BASE) | same | thread.c:2307 from `regs->gp[HOST_GS_BASE]` | YES (correct) |  |
| `sregs.tr` (TSS) | `KVM_SET_SREGS` | thread.c:2283 (busy-TSS at GDT[6]) | per-vCPU |  |
| `sregs.gdt.base/limit` | `KVM_SET_SREGS` | thread.c:2268 → `kvm_setup_production_sregs` (sregs.c:301) | per-vCPU (same bootstrap page for every entry) |  |
| `sregs.idt.base/limit` | `KVM_SET_SREGS` | thread.c:2281 | per-vCPU |  |
| `sregs.ldt` | never written | — | left at KVM default (zeroed) |  |

Segments are largely fine for long-mode user space — DS/ES/SS are flat,
and FS/GS use explicit bases that are per-task. The TR/IDT/GDT bootstrap
template is per-vCPU but the same template every entry, so no leak.

### 1c. Control registers and EFER

| Field | Owning ioctl | Set where | Per-task? | Notes |
|-------|--------------|-----------|-----------|-------|
| CR0 | `KVM_SET_SREGS` | thread.c:2268 → sregs.c:262 (PE\|MP\|NE\|WP\|PG) | per-vCPU | Static template. |
| CR3 | `KVM_SET_SREGS` | thread.c:2352 (= `shadow->pgd_gpa` from per-mm shadow) | YES (per-mm) | Keystone fix forces CR3 toggle even when value matches; same-mm task switches keep the same CR3 (correct since the mm IS shared). |
| CR4 | `KVM_SET_SREGS` | sregs.c:261 (PAE\|OSFXSR\|OSXMMEXCPT) | per-vCPU | **Static — does NOT include OSXSAVE.** Without OSXSAVE, `XSETBV` would #GP and CPUID-AVX advertises features the OS hasn't actually enabled. See gap G-XSAVE below. |
| CR2 | KVM internal (CPU writes on #PF) | thread.c reads via KVM_GET_SREGS / KVM_SYNC_X86_SREGS | per-vCPU latch | Read-only from our perspective. Whoever faulted last "wins" CR2; if task A faults at addr X, scheduler runs B, B faults at addr Y, then we look at sregs.cr2 for B's fault — fine. |
| CR8 (TPR) | never written | — | per-vCPU default 0 | No APIC. |
| EFER | `KVM_SET_SREGS` | sregs.c:264 (SCE\|LME\|LMA\|NXE) | per-vCPU | Static. |
| XCR0 | never written | — | per-vCPU default | **Likely 0x1 (legacy x87 only) since OSXSAVE=0 in CR4.** AVX256/512 state space therefore not architectural — see gap G-XSAVE. |

### 1d. MSRs touched

| MSR | Numeric | Owning ioctl | Set where | Per-task? | Notes |
|-----|---------|--------------|-----------|-----------|-------|
| MSR_STAR | 0xC0000081 | `KVM_SET_MSRS` (one-shot) | thread.c:1709, primed once via `msrs_primed` (kvm_backend.h:48) | per-vCPU constant | Correct — depends only on GDT layout. |
| MSR_LSTAR | 0xC0000082 | one-shot | thread.c:1714 (= bootstrap_va + 0x40) | per-vCPU constant | Correct. |
| MSR_FMASK | 0xC0000084 | one-shot | thread.c:1744 (= 0x47700 post-memo-16) | per-vCPU constant | Correct — masks TF/IF/DF/IOPL/NT/AC on SYSCALL entry. |
| MSR_FS_BASE | 0xC0000100 | `KVM_SET_SREGS` (cached) and post-arch_prctl `kvm_propagate_fs_gs_base` (`KVM_SET_MSRS`) | thread.c:2306, 2970 | YES (correct) | Round-trips through gp[HOST_FS_BASE]. |
| MSR_GS_BASE | 0xC0000101 | same | thread.c:2307, 2971 | YES (correct) |  |
| MSR_KERNEL_GS_BASE | 0xC0000102 | `KVM_SET_MSRS` every entry (P0-3 fix dropped one-shot) | `kvm_enter_guest_program_kernel_gs_base` thread.c:1685, called from kvm_enter_guest:2493 | per-vCPU constant (= `gadget_state_va`) | **GAP G-KGS:** programmed every entry to point at the gadget state page (one page per UML system, allocated once). The gadget state page itself is **rewritten per entry** from `current` via `kvm_gadget_state_refresh`. So MSR_KERNEL_GS_BASE is correct; the **page contents** are per-task. But there's a subtle issue: the refresh and the entry are atomic w.r.t. that vCPU, so no torn read possible while vCPU is running. The gap is that the page is shared across tasks; any failure to refresh between A→B switch leaks A's pid into B's gadget read. |
| MSR_EFER | 0xC0000080 | via SREGS | sregs.c:264 | per-vCPU constant | Correct. |
| MSR_TSC_AUX | 0xC0000103 | **NEVER WRITTEN** | — | per-vCPU default | Returned by `RDTSCP`. Glibc's `clock_gettime` on `__clock_getcpu`-aware glibc uses RDTSCP to fetch CPU id. Under ncpus=1 this is harmless — there's only one CPU id to return — but a guest task that probes RDTSCP will see whatever KVM defaulted (often 0). |
| MSR_IA32_DEBUGCTL | 0x000001D9 | **NEVER WRITTEN** | — | per-vCPU default | Branch-trace, BTF, etc. Real Linux sets this per-task. UML doesn't expose ptrace single-step under KVM, so probably moot. |
| MSR_IA32_SYSENTER_* | 0x174-0x176 | **NEVER WRITTEN** | — | unused | SYSENTER not used; we use SYSCALL. |
| MSR_IA32_PAT | 0x00000277 | **NEVER WRITTEN** | — | per-vCPU default | Memory-type attribute table. |
| MSR_TSC | 0x00000010 | **NEVER WRITTEN** | — | per-vCPU monotone | KVM-managed. RDTSC reads it; consistent across tasks (per real-CPU behavior under ncpus=1). |

### 1e. FPU / XSAVE state

| Region | Bytes | Saved? | Where | Per-task? | Notes |
|--------|-------|--------|-------|-----------|-------|
| Legacy FXSAVE area: x87 FP regs (8×16 = 128 B), FCW/FSW/FTWX (8 B), last_op/last_ip/last_dp (24 B), XMM0..XMM15 (16×16 = 256 B), MXCSR (4 B) | 512 B (`struct kvm_fpu`, kvm.h:175) | YES via `KVM_GET/SET_FPU` | per-task hash `kvm_fpu_hash[]` thread.c:97-178; called from `kvm_context_switch` thread.c:292-295 | YES (per-task, but with bugs noted below) | This covers SSE memcpy state (XMM-based glibc memcpy emits movdqa/movdqu over XMM regs). |
| YMM hi-128 of YMM0..YMM15 (16×16 = 256 B) | AVX state | **NO** | — | shared across all tasks on vCPU | **GAP G-AVX:** AVX2-capable glibc memcpy uses `vmovdqu %ymm` (256-bit moves). The lower 128 bits live in XMM (saved); the upper 128 bits live in the YMM-state region of XSAVE — never accessed by the legacy FXSAVE region kvm_fpu uses. Cross-task leak. |
| ZMM hi-256 of ZMM0..ZMM15 + ZMM16..ZMM31 + opmask K0..K7 + Hi16_ZMM | AVX-512 state | **NO** | — | shared | **GAP G-AVX512:** likely moot today (most distributions don't ship AVX-512 glibc routines), but lurking landmine. |
| MPX bounds (BND0..BND3 + BNDCFG/BNDSTATUS) | MPX state | **NO** | — | shared | Deprecated; CPUID likely says no MPX, harmless. |
| PKRU | PKU state | **NO** | — | shared | Glibc doesn't program PKRU by default; potential issue if guest libc uses memory-protection keys. |
| XSAVE header (XSTATE_BV, XCOMP_BV) | 64 B | **NO** | — | shared | Tracks which state components are present; not per-task in our model. |

`KVM_GET_FPU` / `KVM_SET_FPU` (the ones we use) are **explicitly the legacy fxsave layout** per the UAPI struct definition (`kvm_fpu` in arch/x86/include/uapi/asm/kvm.h:175). They do NOT cover any state in the XSAVE area beyond the 512-byte legacy region. KVM provides `KVM_GET_XSAVE` / `KVM_SET_XSAVE` (kvm.h:1408-1409) for the full XSAVE area but UML's KVM backend never calls them.

This is the most concrete inventory of the gap: **AVX register state across UML task switches is shared on the vCPU**.

### 1f. Debug registers

| Field | Owning ioctl | Set where | Per-task? | Notes |
|-------|--------------|-----------|-----------|-------|
| DR0..DR3 (breakpoint addresses) | `KVM_SET_DEBUGREGS` | **NEVER WRITTEN** | per-vCPU default 0 | **GAP G-DR:** ptrace can request hardware breakpoints; under KVM the per-task DR state isn't maintained. UML's ptrace already mostly software-emulates ptrace, so probably no failing test depends on DR0..3 today. |
| DR6 (status, captures last #DB cause) | same | **NEVER WRITTEN** | per-vCPU default | If task A ever triggered #DB (via kernel-side debug), DR6 carries that across to task B. Probably moot — no #DB delivery path in UML KVM. |
| DR7 (control / breakpoint enables) | same | **NEVER WRITTEN** | per-vCPU default 0x400 (Intel reset value) | Same as above. |

Per-task DR state is genuinely missing, but `KVM_SET_DEBUGREGS` is so cheap that the cost of doing it on every context switch is negligible. The reason it isn't done is "no test demands it" — until someone runs gdb under UML+KVM and a hw breakpoint silently misses.

### 1g. Pending interrupt / exception state

| Field | Owning ioctl | Set where | Per-task? | Notes |
|-------|--------------|-----------|-----------|-------|
| `kvm_vcpu_events.exception.{injected,nr,has_error_code,error_code,pending,exception_payload}` | `KVM_GET_VCPU_EVENTS` / `KVM_SET_VCPU_EVENTS` | **NEVER WRITTEN; READ only on fatal-fault diagnostic** thread.c:4148-4162 | per-vCPU latch | **GAP G-EVENTS:** if task A is mid-execution and KVM has a pending injected #PF queued (because the previous KVM_RUN was about to deliver one when KVM_EXIT_INTR fired due to host signal), and we then switch to task B via kvm_context_switch and re-enter, KVM will inject task A's #PF into task B at first opportunity. **This is a plausible cause of the flaky 8/10 reliability on `import unittest`** — a SIGALRM that fires while a #PF is being delivered preserves a stale exception across task switches. |
| `kvm_vcpu_events.interrupt.*` | same | never | per-vCPU latch | No APIC; usually empty. |
| `kvm_vcpu_events.nmi.*` | same | never | per-vCPU | Not delivered. |
| `kvm_vcpu_events.smi.*` | same | never | per-vCPU | Not relevant in long-mode userspace. |
| `kvm_vcpu_events.triple_fault.pending` | same | never | per-vCPU | Would indicate hard cascade. |

The vcpu_events area carrying pending injected exceptions across task switches is a previously-uncatalogued gap that ALSO matches the symptom profile of "looks like memory corruption but actually a stale fault delivered on the wrong task."

### 1h. APIC / LAPIC state

| Field | ioctl | Notes |
|-------|-------|-------|
| `KVM_GET_LAPIC` / `KVM_SET_LAPIC` | never | UML KVM backend has no APIC; KVM creates one by default but we don't enable INIT. Per-vCPU but moot. |
| `KVM_GET_MP_STATE` / `KVM_SET_MP_STATE` | never | Multi-processor state; ncpus=1 keeps default. |

### 1i. Summary table — per-task gap census

| State component | Per-task on real x86 | Saved/restored under UML KVM today? |
|-----------------|---------------------|-------------------------------------|
| GPR (RAX..R15, RIP, RSP, RFLAGS) | yes | YES (gp[] round-trip) |
| FS_BASE | yes | YES |
| GS_BASE | yes | YES |
| KERNEL_GS_BASE | yes (per-thread on Linux) | NO — pinned to gadget state page (page IS refreshed per entry from current, so equivalent for the gadget's purposes) |
| CR3 | per-mm | YES (per-mm) |
| Legacy FXSAVE (XMM0..15, x87, MXCSR, FCW/FSW) | yes | YES (per-task hash) |
| YMM0..15 hi-128 (AVX) | yes | **NO — gap G-AVX** |
| ZMM hi-256, ZMM16..31, opmask K0..7 (AVX-512) | yes | **NO — gap G-AVX512** |
| PKRU (mem-protection keys) | yes | **NO — gap G-PKRU** |
| DR0..7 (debug regs) | yes | **NO — gap G-DR** |
| MSR_TSC_AUX | per-CPU (logical id) | **NO — gap G-TSC_AUX** (moot under ncpus=1) |
| MSR_IA32_DEBUGCTL | yes | **NO — gap G-DEBUGCTL** (likely moot) |
| Pending exception/interrupt (kvm_vcpu_events) | per-vCPU on real CPU | **NO save/restore on context switch — gap G-EVENTS** |
| TSC delta | typically per-vCPU | not adjusted; KVM monotone |

## Section 2 — Identified gaps (specific code citations)

### G-AVX: AVX upper-half not saved on context switch

`arch/um/backend/kvm/thread.c:142-143`:
```c
rc = os_ioctl_generic(vcpu_fd, KVM_GET_FPU, (unsigned long)&s->fpu);
```
and `:172`:
```c
rc = os_ioctl_generic(vcpu_fd, KVM_SET_FPU, (unsigned long)&s->fpu);
```

The struct used (`s->fpu`, type `struct kvm_fpu`) is defined at
`arch/x86/include/uapi/asm/kvm.h:175` and contains:
```c
struct kvm_fpu {
    __u8  fpr[8][16];
    __u16 fcw, fsw; __u8 ftwx, pad1; __u16 last_opcode;
    __u64 last_ip, last_dp;
    __u8  xmm[16][16];   /* XMM0..XMM15: only 128 bits each */
    __u32 mxcsr; __u32 pad2;
};
```

Total ≈ 512 B — exactly the legacy FXSAVE region. The XSAVE area
beyond this (YMM hi-128, ZMM hi-256, ZMM16..31, opmask, PKRU,
MPX, etc.) lives in 0..2048 B more after the legacy region; KVM
does NOT round-trip it through `KVM_GET/SET_FPU`. The dedicated
ioctls are `KVM_GET_XSAVE` / `KVM_GET_XSAVE2` and the corresponding
SETs (`include/uapi/linux/kvm.h:1408-1411, 1628-1629`).

**No call site uses `KVM_GET_XSAVE` or `KVM_GET_XSAVE2` in the
backend.** Confirmed via:
```bash
grep -rn KVM_.*XSAVE arch/um/backend/kvm/
```
returns nothing.

**Mechanism of failure.** Glibc since 2.18 ships `__memmove_avx_
unaligned_erms` (`sysdeps/x86_64/multiarch/memmove-vec-unaligned-
erms.S`). The AVX2 memcpy reads via `vmovdqu (%rsi), %ymm0` and
writes via `vmovdqu %ymm0, (%rdi)`. **YMM0** = lower 128 bits in
XMM0 (saved by KVM_GET_FPU), upper 128 bits in YMM-state region
(NOT saved). On context switch from task A (with YMM0_high =
A_pattern) to task B (whose YMM0_high should be 0 from its own
prior writes), the vmovdqu reads `A_pattern` for the high half
and writes that into the destination. For pthread workloads that
do many memcpys in parallel — exactly the FreeThreadingTest /
subinterpreter pattern — this corrupts memory in a way that is
NOT a wild pointer (no SIGSEGV) but a **wrong-data write**.

Symptom matches `test_bytes.FreeThreadingTest.test_free_threading_
bytearrayiter`: the test spawns N threads each iterating a
shared bytearray. Iteration uses `Py_SIZE(self)` and slices the
bytearray; under the GIL-free model the slicing involves memcpy
on bytearray buffers. Cross-thread AVX leaks would corrupt the
buffer contents in ways that "iter results don't match expected
bytes" — exactly the kind of failure that a
`memcmp(expected, actual, len) != 0` assertion would catch.

For `test_struct.test_endian_table_init_subinterpreters`, each
subinterpreter has its own copy of `_struct.format_table`. The
test asserts that the table (a small SIMD-friendly array of
function pointers and dtype tags) is correctly initialized in
EVERY subinterpreter. Subinterpreters in CPython 3.14 use
threading internally; AVX corruption in the table init memcpy
would break this exactly as observed.

### G-AVX512: AVX-512 also unsaved (latent)

Same code path; `kvm_fpu` doesn't even cover ZMM. Less likely
to bite today (most distributions don't compile glibc with
AVX-512 dispatch enabled by default — `glibc` 2.38+ is more
aggressive about enabling AVX-512 via `--with-cpu` or
`HWCAP2_AVX512` runtime detection), but a strict superset of
G-AVX.

### G-PKRU: PKRU not saved (latent)

Glibc 2.34+ uses memory-protection keys for some hardening
features (e.g. `mprotect`-based safe-keeping in `dl_main`).
Without per-task PKRU save/restore, a task that programs PKRU
to deny some keys leaks that PKRU into the next task, which then
reads its own data and gets a #PF for "key denied". Currently
no failing test is known to depend on PKRU; reported here for
completeness.

### G-EVENTS: pending exception not saved on context switch

`arch/um/backend/kvm/thread.c:192-299`'s `kvm_context_switch`
calls `kvm_fpu_save_for_task(prev)` and `kvm_fpu_restore_for_
task(next)` but never touches `KVM_GET_VCPU_EVENTS` or
`KVM_SET_VCPU_EVENTS`. The only place those ioctls appear in
the codebase is the SHUTDOWN-path diagnostic at
`arch/um/backend/kvm/thread.c:4148-4162` (read-only).

**Mechanism of failure.** KVM's `vcpu_events.exception.injected`
is the bit that says "next vmenter, deliver this exception
unconditionally before resuming guest." If KVM_RUN is interrupted
(EINTR or KVM_EXIT_INTR) AFTER the host kernel has decided to
inject `#PF cr2=X err=4` into the guest but BEFORE the actual
delivery, the injection state is queued on the vCPU. The next
KVM_RUN (regardless of which UML task) delivers the #PF. If
task switch happened between the EINTR and the next entry,
**task B receives a #PF that was queued for task A**, with
task A's CR2 value (CR2 is set by the CPU on #PF entry, not
restored from sregs unless we explicitly write it).

**Mapping to test_list.test_repr_deep**. The deep-recursion
test calls `repr()` on a list nested 1000+ deep. Recursion
descends user stack; each frame's prologue takes a #PF on the
guard-page stack-extension boundary (lazy faulting). If task A
is doing deep recursion and a SIGALRM hits while a #PF is being
delivered, the keystone CR3-toggle correctly forces a flush BUT
the pending exception state could deliver that #PF to whatever
task wakes up next on this vCPU. Task B's user code then
crashes with a "phantom" #PF at task A's cr2. Symptom in CPython:
"NULL deref during deep recursion" because the cr2 is whatever
task A's mid-recursion PF was at, often a NULL-derived offset
when the pre-fault state was clean.

This isn't proven yet; but it fits the symptom shape and the
mechanism is documented in the KVM API for `vcpu_events`.

### G-DR: debug registers not saved

`arch/um/backend/kvm/thread.c` never calls `KVM_GET_DEBUGREGS`
or `KVM_SET_DEBUGREGS`. Real Linux saves DR0..DR7 per-task in
`struct thread_struct.debugreg[]`. UML probably escapes this
because no ptrace HW breakpoint mechanism crosses into the
guest under KVM today.

Risk class: latent. Not a likely cause of the 5 failing tests.

### G-TSC_AUX: not relevant under ncpus=1

The MSR is never written; KVM defaults to whatever value matches
the host vCPU id. Under our ncpus=1 guarantee (`lifecycle.c:142-146`,
`uml_ncpus > 1` rejected), there's only one CPU id to return, so
RDTSCP behaves consistently regardless. Catalogued for SMP follow-on.

### G-XSAVE: CR4.OSXSAVE never set

`arch/um/backend/kvm/sregs.c:261`:
```c
sregs->cr4 = KVM_CR4_PAE | KVM_CR4_OSFXSR | KVM_CR4_OSXMMEXCPT;
```

CR4.OSXSAVE (bit 18) is **NOT** set. CPUID is configured (in
`lifecycle.c:416-526`) to mask the AVX bits, but the masking is
not 100% airtight on every CPUID leaf; some glibc dispatch
tables will still pick AVX paths if HWCAP2 is set or if `LD_HWCAP_
MASK` isn't tightened. **If CPUID is leaking AVX-cap to the
guest, glibc dispatches to AVX memcpy, and AVX state is then
mutated on the vCPU — the YMM hi-128 region is in the XSAVE
area, which the guest cannot architecturally save (XSAVE-with-
XCR0!=allset would #GP if OSXSAVE=0, but the *read* via vmov
still works because no XSAVE/XSETBV is involved in basic AVX
arithmetic).**

This is the underlying reason G-AVX is hit: CPUID says yes,
the OS bit says no, glibc trusts CPUID, AVX moves run, AVX state
mutates without the OS knowing. Either:
- (a) CPUID must be tighter — strip every AVX bit so glibc never
  dispatches to AVX paths, or
- (b) OSXSAVE must be set AND we must save/restore the full XSAVE
  region per task.

(b) is the more architecturally correct fix because it doesn't
constrain workload performance, but it's the more invasive
change.

## Section 3 — Failure-mode mapping

For each of the 5 failing tests, the following table identifies
which gap is the most plausible cause, with reasoning. Each
mapping is a **hypothesis** — direct empirical confirmation
requires an instrumented build that logs (a) when the gap fires
and (b) the failing test's exact symptom.

| Test | Likely gap | Mechanism | Confidence |
|------|-----------|-----------|-----------|
| `test_struct.test_endian_table_init_subinterpreters` | **G-AVX** | Subinterpreter init populates `_struct.format_table` with format-code entries via memcpy of small structs; on AVX-capable glibc this uses `vmovdqu %ymm`. Multiple subinterpreters initialize concurrently (each gets its own thread); cross-thread AVX leak corrupts the table. Subinterpreter divergence would manifest as "format X works in interpreter 1, fails in interpreter 2." | HIGH — fits both the AVX dependence and the multi-thread shape |
| `test_decimal` doctest 731 | **G-AVX** or **G-EVENTS** | `_decimal` uses `mpd_*` C extension which has its own arena-based allocator; allocator paths use memcpy. Doctest 731 is a known race-prone test in CPython that exercises Decimal-from-str parsing under concurrent access. Either AVX leak corrupting digit buffers or pending-exception delivery on the wrong task would explain "this doctest sometimes fails under multi-threaded run." | MEDIUM — needs the actual doctest line to confirm threading shape |
| `test_bytes.FreeThreadingTest.test_free_threading_bytearrayiter` | **G-AVX** | Test name is explicit: free-threading (no GIL). N pthreads share a bytearray; each thread iterates and slices. Slicing → `bytearray_subscript` → `memcpy`. AVX-256 leak between threads = wrong byte contents = test assertion fails. | VERY HIGH — exact pattern G-AVX predicts |
| `test_list.test_repr_deep` | **G-EVENTS** (most likely) or stack overflow handling | Test recurses ~1000 deep. Each frame triggers stack growth (lazy guard-page #PF). NULL-deref during deep recursion ⇒ a #PF was delivered against a clean task with a poisoned cr2 from an earlier task's faulted state, OR pending interrupt handling clobbered task state. NOT obviously AVX. Could also be G-DEBUGCTL if recursion gets traced. | MEDIUM — symptom matches but several possible causes |
| `test_set.TestWeirdBugs.test_merge_and_mutate` | **G-AVX** (most likely) | Test name is "merge and mutate" — hash-set merge + concurrent mutation. CPython's `setobject.c::set_merge` uses memcpy on hash table buckets. Under threading, multiple threads can do merges/mutations; AVX leak corrupts the hash buckets (key/hash pairs) leading to "weird" duplicate keys, missed lookups, etc. | HIGH — fits "weird bugs" symptom + threading + memcpy use |

If gap G-AVX is the dominant cause, fixing it should resolve at
least 3 of the 5 modules (test_struct, test_bytes, test_set).
test_decimal and test_list might require the additional fix for
G-EVENTS.

The 8/10 (not 10/10) reliability of `import unittest` is more
likely G-EVENTS than G-AVX, because `import unittest` is
single-threaded but flaky — a pending-exception delivery that
fires only when SIGALRM happens to land mid-PF would explain
the 20% flake rate.

## Section 4 — Architectural recommendation

Three options, ranked by depth of fix:

### Option A (minimum viable) — Save/restore full XSAVE + VCPU_EVENTS in `kvm_context_switch`

**Code change**: replace the per-task `struct kvm_fpu` slot
with a per-task `struct { kvm_xsave xs; kvm_vcpu_events ev;
kvm_debugregs dr; }` blob. Save/restore via the
matching three ioctls instead of the single FPU one.

```c
struct kvm_per_task_state {
    struct task_struct *task;
    /* KVM-side queryable state */
    struct kvm_xsave        xsave;     /* full XSAVE area */
    struct kvm_vcpu_events  events;    /* pending exception */
    struct kvm_debugregs    debugregs; /* DR0..DR7, DR6 */
    /* No kvm_fpu — kvm_xsave subsumes it */
    struct kvm_per_task_state *next;
};
```

In `kvm_context_switch`:
```c
if (prev && next && prev != next) {
    kvm_pertask_save(prev);   /* GET_XSAVE + GET_VCPU_EVENTS + GET_DEBUGREGS */
    kvm_pertask_restore(next); /* SET_XSAVE + SET_VCPU_EVENTS + SET_DEBUGREGS */
}
```

`kvm_xsave.region[1024]` is 4 KiB — bigger than `kvm_fpu`'s 512 B
but still fits comfortably in slab. `KVM_GET_XSAVE2` returns
exactly the size matching the host's XSAVE feature mask (queryable
via `KVM_CHECK_EXTENSION(KVM_CAP_XSAVE2)`); use the larger
ioctl when the cap is present, else fall back to fixed-size
`KVM_GET_XSAVE`.

**Pros:**
- Surgical — touches only kvm_context_switch
- Preserves the singleton-vCPU model (no SMP refactor)
- Same hash-table lifetime model as today (still has the UAF
  noted in memo 16's B-FPU-HASH-UAF; should be fixed in tandem
  by allocating in `init_new_context` and freeing in
  `arch_release_task_struct`)

**Cons:**
- 3 extra ioctls per context switch (vs 2 today). At ~150k
  cycles per ioctl × 3 = ~450k cycles per switch. UML schedules
  ~1k times/sec under typical workload → ~3 ms/sec overhead =
  0.3% wall clock cost. Acceptable.
- Doesn't fix the underlying "single vCPU shared by all tasks"
  pattern. If we ever go SMP (lifecycle.c:142 lifts the
  `uml_ncpus > 1` block), per-task state on a per-vCPU shared
  resource still needs explicit save/restore.

**Cost estimate**: ~150 lines of C (the per-task struct, the
two save/restore helpers, the call sites in kvm_context_switch,
the lifetime hooks in init_new_context / arch_release_task_struct).

### Option B (correct but larger) — Per-task vCPU state struct, plus FPU UAF fix

Combine Option A with the lifetime fix from memo 16 (B-FPU-
HASH-UAF). Key the per-task state on `task->thread` not on
`(unsigned long)task` so it's safely embedded:

```c
struct arch_thread {
    /* existing fields ... */
    struct kvm_per_task_state *kvm_state;  /* alloc on init_new_context */
};
```

Allocate in `init_new_context`; free in `arch_release_task_struct`.
No hash table at all — one `kmalloc(sizeof(struct kvm_per_task_
state), GFP_KERNEL)` per task at fork time, one `kfree` at exit.

**Cost**: ~50 more lines of C (lifetime), but eliminates a
proven UAF.

### Option C (architectural) — Per-CPU vCPU + KVM-managed save/restore

Lift the `uml_ncpus > 1` block in lifecycle.c. Create one vCPU
per UML CPU. Use UML's existing per-CPU model. On context
switch, switch which vCPU's KVM_RUN is the active one (per-CPU
data); KVM itself manages save/restore between vCPU swaps.

**Pros:**
- Architecturally clean — matches how real SMP systems work.
  Each vCPU has its own state; KVM's vmenter/vmexit machinery
  preserves it.
- Eliminates the entire "save/restore on context switch"
  category.
- Enables SMP UML on KVM (a separately-tracked goal).

**Cons:**
- Substantially larger refactor (estimated 500-1000 lines).
- Requires the gadget state page to become per-vCPU (memo 11
  flags this as v2 SMP work).
- Requires re-doing the singleton-shadow-PT model (likely already
  per-mm post-#275; less invasive than feared).

**Recommendation for this memo's scope**: **Option B**, possibly
followed by Option C as a separate workstream once the parity
gate moves to 21/21.

Option A alone is insufficient because it doesn't fix the FPU
hash UAF, which is a separate proven bug. Option B fixes both
(per-task state save/restore + correct lifetime).

Tradeoff to discuss with reviewers:

- **If we do Option B**: the residual 5 modules likely move
  toward parity. Estimated 3-4 of them flip; 1-2 may need
  additional work for stack-fault-recovery edge cases.
- **If we do Option C immediately**: the right architecture but
  longer schedule. Worth doing only if Option B doesn't move the
  parity gate, OR as a planned follow-on.

## Section 5 — Concrete proof-of-concept

A minimal C reproducer that would demonstrate gap G-AVX in
isolation, runnable under both backends with a clear PASS
(seccomp) / FAIL (KVM) split:

```c
/*
 * test_avx_leak.c - minimal AVX state-leak reproducer for UML
 *
 * Compile:
 *    gcc -O2 -mavx2 -pthread -o test_avx_leak test_avx_leak.c
 *
 * Run inside UML (under both KVM and seccomp backends):
 *    ./test_avx_leak
 *
 * Expected outcome:
 *    - seccomp / ptrace: PASS (no divergence)
 *    - KVM: FAIL with diff lines showing wrong bytes in
 *      thread B's expected pattern, where the wrong bytes
 *      are thread A's pattern bytes that leaked via shared
 *      YMM upper-half state.
 *
 * Mechanism: each thread sets its own YMM register pattern,
 * busy-loops vmovdqu through a buffer (forcing AVX paths),
 * verifies the buffer contents match its own pattern. Without
 * per-thread YMM state isolation, threads will see each
 * other's high-half bytes.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>

#define BUF_SZ      (256 * 1024)   /* 256 KiB - bigger than L1 */
#define ITERATIONS  100000
#define NTHREADS    4

struct thread_arg {
    int   tid;
    __m256i pattern;     /* per-thread YMM pattern */
    int   failures;
};

static void *worker(void *p) {
    struct thread_arg *a = p;
    uint8_t *buf;
    size_t i, j;

    if (posix_memalign((void **)&buf, 32, BUF_SZ))
        abort();

    for (i = 0; i < ITERATIONS; i++) {
        /* Set this thread's pattern in YMM via vmovdqa. */
        for (j = 0; j < BUF_SZ; j += 32)
            _mm256_store_si256((__m256i *)(buf + j), a->pattern);

        /* Read back and compare with the pattern. Any difference
         * means YMM upper-half state was clobbered between the
         * stores in this iteration -- which can ONLY happen if
         * a context switch ran in the middle and another
         * thread's YMM upper-half leaked into this thread's
         * registers when we resumed.
         */
        for (j = 0; j < BUF_SZ; j += 32) {
            __m256i v = _mm256_load_si256((__m256i *)(buf + j));
            __m256i diff = _mm256_xor_si256(v, a->pattern);
            if (!_mm256_testz_si256(diff, diff)) {
                a->failures++;
                if (a->failures < 5) {
                    /* Dump diff and the byte pattern. */
                    uint8_t exp[32], act[32];
                    _mm256_storeu_si256((__m256i *)exp, a->pattern);
                    _mm256_storeu_si256((__m256i *)act, v);
                    printf("tid=%d iter=%zu off=%zu DIFFER\n",
                           a->tid, i, j);
                    printf("  expected:");
                    for (int k = 0; k < 32; k++)
                        printf(" %02x", exp[k]);
                    printf("\n  actual:  ");
                    for (int k = 0; k < 32; k++)
                        printf(" %02x", act[k]);
                    printf("\n");
                }
                break;  /* Don't flood; one diff per iter is enough */
            }
        }
    }

    free(buf);
    return NULL;
}

int main(void) {
    pthread_t th[NTHREADS];
    struct thread_arg args[NTHREADS];

    /* Each thread gets a distinct YMM pattern: lower-half =
     * 0xAA repeating, upper-half = thread id repeating. The
     * difference is in the AVX upper-half so the leak is visible
     * via vmovdqu / vmovdqa upper-bytes only.
     */
    for (int t = 0; t < NTHREADS; t++) {
        uint8_t bytes[32];
        memset(bytes, 0xAA, 16);
        memset(bytes + 16, (uint8_t)(0x10 + t), 16);
        args[t].tid     = t;
        args[t].failures = 0;
        memcpy(&args[t].pattern, bytes, 32);
        pthread_create(&th[t], NULL, worker, &args[t]);
    }

    int total = 0;
    for (int t = 0; t < NTHREADS; t++) {
        pthread_join(th[t], NULL);
        total += args[t].failures;
        printf("thread %d failures: %d\n", t, args[t].failures);
    }
    printf("TOTAL FAILURES: %d\n", total);
    return total ? 1 : 0;
}
```

**Test plan:**
1. Build a UML kernel with `CONFIG_UM_BACKEND_KVM_INTEGRATED=y`
   and `CONFIG_UM_BACKEND_KVM_GADGET=y`.
2. Build the reproducer for x86-64 with `-mavx2`.
3. Boot UML twice with different `backend=force=` values.
   Run the reproducer in each.
4. Expected:
   - Under `backend=force=seccomp`: TOTAL FAILURES: 0 (host
     kernel does proper FPU/XSAVE save/restore on UML's stub
     child process; per-thread YMM state isolated correctly).
   - Under `backend=force=kvm`: TOTAL FAILURES: > 0 (one or
     more threads observe wrong bytes; the wrong bytes are
     specifically in the AVX-upper-half region matching another
     thread's pattern).

A diagnostic version with even tighter bounds: spawn 2 threads,
sched_yield in the middle of each iteration, and assert byte-
exact match. Should be deterministic FAIL on KVM.

**Test plan to validate Option B fix (after applying the
suggested change):**
1. Apply Option B: per-task `kvm_xsave` save/restore on
   `kvm_context_switch`.
2. Re-run the reproducer.
3. Expected: TOTAL FAILURES: 0 on KVM as well.
4. Re-run cpython-parity. Expected: at least 3 of the 5 currently
   diverging modules flip to PARITY (test_struct, test_bytes,
   test_set; possibly test_decimal).

If parity moves but `test_list.test_repr_deep` still fails,
that's evidence for G-EVENTS (pending exception leak across
tasks) — apply Option B's events-save sub-component and retest.

A second reproducer specifically for G-EVENTS would exercise
"signal mid-page-fault" patterns: spawn 2 threads, each touching
a freshly-allocated guard-page region with high SIGALRM rate,
then verify each thread's local data is undamaged. That's harder
to write a deterministic harness for; recommend deferring until
the AVX repro shows the framework is sound.

## Section 6 — What to do with this memo

1. **First**: confirm the AVX hypothesis by running the
   reproducer above. ETA: 30 minutes.
2. **If confirmed**: implement Option B as the next-touch fix.
   ETA: half-day for the patch + half-day to re-run the parity
   gate + threading-test smoke. Expected outcome: parity flips
   from 17/21 to ≥20/21.
3. **If not confirmed and AVX isn't the cause**: instrument
   `kvm_context_switch` to log `KVM_GET_VCPU_EVENTS` before/after
   and check whether `events.exception.injected` is ever non-zero
   across a switch. If yes, gap G-EVENTS is real and Option B's
   events-save sub-component is the path forward.
4. **Either way**: track G-AVX, G-EVENTS, G-DR, G-PKRU, G-XSAVE
   in the issue tracker as "per-task vCPU state isolation"
   sub-tasks under #274 (or its successor). They're independent
   — each can be proven and fixed without prerequisite.

## Appendix A — file-by-file map of what was reviewed

- `arch/um/backend/kvm/thread.c:1-4325` — full read; key sections
  cited inline: 82-178 (FPU hash), 192-299 (kvm_context_switch),
  301-369 (kvm_init_thread_regs), 1564-1782 (MSR programming),
  1906-2498 (kvm_enter_guest), 3040-4283 (kvm_run_userspace).
- `arch/um/backend/kvm/kvm_backend.h:1-815` — full read; struct
  kvm_um lines 34-150, gadget structs 221-319.
- `arch/um/backend/kvm/sregs.c:1-305` — full read; segment +
  control register template at 219-304.
- `arch/um/backend/kvm/lifecycle.c:1-200` — vCPU creation + cap
  detection (1-200 reviewed; remainder skimmed for per-task
  signals and found none).
- `arch/um/backend/kvm/snapshot.c:42-200` — confirmed only 7
  MSRs are tracked + KVM_GET_FPU / KVM_GET_VCPU_EVENTS used
  only in capture path (not context switch).
- `arch/um/backend/kvm/mm.c:1-130` — kvm_mm_attach is the only
  place per-mm `kvm_shadow` allocates; no per-task state
  managed here (correct — task lifetime ≠ mm lifetime).
- `arch/x86/um/syscalls_64.c:1-65` — sys_arch_prctl writes
  `gp[FS_BASE]` per-task; correct path.
- `arch/x86/um/tls_64.c:1-19` — `arch_set_tls` writes
  `gp[FS_BASE]` per-task; correct path.
- `arch/x86/um/shared/sysdep/ptrace.h:54-62` — confirms
  `uml_pt_regs` is a per-task struct; gp[] lives in
  `current->thread.regs.regs.gp[]`.
- `arch/x86/include/uapi/asm/kvm.h:175-200` — `struct kvm_fpu`
  proves the legacy-only coverage; `struct kvm_xsave` at line
  386 + `struct kvm_vcpu_events` at 340 + `struct kvm_debugregs`
  at 377 confirm the ioctl surface for fix Option B.
- `include/uapi/linux/kvm.h:1397-1411` — `KVM_GET/SET_VCPU_
  EVENTS`, `KVM_GET/SET_DEBUGREGS`, `KVM_GET/SET_XSAVE` ioctl
  numbers + `KVM_CAP_XSAVE` / `KVM_CAP_XSAVE2` cap checks.
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
  16-architecture-review/02-vcpu-state.md:1-580` — read in full
  for prior context. Memo 16 catalogued state leaking INTO the
  guest at execve time; this memo extends to state leaking
  ACROSS task switches at runtime. Memo 16's SPEC1 mentions FPU
  hash key by task_struct but doesn't extend to AVX/XSAVE
  beyond legacy.
- `Documentation/virt/uml/redesign/STATUS.md:1-524` — read for
  the post-keystone landscape; the open follow-on at line 60-65
  ("residual issue is likely in how clone(CLONE_VM) tasks share
  but each need their own TLS / per-task vCPU state") is exactly
  this memo's scope.

## Appendix B — Why the current FPU save/restore is necessary but not sufficient

Memo 16's SPEC1 (line 296-309) noted "per-task FPU hash slot is
keyed by `task_struct *`, not by mm or user-state generation",
and recommended adding execve-time KVM_SET_FPU with zero. That
fix WAS applied (visible in `kvm_diag_skip_fpu_save` knob and
the `kvm_fpu_save_for_task` calls in `kvm_context_switch`).
What memo 16 did not catch — because it focused on execve-time
state — is that the legacy FXSAVE region (which `kvm_fpu`
covers) is only **half** the modern FPU state. Everything in
the XSAVE area beyond the first 512 B is silently shared.

The fix is conceptually simple: replace `KVM_GET_FPU` /
`KVM_SET_FPU` with `KVM_GET_XSAVE2` / `KVM_SET_XSAVE`
(or `KVM_GET_XSAVE` / `KVM_SET_XSAVE` if the cap2 isn't
present). The buffer grows from 512 B per task to ~2-4 KiB,
and we get full coverage of all XSAVE-managed state including
AVX, AVX-512, MPX, PKRU, etc.

Practically: ENABLE OSXSAVE in CR4 (so the host CPUID-AVX bits
that we already pass through become semantically meaningful),
SET XCR0 to enable the state components we want to preserve
(at least x87 + SSE + AVX = 0x7), then save/restore via XSAVE
ioctls. Without enabling OSXSAVE/XCR0 the AVX state is
"present but architecturally invalid" — KVM may still let
guest mutate it via vmov, but the host can't easily query it.

The cleanest path is:

1. In `kvm_setup_production_sregs`, set CR4.OSXSAVE.
2. In `kvm_enter_guest_program_msrs` (or a sibling helper), set
   XCR0 = 0x7 (x87 | SSE | AVX) via `KVM_SET_XCRS` once at
   prime-time.
3. In `kvm_context_switch`, replace the `kvm_fpu` save/restore
   with `kvm_xsave` save/restore.
4. Update CPUID masking to ALLOW AVX (since we now properly
   support it) but mask AVX-512 unless we explicitly enable
   it in XCR0 (since AVX-512 state is bigger and we may not
   want to pay the save/restore cost).

This is the architecturally correct realization of fix Option B.
