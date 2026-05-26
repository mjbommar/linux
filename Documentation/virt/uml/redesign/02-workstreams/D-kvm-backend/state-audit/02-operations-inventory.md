# UML KVM v2 — Operations Inventory (Layer 2)

**Created:** 2026-05-01
**Tip at audit:** `e5f231656ead`
**Status:** Audit fill-in for Layer 2 of the state-ownership audit (see
[00-overview.md](00-overview.md)).

Every operation that touches v2-managed state, with the precise state
items each one reads/writes. File:line citations throughout. No
proposals, no fixes — pure enumeration.

---

## Section 1 — Hardware-driven operations

State the CPU saves/restores autonomously across guest/user/kernel
transitions. v2 inherits these contracts from the SVM/VMX
architecture; we do not control them but we depend on them.

### VMRUN (entering guest)
- READS (host area saved by `vmsave` in `svm_prepare_switch_to_guest`,
  per AMD APM Vol.2 §15.5):
  - Host CS/SS/DS/ES/FS/GS selectors + bases, GDTR/IDTR, EFER, CR0/CR3/CR4,
    RSP, RIP, RFLAGS — saved into VMCB host-save area.
- WRITES (guest area loaded from VMCB.save):
  - CR0, CR2, CR3, CR4, CR8, EFER, RFLAGS, RIP, RSP, RAX
  - CS/SS/DS/ES/FS/GS/LDTR/TR (selector + base + limit + attr)
  - DR6, DR7, GDTR, IDTR, SYSENTER_{CS,ESP,EIP}
- Guest GP registers other than RAX are loaded by KVM's per-vendor
  asm trampoline (`__svm_vcpu_run` / `__vmx_vcpu_run`) immediately
  before the architectural VMRUN/VMLAUNCH.

### vmexit (leaving guest)
- WRITES into VMCB.save (AMD APM §15.6 / Intel SDM Vol.3 §27):
  - All guest CR/RIP/RSP/RFLAGS/RAX, segment regs, IDTR/GDTR, DR6/DR7,
    EFER, exit_code, exit_info_{1,2}, ee_intr_info, exitintinfo, CR2.
- WRITES into host control regs:
  - CR0/CR3/CR4/EFER/IDTR/GDTR/TR/CS/SS/DS/ES/FS/GS restored from
    host-save area (or via `vmload(host_save)` on SVM).
- Guest GP registers other than RAX are spilled to KVM's per-vCPU
  arch.regs[] by the asm trampoline.

### SYSCALL (CPL=3 → CPL=0; AMD APM Vol.3 / Intel SDM Vol.2A)
- READS: MSR_LSTAR (target RIP), MSR_STAR.SYSCALL_CS (CS/SS selectors),
  MSR_FMASK (RFLAGS bits to clear), current RIP and RFLAGS.
- WRITES (architectural):
  - RCX ← RIP (return address)
  - R11 ← RFLAGS
  - RIP ← MSR_LSTAR
  - RFLAGS ← RFLAGS & ~MSR_FMASK (IF/TF/etc cleared)
  - CS.sel ← STAR[47:32] | 0; CS becomes 64-bit code segment, CPL=0
  - SS.sel ← STAR[47:32] + 8
- Does NOT switch RSP. v2's trampoline runs on the user RSP.

### SYSRETQ (CPL=0 → CPL=3)
- READS: MSR_STAR.SYSRET_CS, RCX, R11.
- WRITES:
  - RIP ← RCX
  - RFLAGS ← R11
  - CS ← STAR[63:48] + 16; SS ← STAR[63:48] + 8; CPL=3
- The LSTAR trampoline (`syscall_trap.c:137`) issues `out %al, $0xf4`
  then `sysretq`; v2's syscall arm pre-stages RCX/R11 at
  `syscall_trap.c:1693-1696`.

### IDT delivery via IST (Intel SDM Vol.3 §6.14.5, §6.15)
- READS: IDT base+limit (via IDTR, programmed in
  `arch/um/backend/kvm-v2/exception.c`), GDT (for IST stack-segment),
  TSS.IST[n] (n encoded in IDT entry's IST field).
- WRITES (hardware push onto IST stack, in this exact order):
  - SS  → top - 8
  - RSP → top - 16
  - RFLAGS → top - 24
  - CS  → top - 32
  - RIP → top - 40
  - error_code → top - 48 (only for vectors 8/10/11/12/13/14/17/30)
  - then RSP ← TSS.IST[n], RIP ← gate.offset, CS ← gate.selector,
    CPL=0, RFLAGS.IF cleared (interrupt gate) or unchanged (trap gate),
    CR2 ← faulting linear address (#PF only).

### IRETQ (long mode, CPL change)
- POPS in reverse push order: RIP, CS, RFLAGS, RSP, SS. error_code is
  popped+discarded by software BEFORE iretq (the in-guest stub does
  `add $8, %rsp` if the vector pushed one).
- WRITES: RIP/CS/RFLAGS/RSP/SS, CPL.
- v2 handler stubs at `KVM_V2_HANDLERS_GVA` emit IRETQ as their tail
  (`exception.c`). `kvm_v2_ist_frame_write` (syscall_trap.c:875)
  patches RIP/RFLAGS/RSP so the pop lands post-handler.

### INT3 / debug
- INT3 (vector 3): IDT delivery, no error code.
- #DB (vector 1): IDT delivery, writes DR6.
- v2 wires both to the panic default-stub (no test workload exercises).

### Page fault (#PF)
- WRITES CR2 ← faulting linear address (Intel SDM §4.7).
- WRITES error_code (P/W/U/RSVD/I/PK/SS/SGX bits per SDM §4.7).
- Pushes 6-qword frame onto IST stack (see "IDT delivery via IST").
- KVM mirrors VMCB.save.cr2 into `vcpu->arch.cr2` and (with
  KVM_SYNC_X86_SREGS) into `kvm_run->s.regs.sregs.cr2` (x86.c:12758).

---

## Section 2 — KVM-driven (host kernel) operations in `arch/x86/kvm/`

### kvm_arch_vcpu_ioctl_run  (x86.c:12014)
- KVM_RUN ioctl entry. Consumes `kvm_dirty_regs` on entry,
  `kvm_valid_regs` on exit.
- Validates dirty/valid masks against `sync_valid_fields` (x86.c:12065-12066).
- Calls `sync_regs(vcpu)` if any dirty bits set (x86.c:12071-12072).
- Calls `vcpu_run(vcpu)` to drive `vcpu_enter_guest` until exit.
- Calls `store_regs(vcpu)` post-loop (x86.c:12124-12125).

### sync_regs  (x86.c:12763)
- READS `kvm_run->kvm_dirty_regs`. For each set bit:
  - `KVM_SYNC_X86_REGS` → `__set_regs(vcpu, &run->s.regs.regs)` then
    clears the dirty bit (x86.c:12765-12767).
  - `KVM_SYNC_X86_SREGS` → `__set_sregs(vcpu, &sregs)` then clears
    (x86.c:12770-12776).
  - `KVM_SYNC_X86_EVENTS` → `kvm_vcpu_ioctl_x86_set_vcpu_events` then
    clears (x86.c:12779-12785).

### store_regs  (x86.c:12748)
- READS `kvm_run->kvm_valid_regs`. For each set bit, dumps the
  current vCPU state into `run->s.regs.{regs,sregs,events}`. v2
  enables REGS+SREGS at vcpu_create so post-exit GPRs and CR2/CR3/CR4/
  fs.base/gs.base are always live in the mmap.

### __set_regs  (x86.c:12182)
- WRITES `vcpu->arch.regs[]` (RAX..R15, RIP, RFLAGS) from `struct
  kvm_regs`. Sets `KVM_REQUEST_*` bits where needed (e.g., the RIP
  write goes through `kvm_rip_write`).

### __set_sregs / __set_sregs_common  (x86.c:12520 / 12451)
- WRITES `vcpu->arch.{cr0, cr2, cr3, cr4, cr8, efer, apic_base}` and
  the descriptor segments (cs/ds/es/fs/gs/ss/tr/ldt + idt/gdt).
- Compares new vs old CR3/CR4 to set `mmu_reset_needed` and calls
  `kvm_make_request(KVM_REQ_TLB_FLUSH_GUEST, ...)` at x86.c:12531 +
  x86.c:12575 — this is the gate v2 leans on by toggling CR4.PGE
  every dispatch.

### vcpu_enter_guest  (x86.c:11167)
- Services pending KVM_REQ_* bits before VMRUN: TLB_FLUSH (x86.c:11218),
  EVENT, NMI, IRQ_WINDOW, MMU_SYNC etc.
- Calls `kvm_x86_ops.vcpu_run(vcpu, run_flags)` to execute VMRUN.
- On return processes vmexit reason and may loop or exit to userspace.

### svm_vcpu_run  (svm/svm.c:4413)
- Loads guest state from VMCB.save via VMRUN; calls `__svm_vcpu_run`
  (svm/svm.c:4406) — asm trampoline that loads guest GPRs + VMRUN.
- Post-VMRUN: spills guest GPRs to `vcpu->arch.regs[]`; `vmsave`
  (svm/svm.c:1429) restores host MSR-anchored state.

---

## Section 3 — v2 dispatcher operations

### kvm_v2_vcpu_run (main loop)  (vcpu.c:1476)
Sequence executed per dispatch:
- (vcpu.c:1483) `preempt_disable` — pins us to one host CPU.
- (vcpu.c:1485-1486) `cpu = smp_processor_id(); vcpu = kvm_v2_vcpu_get(cpu)` —
  picks the per-host-CPU pool member.
- (vcpu.c:1499) `run = vcpu->kvm_run` — the SYNC_REGS mmap.
- (vcpu.c:1514-1525) lazy CPUID install on first use (sticky).
- (vcpu.c:1570-1575) `um_tlb_sync(current->mm)` — drains UML's
  deferred-PTE queue into the spawner mm so KVM's mmu_notifier sees
  it BEFORE entering guest. Bumps `mm->context.tlb_gen` on success.
- (vcpu.c:1578-1581) `kvm_v2_load_user_sregs(vcpu, __pa(active_mm->pgd),
  regs->gp[HOST_FS_BASE], regs->gp[HOST_GS_BASE])` — see below.
- (vcpu.c:1589) `kvm_v2_fpu_install_on_first_run(vcpu)` — see below.
- (vcpu.c:1603) `kvm_v2_ist_frame_restore_pending(vcpu)` — replay
  per-task IST snapshot if last exit was an exception class.
- (vcpu.c:1614-1618) restore per-task `iotrap_fpu` via KVM_SET_FPU if
  `iotrap_fpu_valid`.
- (vcpu.c:1625-1626) `kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs)`
  + `kvm_dirty_regs |= KVM_SYNC_X86_REGS`.
- (vcpu.c:1630) `os_ioctl_generic(vcpu->vcpu_fd, KVM_RUN, 0)`.
- (vcpu.c:1650-1665) post-exit FPU capture: if `!(sregs.cr0 & TS)`,
  KVM_GET_FPU into `iotrap_fpu`. Otherwise leave as-is (Phase H.2 skip).
- (vcpu.c:1693-1696) snapshot `kvm_regs eintr_regs = run->s.regs.regs`
  + `kvm_sregs eintr_sregs = run->s.regs.sregs` + `exit_reason_snap`
  BEFORE `unblock_signals()` to defend against another task's KVM_RUN
  overwriting the live mmap.
- (vcpu.c:1711) `unblock_signals()` — drain UML's deferred signal queue.
- (vcpu.c:1717-1801) EINTR path:
  - marshal from snapshot back into `regs`,
  - if EINTR caught us mid-IDT-delivery in the PF stub
    (`rip ∈ [HANDLERS_GVA+0x140, +0x180)`), call
    `kvm_v2_handle_pf_eintr_inline(regs, run, vcpu, eintr_sregs.cr2)`
    (vcpu.c:1761-1769),
  - else if RIP is in the broader handler region, snapshot raw IDT
    frame + saved_cr2 into per-task arch_thread (vcpu.c:1770-1776),
  - drain mmu_gather, preempt_enable, `interrupt_end()`, return.
- (vcpu.c:1814-1815) marshal GPRs/sregs back from snapshot into
  `regs`.
- (vcpu.c:1832-1833) `um_mmu_gather_drain(current->mm)` — release
  pages deferred by previous dispatches' exit handling.
- (vcpu.c:1835-1868) switch on `exit_reason`:
  - `KVM_EXIT_IO` → `kvm_v2_handle_io_trap(regs, run, vcpu)`,
  - HLT/FAIL_ENTRY/INTERNAL_ERROR/SHUTDOWN/default → panic.
- (vcpu.c:1870) `preempt_enable`.

### kvm_v2_load_user_sregs  (vcpu.c:1176)
- READS:
  - `current->thread.arch.kvm_v2.{saved_cr2_valid, saved_cr2_at_eintr}` (1216-1217).
  - `current->mm->context.tlb_gen` (1273-1275).
- WRITES into `run->s.regs.sregs` (live mmap):
  - `sregs->cr3 = pgd_pa` (1184).
  - `sregs->fs.base = fs_base` (1185).
  - `sregs->gs.base = gs_base` (1186).
  - `sregs->cr2` = saved-cr2 (if valid) or 0 (1217-1221), then clears
    `saved_cr2_valid`.
  - `sregs->cr4 ^= X86_CR4_PGE` (1259) — forces guest TLB flush via
    KVM's `__set_sregs_common` mmu_reset path (x86.c:12575).
  - `sregs->efer = SCE | LME | LMA | NX` (1307).
  - `sregs->cr0 |= X86_CR0_TS` (1321) — arms lazy FPU detection.
- WRITES into `vcpu`:
  - `atomic_set(&vcpu->kick_pending, 0)` (1270).
  - `WRITE_ONCE(vcpu->current_mm, current->mm)` (1271).
  - `atomic64_set(&vcpu->last_seen_tlb_gen, current->mm->context.tlb_gen)` (1273).
- WRITES `run->kvm_dirty_regs |= KVM_SYNC_X86_SREGS` (1323).

### kvm_v2_marshal_to_kvm_regs  (vcpu.c:1339)
- READS `src->gp[HOST_AX..R15, IP, EFLAGS]` (1342-1361).
- WRITES `dst->{rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8..r15, rip,
  rflags}`. RFLAGS is OR'd with `1<<1` (reserved-must-be-1, AMD64
  SDM §3.1.4).
- Caller sets `kvm_dirty_regs |= KVM_SYNC_X86_REGS`.

### kvm_v2_marshal_from_kvm_regs  (vcpu.c:1373)
- READS `src->{rax..r15, rip, rflags}`.
- WRITES `dst->gp[HOST_AX..R15, IP, EFLAGS]`. HOST_ORIG_AX is
  intentionally NOT written here — caller arranges it.

### kvm_v2_marshal_sregs_back  (vcpu.c:1430)
- READS `src->fs.base`, `src->gs.base` (sregs snapshot or live mmap).
- WRITES `dst->gp[HOST_FS_BASE]`, `dst->gp[HOST_GS_BASE]` (1433-1434).
- Symmetric pair to load_user_sregs's fs.base/gs.base writes.

### kvm_v2_handle_io_pf  (syscall_trap.c:1052)
- READS:
  - `vcpu->ist_stack_kva + PAGE_SIZE - {40,48}` via `kvm_v2_ist_frame_read`
    — error_code, user_rip, user_cs, user_rflags, user_rsp, user_ss
    (syscall_trap.c:1059, frame layout 824-862).
  - `run->s.regs.sregs.cr2` (live mmap, 1068).
  - `vcpu->ist_stack_kva + PAGE_SIZE - 64` (captured CR2 from in-guest
    stub, 1088-1089).
  - `vcpu->ist_stack_kva + PAGE_SIZE - 72` (captured RDX, 1090-1091).
  - `regs->gp[HOST_DX]` (used in the cr2-fallback heuristic, 1102/1127).
- WRITES into `regs` (uml_pt_regs):
  - `regs->gp[HOST_IP] = frame.user_rip` (1170).
  - `regs->gp[HOST_SP] = frame.user_rsp` (1171).
  - `regs->gp[HOST_EFLAGS] = frame.user_rflags` (1172).
  - `regs->is_user = 1` (1173).
  - `regs->faultinfo.{error_code, cr2, trap_no=14}` (1187-1189).
- CALLS that may sleep / schedule:
  - `segv_handler(SIGSEGV, NULL, regs, NULL)` (1205) →
    `handle_page_fault` → `handle_mm_fault` (CAN SLEEP).
  - `interrupt_end()` (1237) — drains TIF_NEED_RESCHED via
    `schedule()`, TIF_SIGPENDING via `do_signal(regs)`,
    TIF_NOTIFY_RESUME via `resume_user_mode_work()`
    (`arch/um/kernel/process.c:87-102`).
- WRITES (post-handlers):
  - `vcpu->ist_stack_kva` IST frame slots (RIP/CS/RFLAGS/RSP/SS) via
    `kvm_v2_ist_frame_write` (1254 → 875).
  - `current->thread.arch.kvm_v2.ist_frame[0..5]` and
    `ist_pending = true` (875 → 912-918).
  - `run->s.regs.regs.*` via `kvm_v2_marshal_to_kvm_regs` (1267).
  - `run->kvm_dirty_regs |= KVM_SYNC_X86_REGS` (1268).
- SYNCHRONIZATION:
  - Caller (`kvm_v2_vcpu_run`) holds `preempt_disable`. The
    `segv_handler` and `interrupt_end` calls can yield (sleep,
    schedule); under the v2 backend's no-DEBUG_ATOMIC_SLEEP config
    these proceed without warning.
- POTENTIAL CONTAMINATION (matrix-level concern, recorded for Layer 3):
  - Between the yield in `segv_handler`/`interrupt_end` and the
    post-handler writes, another UML task on the same per-host-CPU
    pthread can dispatch on this vCPU. `kvm_v2_ist_frame_write` reads
    CS/SS LIVE from `vcpu->ist_stack_kva` (893, 896) — could pick up
    the other task's IDT-delivery frame data.

### kvm_v2_handle_pf_eintr_inline  (syscall_trap.c:1001)
- READS `vcpu->ist_stack_kva` IST frame via `kvm_v2_ist_frame_read`
  (1008). Takes `cr2` as a parameter (caller passes
  `eintr_sregs.cr2` snapshot, NOT live mmap, because the mmap may
  have been overwritten by a context-switched task).
- WRITES `regs->gp[HOST_IP/SP/EFLAGS]`, `regs->is_user`,
  `regs->faultinfo.*` (1010-1017).
- CALLS `segv_handler(SIGSEGV, NULL, regs, NULL)` (1023),
  `interrupt_end()` (1024).
- WRITES post-handler IST frame, per-task snapshot via
  `kvm_v2_ist_frame_write` (1026); marshals back to
  `run->s.regs.regs` + dirties REGS (1027-1028).
- Bypasses the in-guest stub entirely — the snapshot/replay path was
  fragile under SMP because the saved RSP pointed at the
  vmexit-vCPU's IST page and the resumed task could land on a
  different vCPU.

### kvm_v2_handle_io_trap (SYSCALL arm)  (syscall_trap.c:1501)
- After dispatching PF/GP/UD/DE/OF/PANIC variants for non-SYSCALL
  ports (1517-1533), the SYSCALL fall-through:
- READS:
  - `regs->gp[HOST_AX]` → `syscall_nr` (1542).
  - `regs->gp[HOST_CX]` → user RIP (1565).
  - `regs->gp[HOST_R11]` → user RFLAGS (1566).
- WRITES:
  - `PT_SYSCALL_NR(regs->gp) = syscall_nr` (1549).
  - `regs->is_user = 1` (1550).
  - `regs->gp[HOST_IP] = regs->gp[HOST_CX]` (1565).
  - `regs->gp[HOST_EFLAGS] = regs->gp[HOST_R11]` (1566).
  - `UPT_SYSCALL_NR(regs) = -1` (1577, defensive).
- CALLS:
  - `handle_syscall(regs)` (1582) — dispatches the syscall;
    may sleep, may set HOST_AX = -ERESTART*, may invoke do_signal
    indirectly; returns with regs->gp[HOST_AX] = syscall return value.
  - `interrupt_end()` (1622) — same drains as above.
- WRITES (post-handler):
  - `PT_SYSCALL_NR(regs->gp) = -1` (1662, x86_64 always taken).
  - `kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs)` (1693).
  - `run->s.regs.regs.rcx = regs->gp[HOST_IP]` (1694) — explicit
    overwrite so SYSRETQ pops the post-handler RIP (signal handler
    VA, restart-RIP, or original).
  - `run->s.regs.regs.r11 = regs->gp[HOST_EFLAGS]` (1695).
  - `run->kvm_dirty_regs |= KVM_SYNC_X86_REGS` (1696).

### kvm_v2_ist_frame_read  (syscall_trap.c:824)
- READS 5 (or 6 with error_code) qwords from
  `vcpu->ist_stack_kva + PAGE_SIZE - 40` (and `-48` for error_code)
  into a local `struct kvm_v2_ist_frame` (851-861).
- Pure read — no writes anywhere.

### kvm_v2_ist_frame_write  (syscall_trap.c:875)
- READS `regs->gp[HOST_IP/EFLAGS/SP]`, plus `top - 40 + 8` (CS) and
  `top - 40 + 32` (SS) LIVE from the IST page (893, 896).
- WRITES IST page slots:
  - `top - 40 + 0` ← `regs->gp[HOST_IP]` (892).
  - `top - 40 + 16` ← `regs->gp[HOST_EFLAGS]` (894).
  - `top - 40 + 24` ← `regs->gp[HOST_SP]` (895).
- WRITES per-task arch_thread snapshot:
  - `arch_thread.kvm_v2.ist_frame[0..5]` ← {error_code, RIP, CS,
    RFLAGS, RSP, SS} (912-917).
  - `arch_thread.kvm_v2.ist_pending = true` (918).

### kvm_v2_ist_frame_snapshot_raw  (syscall_trap.c:967)
- READS 6 qwords from IST page (top-48 error_code, then RIP/CS/RFLAGS/RSP/SS).
- WRITES `arch_thread.kvm_v2.ist_frame[0..5]` + `ist_pending = true`
  (972-978).
- Used on the EINTR-mid-stub path at vcpu.c:1775 to preserve the
  hardware-pushed frame as-is for stub replay on the next dispatch.

### kvm_v2_ist_frame_restore_pending  (syscall_trap.c:929)
- READS `arch_thread.kvm_v2.{ist_pending, ist_frame[0..5]}`.
- WRITES the IST page slots (top-48 through top-40+32) from the
  snapshot (937-942), then clears `ist_pending` (944).
- Defends against cross-task IST clobber: if last exit for THIS task
  was an exception class, restore OUR frame before re-entering.

### kvm_v2_fpu_capture_for_fork  (vcpu.c:1898)
- preempt_disable, picks per-CPU vCPU.
- READS via `os_ioctl_generic(vcpu_fd, KVM_GET_FPU, &to->kvm_v2.fpu)`
  (1917).
- WRITES `to->kvm_v2.fpu` (the 512-byte legacy FXSAVE area),
  `to->kvm_v2.fpu_valid = true` (1928).
- Called from `arch_copy_thread` at fork. NULL-or-sentinel guard
  leaves `fpu_valid = false` and child gets architectural reset.

### kvm_v2_fpu_capture_for_switch_out  (vcpu.c:1964)
- preempt_disable, picks per-CPU vCPU.
- READS `KVM_GET_FPU` into `from->thread.arch.kvm_v2.fpu` (1985).
- WRITES `from->thread.arch.kvm_v2.fpu_valid = true` (1996).
- Called from `kvm_v2_context_switch` (vcpu.c:2025) BEFORE delegating
  to `seccomp_context_switch`. Carries outgoing task's FPU snapshot
  into per-task storage so its NEXT first-run installs it.

### kvm_v2_fpu_install_on_first_run  (vcpu.c:2076)
- READS `current->thread.arch.kvm_v2.{fpu_valid, fpu}`.
- WRITES (if `fpu_valid`):
  - `KVM_SET_FPU` from `&a->kvm_v2.fpu` (2083-2084).
  - `a->kvm_v2.fpu_valid = false` (2087, one-shot).
- WRITES (if not `fpu_valid`): nothing (E.4 hypothesis: leave per-vCPU
  FPU untouched on re-entry to avoid clobbering mid-instruction XMM).

### kvm_v2_tlb_kick_others  (vcpu.c:820)
- READS:
  - `mm->context.tlb_gen` (828, the post-um_tlb_sync gen).
  - `raw_smp_processor_id()` (829).
  - `vcpu->current_mm` (846, READ_ONCE).
  - `vcpu->last_seen_tlb_gen` (854).
- WRITES (per remote vCPU):
  - `atomic_cmpxchg(&vcpu->kick_pending, 0, 1)` (860).
  - `os_send_ipi(cpu, 0 /* UML_IPI_RES */)` (861) — pthread_sigqueue
    of IPI_SIGNAL to the remote cpu_thread.
- Today this helper is defined but the activation point in
  `um_tlb_sync` (tlb.c:511-519) is COMMENTED OUT — see Layer 1 note.

### um_tlb_sync  (arch/um/kernel/tlb.c:413)
- READS `mm->context.{sync_tlb_lock, sync_tlb_range_from,
  sync_tlb_range_to}` (420-422).
- READS the page tables and walks pgd/p4d/pud/pmd/pte ranges, calling
  `ops.{mmap,unmap}` (= `um_backend->mm_region_added/removed`) for each
  needsync entry (439-440, 444-469).
- WRITES per-pgd/p4d/pud/pmd/pte: `*_mkuptodate` on success.
- WRITES `mm->context.tlb_gen += 1` on success (512, atomic64_inc).
- WRITES `mm->context.sync_tlb_range_from/to = 0` on success
  (522-524) or narrows `from = addr` on partial failure (526).

---

## Section 4 — UML-kernel cross-cutting

### schedule / __switch_to  (process.c:75-85)
- `__switch_to`:
  - WRITES `to->thread.prev_sched = from`.
  - `set_current(to)` → updates `cpu_tasks[cpu]`.
  - Calls `um_on_context_switch(from, to)` then
    `um_backend_dispatch(context_switch, from, to)` — for v2 this is
    `kvm_v2_context_switch` (vcpu.c:2025) which captures FPU into
    `from`, drains `from->active_mm`'s deferred TLB queue, then
    delegates to `seccomp_context_switch` (which does the
    `switch_threads` jmp_buf swap).
  - Finally `arch_switch_to(current)`.
- What v2 preserves across switch: per-task `regs` (gp[], faultinfo,
  is_user, etc. — these live in `current->thread.regs`), per-task
  arch_thread.kvm_v2.{fpu, fpu_valid, iotrap_fpu, iotrap_fpu_valid,
  ist_frame[], ist_pending, saved_cr2_at_eintr, saved_cr2_valid}.
  The per-vCPU mmap is NOT swapped — it stays bound to whatever
  pool member the next dispatch picks.

### interrupt_end  (process.c:87-102)
- READS `read_thread_flags()` in a loop.
- For each set flag in `_TIF_WORK_MASK`:
  - `_TIF_NEED_RESCHED` → `schedule()`.
  - `_TIF_SIGPENDING | _TIF_NOTIFY_SIGNAL` → `do_signal(regs)`.
  - `_TIF_NOTIFY_RESUME` → `resume_user_mode_work(regs)`.
- Loops until no flags remain.
- Called from every v2 exit handler (handle_io_pf:1237,
  handle_io_gp:1309, handle_io_ud:1375, handle_io_de:1411,
  handle_io_of:1457, handle_io_trap syscall arm:1622) and from the
  EINTR path at vcpu.c:1800.

### do_signal → setup_rt_frame
- WRITES a `struct rt_sigframe` onto the user stack at
  `regs->gp[HOST_SP] - sizeof(frame)`. The frame holds the saved GPRs,
  RIP, RFLAGS, FPU state, sigmask.
- WRITES `regs->gp[HOST_IP]` ← signal handler VA, `regs->gp[HOST_SP]`
  ← stack-after-frame, `regs->gp[HOST_DI/SI/DX]` ← signal arguments.
- v2's IST `kvm_v2_ist_frame_write` then propagates the new RIP/RSP
  into the IST iretq frame so the in-guest stub returns into the
  signal handler, not the original faulting instruction.

### sys_rt_sigreturn
- READS the saved frame at user RSP.
- WRITES `regs->gp[*]`, `regs->gp[HOST_IP/SP/EFLAGS]` from the frame.
- Restores user FPU state (transitively into `iotrap_fpu` on next
  KVM_GET_FPU).

### arch_send_call_function_*ipi + os_send_ipi  (smp.c:92)
- `arch_send_call_function_ipi` / `arch_send_call_function_single_ipi`
  (in `arch/um/kernel/smp-internal.c`) call `os_send_ipi(cpu, vector)`.
- `os_send_ipi(cpu, vector)`:
  - WRITES via `pthread_sigqueue(cpu_threads[cpu], IPI_SIGNAL, value)`
    (smp.c:96) — queues IPI_SIGNAL (= SIGRTMIN) to the destination
    cpu_thread with `si_value.sival_int = vector`.
- `ipi_sig_handler` (smp.c:125-138): runs in the destination
  cpu_thread's signal context, sets `signals_enabled = 0`, calls
  `uml_ipi_handler(si->si_value.sival_int)` → routes to
  `ipi_handler(vector)` which dispatches scheduler_ipi /
  generic_smp_call_function_interrupt etc.

---

## Section 5 — Signals / interrupts that can interrupt KVM_RUN

`kvm_v2_install_signal_mask` (vcpu.c:751) installs a mask that BLOCKS
every signal except:

### SIGALRM (timer preempt — always allowed)
- Source: `timer_real_alarm_handler` (`arch/um/os-Linux/signal.c:373`)
  fires from a SI_TIMER signal (host-side per-cpu_thread timer).
- Effect: if the cpu_thread is mid-`KVM_RUN`, the kernel's signal
  delivery causes KVM to abort with `-EINTR`, snapshot vcpu state into
  `kvm_run->s.regs.{regs,sregs}` (because KVM_SYNC_X86_{REGS,SREGS}
  is in `kvm_valid_regs`), and return.
- v2 path:
  - vcpu.c:1717 detects `rc == -EINTR`,
  - marshals from snapshot (1718-1719),
  - if RIP is in PF stub region, runs `handle_pf_eintr_inline`
    (1768) which dispatches the fault inline,
  - else if RIP is anywhere in the handler region, snapshots
    `saved_cr2_at_eintr` + `kvm_v2_ist_frame_snapshot_raw` (1773-1775)
    so the next dispatch can replay,
  - drains mmu_gather + preempt_enable + `interrupt_end()` + return
    (1786-1801).
- #121 root cause (closed 2026-05-01): load_user_sregs zeroed
  `sregs.cr2` AND cross-task IST clobber when EINTR caught
  mid-IDT-delivery. Fix: per-task `saved_cr2_at_eintr` + raw IST
  snapshot at the EINTR-mid-stub gate.

### IPI_SIGNAL (SIGRTMIN — cross-vCPU IPI, SMP only)
- Unblocked on SMP via `sigdelset(&set, os_ipi_signum())` at
  vcpu.c:781.
- Source: `os_send_ipi(cpu, vector)` from a remote cpu_thread, e.g.
  scheduler / smp-call-function / `kvm_v2_tlb_kick_others`.
- Handler: `ipi_sig_handler` (smp.c:125) → `uml_ipi_handler` →
  `ipi_handler(vector)`.
- Effect on KVM_RUN: kicks the receiving vCPU out with `-EINTR`,
  same EINTR path as SIGALRM. The next dispatch's CR4.PGE toggle in
  `load_user_sregs` does the actual local guest-TLB flush.
- Vector 0 (UML_IPI_RES) is repurposed for tlb_kick_others — the
  scheduler_ipi side-effect is harmless when nothing to schedule.

### All other signals
- BLOCKED for the duration of `KVM_RUN` via `KVM_SET_SIGNAL_MASK`
  (vcpu.c:762 sigfillset, then sigdelset for SIGALRM and IPI_SIGNAL
  only). SIGIO, SIGCHLD, SIGUSR1/2, SIGSEGV, SIGBUS etc. cannot
  interrupt the ioctl — they queue and are delivered when the
  cpu_thread later calls `unblock_signals()` (vcpu.c:1711) or returns
  to UML kernel context with the host-side mask cleared.

---

**End of Layer 2.** Cross-references:
- Per-state-item READ/WRITE matrix → Layer 3
  ([03-state-ownership-matrix.md](03-state-ownership-matrix.md)).
- Per-suspect-register deep audits (RBP/RAX/RDX/FS_BASE) → Layer 4
  ([04-suspect-register-audits.md](04-suspect-register-audits.md)).
