# UML KVM v2 — Layer 3: State Ownership Matrix

**Created:** 2026-05-01 · **Tip:** `7acce0471395`

Cross-references Layer 1's state inventory (rows) against Layer 2's
operations inventory (columns). Each cell answers: **R/W**, **where
the value flows**, **what synchronization protects it**, and **is the
operation race-free under SMP T>=N**?

The matrix is organized by state-item category. "Race-free under SMP
T>=N?" answers are filled per the invariants restated below; cells
marked NO or UNCERTAIN are summarized in the BUG CANDIDATES section
at the bottom.

## Pre-matrix invariants (per Layer 4)

- **Per-task `regs`.** `kvm_v2_vcpu_run` always receives
  `&current->thread.regs.regs` (process.c:129,:146). `gp[]` lives
  per-task.
- **Whole-dispatch preempt_disable.** vcpu.c:1483 → vcpu.c:1788 (EINTR
  exit) / vcpu.c:1870 (normal exit). `current` is pinned for one
  dispatch; no other UML task on this host CPU runs concurrently.
- **Per-host-CPU vCPU pool.** `vcpus[NR_CPUS]` (vcpu.c:1058);
  `vcpu->kvm_run` mmap, `ist_stack_kva`, `tss_kva` are per-host-CPU,
  shared serially across tasks on the same CPU.
- **Per-task EINTR snapshot.** vcpu.c:1693-1696 stack-snapshots
  `kvm_run->s.regs.{regs,sregs}` BEFORE `unblock_signals()`; marshal
  reads from snapshot, never the live mmap.
- **HOST_* indices** (arch/x86/um/user-offsets.c:41-68):
  HOST_AX=10, HOST_CX=11, HOST_DX=12, HOST_BP=4, HOST_IP=16,
  HOST_FS_BASE=21, HOST_GS_BASE=22.

In every cell below, "preempt_disable + per-task `regs`" means: the
write target is `current->thread.regs.regs.gp[]`, owned exclusively
by the calling task; preempt_disable forbids another task from
running on the same CPU mid-dispatch.

---

## Category 1a: GP registers (RAX–R15, RIP, RFLAGS — 18 items)

All 18 GP registers travel through identical infrastructure:
`vcpu->kvm_run->s.regs.regs.<field>` ↔ `current->thread.regs.regs
.gp[HOST_*]`. SYNC_REGS dirty/valid bit `KVM_SYNC_X86_REGS` is OR'd
in at vcpu.c:1626 / vcpu.c:1696 / syscall_trap.c:1268 before each
KVM_RUN. Combined transition table for the generic GPR (i.e., not
RAX/RCX/R11 — those have SYSCALL-specific extras called out below):

### Combined transition table (RBX, RDX, RSI, RDI, RBP, RSP, R8–R10, R12–R15)

| Operation | R/W | Source → Sink | Sync | Race-free? |
|---|---|---|---|---|
| VMRUN entry (asm trampoline) | R | `vcpu->arch.regs[]` → guest register file | KVM mutex on vcpu_fd, then HW | YES |
| vmexit (asm trampoline) | W | guest register file → `vcpu->arch.regs[]` | KVM mutex | YES |
| `sync_regs` (KVM_SYNC_X86_REGS dirty) | W | `run->s.regs.regs.<r>` → `vcpu->arch.regs[]` (x86.c:12765) | KVM mutex | YES |
| `store_regs` (KVM_SYNC_X86_REGS valid) | W | `vcpu->arch.regs[]` → `run->s.regs.regs.<r>` (x86.c:12748) | KVM mutex | YES |
| `marshal_to_kvm_regs` (vcpu.c:1339-1362) | R/W | per-task `gp[HOST_*]` → `run->s.regs.regs.<r>`; OR `KVM_SYNC_X86_REGS` (vcpu.c:1626/1323) | preempt_disable | YES |
| `marshal_from_kvm_regs` (vcpu.c:1373-1396) | W | snapshot `eintr_regs.<r>` → per-task `gp[HOST_*]` | preempt_disable, src=stack-local (vcpu.c:1694) | YES |
| EINTR snapshot (vcpu.c:1693-1696) | R | `run->s.regs.regs` → stack `eintr_regs` | preempt_disable, BEFORE unblock_signals | YES |
| `__switch_to` / `kvm_v2_context_switch` (vcpu.c:2025) | persists | `task->thread.regs.regs.gp[]` survives across switch | per-task storage | YES |
| `do_signal` / `setup_rt_frame` | R | `gp[HOST_*]` copied to user sigframe | per-task | YES |
| `sys_rt_sigreturn` | W | sigframe → `gp[HOST_*]` | per-task | YES |
| `kvm_v2_handle_io_pf` (syscall_trap.c:1170-1172) | W | `frame.user_{rip,rsp,rflags}` → `gp[HOST_IP/SP/EFLAGS]` | preempt_disable | YES |
| Guest-side reads/writes by user code | R/W | guest TLB → guest register file | guest CPL=3 | UNCERTAIN — guest TLB staleness is the L4 top hypothesis |

The "guest-side R/W via stale TLB" cell is the only one not
guaranteed race-free; see BUG CANDIDATES.

### RAX (gp[HOST_AX]) — extra writers around SYSCALL semantics

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| LSTAR trampoline `out %al, $0xf4` (syscall_trap.c:137-140) | R | `%al` to bus; KVM dispatches by `run->io.port` not data; RAX in live regfile unchanged | per-vCPU; serialized by preempt_disable | YES |
| In-guest #PF stub `push %rax ; mov %cr2,%rax ; ... ; pop %rax` (exception.c:213-227) | R/W | RAX restored before `out`; transient only inside stub | guest CPL=0; serialized by preempt_disable | YES |
| `kvm_v2_handle_io_trap` syscall arm (syscall_trap.c:1542) | R | `syscall_nr = regs->gp[HOST_AX]` | preempt_disable | YES |
| `handle_syscall` (skas/syscall.c:26 default ENOSYS, :82 actual return) | W | `PT_REGS_SET_SYSCALL_RETURN(regs, ret)` → `gp[HOST_AX] = ret` | preempt_disable, per-task | YES |
| `marshal_to_kvm_regs` (syscall_trap.c:1693) | W | `gp[HOST_AX]` → `run->s.regs.regs.rax` for SYSRETQ | preempt_disable | YES |
| EINTR-mid-PF-stub bypass (vcpu.c:1761-1769 → handle_pf_eintr_inline) | -/transient | guest live RAX may be transient `%cr2` from partial stub; bypass uses snapshot, not live | preempt_disable + snapshot | YES (per L4 §RAX-(E)) |

### RCX / R11 (gp[HOST_CX], gp[HOST_R11]) — SYSCALL semantics extras

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| Hardware SYSCALL | W | RCX ← user RIP, R11 ← user RFLAGS (HW) | architectural | YES |
| `handle_io_trap` syscall arm (syscall_trap.c:1565-1566) | R | `gp[HOST_IP] = gp[HOST_CX]`, `gp[HOST_EFLAGS] = gp[HOST_R11]` | preempt_disable | YES |
| Post-handler explicit overwrite (syscall_trap.c:1694-1695) | W | `run->s.regs.regs.rcx = gp[HOST_IP]`, `.r11 = gp[HOST_EFLAGS]` so SYSRETQ pops post-handler RIP/RFLAGS | preempt_disable | YES |
| Hardware SYSRETQ | R | RIP ← RCX, RFLAGS ← R11 | architectural | YES |

### Why GPR cells are race-free

Per-task `gp[]` is touched only by `current` under preempt_disable.
The `run->s.regs.regs.*` mmap is shared serially: each dispatch
overwrites it via `marshal_to`, snapshots it before
`unblock_signals()`, then `marshal_from` reads from the stack-local
snapshot. No GPR cell can be written from another task's data
(L4 §RBP/RAX/RDX confirms).

---

## Category 1b: Control registers (CR0, CR2, CR3, CR4, CR8 — 5 items)

`run->s.regs.sregs.{cr0,cr2,cr3,cr4,cr8,efer}` is the live mmap
slice; `KVM_SYNC_X86_SREGS` ORs into `kvm_dirty_regs` at vcpu.c:1323.

### CR0

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| Install at vcpu_create (vcpu.c:517-518) | W | `PE\|MP\|NE\|WP\|PG` → `sregs.cr0` | KVM mutex | YES |
| `load_user_sregs` per-dispatch (vcpu.c:1321) | W | `sregs->cr0 \|= X86_CR0_TS` to arm H.2 lazy-FPU detection | preempt_disable | YES |
| Post-vmexit valid_regs (`store_regs`) | W | KVM writes `vcpu->arch.cr0` → `run->s.regs.sregs.cr0`; v2 reads `cr0 & TS` (vcpu.c:1650) | KVM mutex | YES |
| Post-vmexit lazy-FPU branch (vcpu.c:1650) | R | If `!(sregs.cr0 & TS)` → KVM_GET_FPU into iotrap_fpu | preempt_disable | YES |

### CR2

CR2 has THREE potential per-dispatch writers (Layer 1 obs #7); the
#121 fix made the interaction explicit.

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| Hardware #PF | W | CR2 ← faulting linear address (SDM §4.7) | HW | YES |
| In-guest #PF stub captures CR2 to IST scratch (exception.c:213-227) | R | `mov %cr2, %rax ; mov %rax, -64(%rsp)` writes IST page byte `PAGE_SIZE-64` | guest CPL=0 + preempt_disable | YES |
| `store_regs` (KVM_SYNC_X86_SREGS valid) | W | `vcpu->arch.cr2` → `run->s.regs.sregs.cr2` (x86.c:12758) | KVM mutex | YES |
| EINTR snapshot to `eintr_sregs.cr2` (vcpu.c:1694) | R | live mmap → stack-local; consumed by `handle_pf_eintr_inline` (vcpu.c:1768) | preempt_disable, BEFORE unblock_signals | YES |
| `load_user_sregs` per-dispatch (vcpu.c:1216-1221) — #121 fix | W | if `kvm_v2.saved_cr2_valid` then `sregs->cr2 = saved_cr2_at_eintr` else 0; clears `saved_cr2_valid` | preempt_disable; per-task | YES |
| `kvm_v2_handle_io_pf` reads `sregs.cr2` (syscall_trap.c:1068) | R | live mmap | preempt_disable | YES |
| EINTR-mid-stub fix snapshot (#121, vcpu.c:1773-1776 + processor_64.h:79-103) | W | `eintr_sregs.cr2` → `current->thread.arch.kvm_v2.saved_cr2_at_eintr`; sets `saved_cr2_valid` | preempt_disable; per-task | YES |

### CR3

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| `load_user_sregs` per-dispatch (vcpu.c:1184) | W | `sregs->cr3 = __pa(active_mm->pgd)` | preempt_disable | YES |
| `kvm_v2_load_cr3` (vcpu.c:1141) | W | helper version | preempt_disable | YES |
| KVM `__set_sregs` mmu_reset compare (x86.c:12531) | R | new CR3 ≠ old triggers `KVM_REQ_TLB_FLUSH_GUEST` | KVM mutex | YES |
| `store_regs` post-exit | W | `vcpu->arch.cr3` → `run->s.regs.sregs.cr3` | KVM mutex | YES |

### CR4

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| Install (vcpu.c:519) | W | `PAE\|OSFXSR\|OSXMMEXCPT` | KVM mutex | YES |
| `load_user_sregs` per-dispatch (vcpu.c:1259) | W | `sregs->cr4 ^= X86_CR4_PGE` to force guest-TLB flush via KVM `__set_sregs_common` mmu_reset (x86.c:12575) | preempt_disable | YES (for dispatching vCPU) |
| Cross-vCPU TLB flush coverage | — | CR4.PGE toggle covers ONLY the dispatching vCPU; remote vCPUs need `kvm_v2_tlb_kick_others` (vcpu.c:820) which is currently DECLARED but its activation in `um_tlb_sync` (tlb.c:511-519) is COMMENTED OUT (per L4 conclusion) | none | NO — see BUG CANDIDATES |
| `store_regs` post-exit | W | `vcpu->arch.cr4` → mmap | KVM mutex | YES |

### CR8

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| Unmanaged | — | KVM-default 0; no APIC; no v2 reader/writer | n/a | YES (vacuously) |

### Why CR cells are race-free (and where they're not)

CR0/CR3 are per-dispatch installs from per-task data under
preempt_disable. CR2's three writers are made explicit by the #121
fix; per-task `saved_cr2_at_eintr` carries EINTR-mid-stub state
across dispatches without sharing through the mmap. CR4.PGE toggle
flushes the LOCAL vCPU only; cross-vCPU coverage is BUG #1.

---

## Category 1c: Segment registers + bases (8 × 4 = 32 items, plus 3 MSR-anchored bases)

CS/DS/ES/SS/TR/LDTR are CONSTANT after install (vcpu.c:478-515,
702-706): user CS=0x2b at CPL=3, data segs 0x23, TR=0x30 busy. Guest
cannot modify them — LSTAR trampoline doesn't alter CS/SS via
long-mode descriptors and CPUID-masked FSGSBASE prevents
`wr{fs,gs}base`. **All cells in this row group: YES (install-once,
constant).** FS/GS selectors likewise install-once; only their
`base` fields change per-dispatch.

### FS_BASE (gp[HOST_FS_BASE] per-task; sregs.fs.base in mmap)

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| `arch_prctl(ARCH_SET_FS, x)` (arch/x86/um/syscalls_64.c:23) | W | `regs->gp[FS_BASE/8] = arg2` (the only per-task FS_BASE writer in `arch/x86/um/`) | preempt_disable, in handle_syscall | YES |
| `arch_prctl(ARCH_GET_FS)` (arch/x86/um/syscalls_64.c:33) | R | reads `gp[HOST_FS_BASE]` | per-task | YES |
| `load_user_sregs` per-dispatch (vcpu.c:1185) | W | `sregs->fs.base = fs_base` (arg from gp[HOST_FS_BASE]) | preempt_disable | YES |
| `store_regs` post-exit | W | `vcpu->arch.fs.base` → `run->s.regs.sregs.fs.base` (live mmap) | KVM mutex | YES |
| EINTR snapshot to `eintr_sregs.fs.base` (vcpu.c:1694) | R | mmap → stack-local | preempt_disable, BEFORE unblock | YES |
| `marshal_sregs_back` (vcpu.c:1430-1434) | W | `eintr_sregs.fs.base` → `gp[HOST_FS_BASE]` | preempt_disable, src=snapshot | YES |
| Guest `wrfsbase` | — | CPUID-masked (vcpu.c:128-167); guest cannot execute | n/a | YES |

### GS_BASE — symmetric to FS_BASE

Replace HOST_FS_BASE → HOST_GS_BASE; `sregs.fs.base` →
`sregs.gs.base`; vcpu.c:1186 (load), vcpu.c:1434 (marshal back). Same
race-free analysis.

### KERNEL_GS_BASE (MSR_KERNEL_GS_BASE, 0xc0000102)

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| Unmanaged (Layer 1 obs #4; vcpu.c:310-312 explicit) | — | D.1 trampoline does not use `%gs:` so no per-task KGS context | n/a | YES (vacuously) |

### Why segment cells are race-free

CS/SS/DS/ES are install-once (vcpu.c:478-515). FS/GS bases write
the per-CPU mmap once per dispatch from per-task `gp[]` under
preempt_disable; marshal-back uses snapshot. Guest cannot
`wr{fs,gs}base` (CPUID-masked). Only FS_BASE writer in `arch/x86/um/`
is `arch_prctl(ARCH_SET_FS)` (L4 §FS_BASE confirms).

---

## Category 1d/1e: Programmed MSRs (LSTAR, STAR, FMASK, EFER — 4)

`kvm_v2_vcpu_program_msrs` (vcpu.c:323-392) installs once per pool
member at vcpu_create.

| MSR | Cadence | Operation | Sync | Race-free? |
|---|---|---|---|---|
| MSR_LSTAR | once at create | `KVM_SET_MSRS`, value=`KVM_V2_LSTAR_GVA` (vcpu.c:331) | KVM mutex on vcpu_fd | YES |
| MSR_STAR | once at create | `KVM_SET_MSRS`, value `(0x18<<48)\|(0x08<<32)` (vcpu.c:332) | KVM mutex | YES |
| MSR_SYSCALL_MASK (FMASK) | once at create | `KVM_SET_MSRS`, value `0x47700` (vcpu.c:334) | KVM mutex | YES |
| MSR_EFER | install (vcpu.c:520) AND every dispatch (vcpu.c:1307) | `sregs->efer = SCE\|LME\|LMA\|NX` | install: KVM mutex; per-dispatch: preempt_disable | YES (per-dispatch is redundant per L1 obs #6, not a race) |

### Why MSR cells are race-free

Install-once MSRs run with KVM mutex held on the per-vCPU fd. Once
installed, the guest cannot modify them (no in-guest `wrmsr` in v2's
trampoline/stub code). EFER's per-dispatch write is redundant but
deterministic — same value every time, no inter-task interleave.

---

## Category 1f: Debug registers (DR0–DR3, DR6, DR7 — 6)

| Operation | R/W | Detail | Race-free? |
|---|---|---|---|
| Unmanaged in v2 | — | No callers of `KVM_GET/SET_DEBUGREGS` in arch/um/backend/kvm-v2/. UML's `arch_thread.debugregs[8]` is the ptrace concept (Layer 1 obs #5). Guest hw breakpoints don't work today. | YES (vacuously) |

---

## Category 1g: FPU / XSAVE state (one row covering iotrap_fpu cycle + per-task snapshot)

Two storage homes per Layer 1 §1g:
- `arch_thread.kvm_v2.fpu` + `.fpu_valid` — per-task switch-out / first-run cycle
- `arch_thread.kvm_v2.iotrap_fpu` + `.iotrap_fpu_valid` — per-task per-dispatch cycle (H.1b)

| Operation | R/W | Source → Sink | Sync | Race-free? |
|---|---|---|---|---|
| `kvm_v2_fpu_capture_for_fork` (vcpu.c:1898-1933) | W | per-CPU vCPU regfile → `to->kvm_v2.fpu`; `to->kvm_v2.fpu_valid = true` | preempt_disable | YES |
| `kvm_v2_fpu_capture_for_switch_out` (vcpu.c:1964) | W | `KVM_GET_FPU` → `from->kvm_v2.fpu`; `fpu_valid = true` | preempt_disable; per-CPU vCPU | YES |
| `kvm_v2_fpu_install_on_first_run` (vcpu.c:2076) | R/W | if `current->kvm_v2.fpu_valid`: `KVM_SET_FPU` from `current->kvm_v2.fpu`; clear `fpu_valid` | preempt_disable | YES |
| Per-dispatch `KVM_SET_FPU` (vcpu.c:1614-1617) | W | `current->kvm_v2.iotrap_fpu` → vCPU regfile (if `iotrap_fpu_valid`) | preempt_disable | YES |
| Per-dispatch `KVM_GET_FPU` (vcpu.c:1653-1655) | W | vCPU regfile → `current->kvm_v2.iotrap_fpu`; gated by `!(sregs.cr0 & TS)` (H.2 lazy skip) | preempt_disable | YES |
| `arch_flush_thread` (processor_64.h:130) | W | clears `fpu_valid` | per-task | YES |

### Why FPU cells are race-free

Per-task `kvm_v2.{fpu,iotrap_fpu}` only touched by `current` under
preempt_disable. vCPU regfile is shared serially: each dispatch
installs OUR data, KVM_RUN, then snapshots ours back. H.2 lazy
skip safe — CR0.TS set post-exit means regfile unchanged.

---

## Category 2: v2 per-task state (`arch_thread.kvm_v2.*`)

Layer 1 §2; processor_64.h:28-104. Ownership rule: every field is
owned exclusively by the owning task.

### `ist_frame[6]` + `ist_pending`

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| `kvm_v2_ist_frame_write` (syscall_trap.c:875 → 912-918) | W | post-PF-handler: writes `arch_thread.kvm_v2.ist_frame[0..5]`, `ist_pending = true` | preempt_disable; per-task | YES |
| `kvm_v2_ist_frame_snapshot_raw` (syscall_trap.c:967-978) | W | EINTR-mid-stub raw IDT-pushed frame snapshot (#121); writes ist_frame[0..5] + ist_pending | preempt_disable; per-task | YES |
| `kvm_v2_ist_frame_restore_pending` (syscall_trap.c:929-944) | R/W | reads ist_pending; if set, copies snapshot back to vCPU IST page; clears ist_pending | preempt_disable; per-task read, per-vCPU IST write | YES |
| `kvm_v2_ist_frame_write` reads CS/SS LIVE from IST page (syscall_trap.c:893, 896) | R | reads `vcpu->ist_stack_kva + PAGE_SIZE - 40 + 8` (CS) and `+32` (SS) live — per-vCPU shared page, but caller is preempt_disabled and the prior IDT delivery on this CPU was THIS task (since preempt_disable was held since dispatch start) | preempt_disable | YES (verified — single dispatch holds preempt the whole time) |

### `iotrap_fpu` + `iotrap_fpu_valid`

Covered in Category 1g.

### `saved_cr2_at_eintr` + `saved_cr2_valid` (#121 fix)

| Operation | R/W | Detail | Sync | Race-free? |
|---|---|---|---|---|
| EINTR-mid-stub snapshot (vcpu.c:1773-1776) | W | `eintr_sregs.cr2` → `arch.kvm_v2.saved_cr2_at_eintr`; `saved_cr2_valid = true` | preempt_disable; per-task | YES |
| `load_user_sregs` consumption (vcpu.c:1216-1218) | R/W | reads pair, writes `sregs->cr2`, clears `saved_cr2_valid` | preempt_disable; per-task | YES |

### `last_seen_tlb_gen`, `current_mm` — these are per-VCPU, not per-task. Covered in Category 3.

### Why per-task cells are race-free

Per-task storage is defended by `current` pinning under
preempt_disable. No cross-task path writes another task's
`arch_thread.kvm_v2.*`; only `arch_copy_thread` writes to a child
via `kvm_v2_fpu_capture_for_fork` (vcpu.c:1898).

---

## Category 3: v2 per-vCPU state (`struct kvm_v2_vcpu.*`)

Layer 1 §3; backend.h:288-357. Pool: `vcpus[NR_CPUS]` (vcpu.c:1058).
Per-host-CPU, NOT per-task — the central sharing surface.

| Field | Operation | R/W | Sync | Race-free? |
|---|---|---|---|---|
| `vcpu_fd`, `kvm_run`, `kvm_run_size`, `cpu` | set at create (vcpu.c:889, 906-909) | W | KVM mutex | YES |
| `cpuid_primed` | first KVM_RUN sets sticky (backend.h:282-286, vcpu.c:1514-1525) | W | preempt_disable; sticky one-shot | YES (per L1 obs #9 — single-VM model) |
| `ist_stack_kva/gpa/top_gva` | E.2 install (backend.h:421) | W (install only) | KVM mutex | YES |
| `ist_stack_kva` IST page CS/SS slots read live by `ist_frame_write` (syscall_trap.c:893,896) | R | preempt_disable; current task's prior IDT delivery is the only writer in this dispatch | YES |
| `ist_stack_kva` CR2/RDX scratch slots written by in-guest stub (`PAGE_SIZE-64`, `PAGE_SIZE-72`) and read by `handle_io_pf` (syscall_trap.c:1088-1091) | R/W | preempt_disable; serialized | YES |
| `tss_kva/gpa/gva` | E.2 install (backend.h:323-329) | W (install only) | KVM mutex | YES |
| `kick_pending` | set by `kvm_v2_tlb_kick_others` (backend.h:336, vcpu.c:860) cmpxchg 0→1; reset by `load_user_sregs` (vcpu.c:1270) | atomic R/W | atomic_cmpxchg | YES (atomic) |
| `last_seen_tlb_gen` | snapshot per-dispatch from mm gen (vcpu.c:1273-1274); read by `kvm_v2_tlb_kick_others` (vcpu.c:854) for narrowing | atomic R/W | atomic64 | YES (atomic ordering) |
| `current_mm` | `WRITE_ONCE` per-dispatch (vcpu.c:1271); READ_ONCE by kicker (vcpu.c:846) for advisory narrowing (Layer 1 obs #8) | R/W | READ_ONCE / WRITE_ONCE | YES (advisory; stale read = unneeded IPI) |

### Why per-vCPU cells are race-free (and the hidden risk)

The "per-vCPU" struct is per-host-CPU, shared by every UML task on
this CPU. preempt_disable guarantees only one task mid-dispatches a
given vCPU at a time, so per-dispatch mmap r/w is serialized. IST
page CS/SS slots are HW-written on every IDT delivery and read
within the same dispatch by `ist_frame_write` — the live-mmap read
is always the current task's data. Per-task `ist_frame[]` snapshots
defend against another task's eventual IDT delivery on the same
vCPU (Layer 1 obs #11).

Hidden risk is **cross-vCPU**, not cross-task on one vCPU: if task X
on vCPU-A holds mm M and task Y on vCPU-B mutates M's page tables,
vCPU-A's hardware TLB needs a flush. `kvm_v2_tlb_kick_others` is
that mechanism; its activation is COMMENTED OUT (L4). See BUG #1.

---

## Category 4: v2 per-mm state (`mm_context_t.*`)

Layer 1 §4; arch/um/include/asm/mmu.h:16-81.

| Field | Operation | R/W | Sync | Race-free? |
|---|---|---|---|---|
| `sync_tlb_lock` | guards range below | spinlock | self | YES |
| `sync_tlb_range_from`, `_to` | extended by `tlb_mark_sync` (deferred-PTE bookkeeping); drained by `um_tlb_sync` (tlb.c:420-422, 522-526) | R/W | `sync_tlb_lock` | YES |
| `deferred_free_lock` | guards `deferred_free_pages` | spinlock | self (mmu.h:60) | YES |
| `deferred_free_pages`, `deferred_free_count` | mmu_gather hand-off; pages held with extra ref until next vcpu_run drains via `um_mmu_gather_drain(current->mm)` (vcpu.c:1832-1833) | R/W | `deferred_free_lock` | YES |
| `tlb_gen` | `atomic64_inc` by `um_tlb_sync` on success (tlb.c:512); read by `kvm_v2_tlb_kick_others` (vcpu.c:828); snapshot to `vcpu->last_seen_tlb_gen` per-dispatch (vcpu.c:1273-1274) | atomic R/W | atomic64 | YES (atomic ordering) |
| `kvm_v2_tlb_kick_others` is DECLARED but its caller in `um_tlb_sync` (tlb.c:511-519) is COMMENTED OUT | — | helper exists, never called | n/a | NO — see BUG CANDIDATES #1 |

### Why per-mm cells are mostly race-free (with the cross-vCPU gap)

Range bookkeeping and deferred-free are spinlock-protected; tlb_gen
is atomic64. The data structure is fine — the KICKER IS NOT WIRED.
`um_tlb_sync` bumps `tlb_gen` but does not call
`kvm_v2_tlb_kick_others`, so remote vCPUs' `last_seen_tlb_gen`
goes stale and CR4.PGE only flushes locally. This is BUG #1.

---

## Category 5: Per-VM state (`struct kvm_v2_vm.*`)

Layer 1 §5; backend.h:49-168. Single instance per UML invocation.

| Field | Operation | R/W | Sync | Race-free? |
|---|---|---|---|---|
| `kvm_fd`, `vm_fd` | open(/dev/kvm) + KVM_CREATE_VM (backend.h:50-51) | W (init) | one-shot | YES |
| `caps` | negotiated at init (backend.h:52, vcpu.c:921-922) | W (init) | one-shot | YES |
| `cpuid` | curated buffer; install deferred to first KVM_RUN per pool member (backend.h:53) | R | per-vCPU first-run, sticky | YES |
| `memslots`, `memslot_bitmap` | mutated by Phase B add/remove | R/W | `vm->lock` (backend.h:64) | YES |
| `trampoline_*`, `idt_kva`, `handlers_kva`, `gdt_kva` etc. | install at vm_create | W (init) | one-shot | YES |

All per-VM state is install-once or guarded by `vm->lock`. No
per-dispatch mutators here.

---

## Category 6: Pending events (`vcpu->arch.exception/interrupt/...`) — KVM_SYNC_X86_EVENTS NOT enabled per L1

Layer 1 §1i + obs #2. `kvm_valid_regs` at vcpu.c:924-925 sets only
REGS|SREGS, NOT EVENTS.

| Slot | Operation | R/W | Race-free? |
|---|---|---|---|
| `exception.{injected,nr,has_error_code,error_code}` | unmanaged | — | YES (vacuously — v2 surfaces guest exceptions as KVM_EXIT_IO via the trampoline-stub design, backend.h:142-148) |
| `interrupt.*` | unmanaged (no APIC) | — | YES (vacuously) |
| `nmi.*`, `sipi_vector`, `smi.*`, `triple_fault.pending` | unmanaged | — | YES (vacuously) |
| `exception_payload` | unmanaged; v2 reads post-vmexit CR2 directly from `sregs.cr2` (Layer 1 §1i) | — | YES |

Future work that needs to inject an exception (e.g. wire #BP for a
debugger) must enable KVM_SYNC_X86_EVENTS and add per-task event
storage.

---

## BUG CANDIDATES (cells flagged NO or UNCERTAIN)

The matrix has only one true NO and one UNCERTAIN — they are the
same bug expressed two ways. L6 must investigate.

### Bug Candidate #1 — Cross-vCPU TLB kick is wired but disabled

**Cells:**
- Category 1b CR4 row "Cross-vCPU TLB flush coverage" → **NO**
- Category 4 last row "`kvm_v2_tlb_kick_others` activation in
  um_tlb_sync is COMMENTED OUT" → **NO**
- Category 1a generic-GPR last row "Guest-side R/W via stale TLB" →
  **UNCERTAIN** (the symptom-side manifestation of Bug #1)

**State items affected:** all guest GP registers, all guest control
registers, every byte of guest memory the user code reads — because
a stale guest TLB delivers wrong data to load instructions, which
then propagates into every subsequent register write.

**Mechanism:**
1. Task X on vCPU-A runs with mm M.
2. Task Y on vCPU-B mutates M's page tables via mremap/munmap;
   `tlb_mark_sync` extends `M->context.sync_tlb_range_*`; eventually
   `um_tlb_sync(M)` runs (e.g. at next vcpu_run, vcpu.c:1570-1575)
   and bumps `M->context.tlb_gen`.
3. CR4.PGE-toggle in `load_user_sregs` flushes vCPU-B's hardware TLB
   but NOT vCPU-A's.
4. `kvm_v2_tlb_kick_others` (vcpu.c:820) is the helper that would
   enumerate remote vCPUs whose `current_mm == M` and
   `last_seen_tlb_gen < M->context.tlb_gen`, cmpxchg their
   `kick_pending` 0→1, and `os_send_ipi` IPI_SIGNAL to bounce them
   out of KVM_RUN so their next dispatch's CR4.PGE toggle does the
   flush.
5. Per L4: the CALLER in `um_tlb_sync` (tlb.c:511-519) is
   **commented out**. So vCPU-A continues with stale TLB.
6. Task X on vCPU-A loads `mov 8(%rsp), %rbp` — guest TLB returns
   the old physmem PFN; the value loaded is from a recycled page
   (e.g. another task's stack). RBP becomes a tid-4 stack address
   (the mt-mini-diag signature).
7. Same mechanism explains RAX=0 (load from MMAP_NULL-shaped
   recycled page), RDX=low (load from low-VA page), and any FS_BASE
   miscompare (load through stale TLB into the canary check).

**Citations:**
- Helper definition: vcpu.c:820 (per L2 §"kvm_v2_tlb_kick_others")
- Caller commented out: tlb.c:511-519 (per L1 §4 note + L2 §um_tlb_sync)
- CR4.PGE local-only flush: vcpu.c:1259 (per L2 §load_user_sregs)
- vcpu.c:1244-1245 archive comment ("narrowing the toggle to
  tlb_stale && same_cr3 regressed v1's gate to ~70% pass rate vs
  100%") — quoted by L4: matches mt-mini's failure rate exactly.
- Symptom convergence: L4 §"Cross-register conclusion" identifies
  guest TLB staleness as the single hypothesis that explains all
  four observed register corruptions simultaneously.

**Why this is the only NO/UNCERTAIN cell in 149 state items:**

L4 exhaustively proved every backend write site for RBP/RAX/RDX/
FS_BASE is race-free under preempt_disable + per-task gp[] + EINTR
snapshot. The matrix extends that: no backend operation on any
state item can write another task's data into ours via the mmap,
IST page, or per-task storage.

The only mechanism left consistent with mt-mini's signature is
**the guest CPU reading wrong DATA** because its hardware TLB went
stale when a remote vCPU mutated the shared mm. Data flow: bad
guest TLB → bad load → bad register file → vmexit ships through
the (race-free) marshal → user-visible corruption. The marshal is
honest; the bug is upstream of it.

### Recommended L6 actions

1. Read `kvm_v2_tlb_kick_others` (vcpu.c:820) and confirm cmpxchg
   dedup + gen-narrowing logic.
2. Read `um_tlb_sync` (tlb.c:500-530); document what is commented
   out and which prior fix attempt disabled it.
3. Audit ordering: tlb_gen-bump in `um_tlb_sync` must happen-before
   the `last_seen_tlb_gen` read in the kicker.
4. Audit IPI semantics: does `os_send_ipi(cpu, UML_IPI_RES)`
   reliably bounce a remote pthread out of KVM_RUN? IPI_SIGNAL is
   unblocked for KVM_RUN on SMP via vcpu.c:781.
5. Reproduce with helper RE-WIRED under SMP T=8 mt-mini; per L4
   should restore v1-archive ~100% pass.

---

**End of Layer 3.** Cross-references: L1 (01-state-inventory.md),
L2 (02-operations-inventory.md), L4
(04-suspect-register-audits.md), L5 (05-toolkit.md), L6
(99-findings.md).
