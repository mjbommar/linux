# UML KVM v2 — Layer 1: State Inventory

**Created:** 2026-05-01 · **Tip:** `e5f231656ead`

Per [00-overview.md](00-overview.md). All citations as `file:line`.

## Total state-item count

| Category | Items |
|---|---|
| 1a. GP regs (RAX–R15, RIP, RFLAGS) | 18 |
| 1b. Control regs (CR0/CR2/CR3/CR4/CR8) | 5 |
| 1c. Segment regs (8 × {sel,base,limit,attr}) | 32 |
| 1d. MSR-anchored bases (FS/GS/KGS) | 3 |
| 1e. Programmed MSRs (LSTAR/STAR/FMASK + EFER) | 4 |
| 1f. Debug regs (DR0–DR3, DR6, DR7) | 6 |
| 1g. FPU/XSAVE (x87 8 + XMM 16 + control 4) | 28 |
| 1h. Descriptor tables (IDT/GDT/LDTR/TR) | 4 pairs |
| 1i. Pending-event slots (excep/intr/NMI/SIPI/SMI/triple) | 6 |
| 2.  Per-task `arch_thread.kvm_v2` | 8 |
| 3.  Per-vCPU `struct kvm_v2_vcpu` | 13 |
| 4.  Per-mm `mm_context_t` v2 fields | 6 |
| 5.  Per-VM `struct kvm_v2_vm` | 16 |
| **Total** | **~149** |

---

## Section 1 — Hardware-defined guest state

`run->s.regs.*` rows refer to the per-vCPU `kvm_run` mmap
(`vcpu->kvm_run`, vcpu.c:889). `KVM_CAP_SYNC_REGS` enabled via
`kvm_valid_regs` (vcpu.c:924-925); writes committed by OR-ing
`kvm_dirty_regs` (vcpu.c:1323) before `KVM_RUN`. Sync-bit defs:
arch/x86/include/uapi/asm/kvm.h:448-467. All rows per-vCPU.

### 1a. GP registers (18)

`struct kvm_regs`: arch/x86/include/uapi/asm/kvm.h:117-124. Lives at
`run->s.regs.regs.<field>`. Marshal in
`kvm_v2_marshal_to_kvm_regs` (vcpu.c:1339-1362), out
`kvm_v2_marshal_from_kvm_regs` (vcpu.c:1373-1396). Sync bit
`KVM_SYNC_X86_REGS`; fallback `KVM_GET/SET_REGS`.

| Item | UML mirror in `uml_pt_regs.gp[]` |
|---|---|
| RAX | `gp[HOST_AX]` (vcpu.c:1344) |
| RBX/RCX/RDX | `gp[HOST_BX/CX/DX]` |
| RSI/RDI | `gp[HOST_SI/DI]` |
| RBP/RSP | `gp[HOST_BP/SP]` |
| R8–R15 | `gp[HOST_R8..R15]` |
| RIP | `gp[HOST_IP]` (vcpu.c:1360) |
| RFLAGS | `gp[HOST_EFLAGS]` (`\| 1<<1` defensively, vcpu.c:1361) |

### 1b. Control registers (5)

`struct kvm_sregs`: arch/x86/include/uapi/asm/kvm.h:150-159. At
`run->s.regs.sregs.<field>`. Sync bit `KVM_SYNC_X86_SREGS`; fallback
`KVM_GET/SET_SREGS`.

| Reg | Owner / write site |
|---|---|
| CR0 | install: PE\|MP\|NE\|WP\|PG (vcpu.c:517-518); per-dispatch re-arms TS for H.2 lazy FPU (vcpu.c:1321) |
| CR2 | per-dispatch: 0 OR `arch.kvm_v2.saved_cr2_at_eintr` (vcpu.c:1216-1221) — #121 fix |
| CR3 | per-dispatch `__pa(mm->pgd)` (vcpu.c:1184); also `kvm_v2_load_cr3` (vcpu.c:1141) |
| CR4 | install: PAE\|OSFXSR\|OSXMMEXCPT (vcpu.c:519); per-dispatch XOR PGE for guest-TLB flush (vcpu.c:1259) |
| CR8 | unmanaged — KVM-default 0 |

### 1c. Segment registers (8 × 4 = 32)

`struct kvm_segment`: arch/x86/include/uapi/asm/kvm.h:132-140 (base
u64, limit u32, selector u16, attrs type/present/dpl/db/s/l/g/avl/unusable).
At `run->s.regs.sregs.{cs,ds,es,fs,gs,ss,tr,ldt}`. Sync bit
`KVM_SYNC_X86_SREGS`.

| Segment | Selector / base / limit / attrs |
|---|---|
| CS | sel=0x2b, base=0, limit=0xffffffff, type=0xb, dpl=3, l=1, g=1 (vcpu.c:478-489) — CPL=3 from boot |
| DS | sel=0x23, base=0, limit=0xffffffff, type=0x3, dpl=3, db=1, g=1 (vcpu.c:490-501) |
| ES/SS | = DS (vcpu.c:515) |
| FS | base = `gp[HOST_FS_BASE]` per-dispatch (vcpu.c:1185); read-back vcpu.c:1433 |
| GS | base = `gp[HOST_GS_BASE]` per-dispatch (vcpu.c:1186); read-back vcpu.c:1434 |
| TR | sel=0x30, base=`vcpu->tss_gva`, limit=103, type=11 busy TSS — `kvm_v2_install_descriptors_sregs` (vcpu.c:702-706) |
| LDTR | KVM-reset default; unusable=1 |

### 1d. MSR-anchored bases (3)

| MSR | Lives in / cadence |
|---|---|
| `MSR_FS_BASE` 0xc0000100 | `sregs.fs.base` (vcpu.c:1185, read-back 1433); per-task |
| `MSR_GS_BASE` 0xc0000101 | `sregs.gs.base` (vcpu.c:1186, read-back 1434); per-task |
| `MSR_KERNEL_GS_BASE` 0xc0000102 | **unmanaged** — D.1 trampoline doesn't use `%gs:` (vcpu.c:310-312) |

### 1e. Programmed MSRs

`kvm_v2_vcpu_program_msrs` (vcpu.c:323-392) writes via `KVM_SET_MSRS`
once per pool member at vCPU create; `KVM_GET_MSRS` readback verifies.

| MSR | Index | Value | Cadence |
|---|---|---|---|
| `MSR_LSTAR` | 0xc0000082 | `KVM_V2_LSTAR_GVA` (vcpu.c:331) | once at create |
| `MSR_STAR` | 0xc0000081 | `(0x18<<48)\|(0x08<<32)` (vcpu.c:332) | once at create |
| `MSR_SYSCALL_MASK` (FMASK) | 0xc0000084 | `0x47700` (vcpu.c:334) | once at create |
| `MSR_EFER` | 0xc0000080 | `EFER_SCE\|LME\|LMA\|NX` — install (vcpu.c:520) AND every dispatch via SREGS (vcpu.c:1307) | install + per-dispatch |

`KVM_SET_CPUID2` install gated by `vcpu->cpuid_primed` (deferred to
first KVM_RUN; backend.h:282-286).

### 1f. Debug registers (6)

`struct kvm_debugregs` (arch/x86/include/uapi/asm/kvm.h:377-383):
DR0–DR3, DR6, DR7. Ioctls `KVM_GET/SET_DEBUGREGS`. **Not used by v2** —
no callers in arch/um/backend/kvm-v2/. UML's
`arch_thread.debugregs[8]` (processor_64.h:25) is a separate ptrace
concept, unrelated.

### 1g. FPU/XSAVE state (28)

`struct kvm_fpu` (arch/x86/include/uapi/asm/kvm.h:175-187):
fpr[8][16], fcw, fsw, ftwx, last_opcode, last_ip, last_dp, xmm[16][16],
mxcsr. Ioctls `KVM_GET/SET_FPU`. AVX/AVX-512/XSAVE/XCRS masked at CPUID
by `kvm_v2_curate_cpuid` (processor_64.h:18-19); legacy 512 B suffices.

| Storage | Capture / install |
|---|---|
| `arch_thread.kvm_v2.fpu` + `.fpu_valid` (processor_64.h:30-31) | `kvm_v2_fpu_capture_for_fork` (vcpu.c:1898-1933, called from `arch_copy_thread` processor_64.h:138); `kvm_v2_fpu_capture_for_switch_out` (vcpu.c:1964); install via `kvm_v2_fpu_install_on_first_run` (vcpu.c:1589) |
| `arch_thread.kvm_v2.iotrap_fpu` + `.iotrap_fpu_valid` (processor_64.h:76-77) | post-KVM_RUN `KVM_GET_FPU` when FP ran (vcpu.c:1653-1655); pre-KVM_RUN `KVM_SET_FPU` (vcpu.c:1614-1617) — H.1b |

H.2 lazy FPU: `KVM_GET_FPU` skipped when post-exit `sregs.cr0 & TS`
indicates no FP instruction ran since the per-dispatch arming
(vcpu.c:1321).

### 1h. Descriptor tables (4 base+limit pairs)

`struct kvm_dtable`: arch/x86/include/uapi/asm/kvm.h:142-146.

| Table | base / limit | Owner |
|---|---|---|
| IDT | `KVM_V2_IDT_GVA` / 256×16-1 — `kvm_v2_install_descriptors_sregs` writes `sregs.idt` (vcpu.c:702); page at `vm->idt_kva` (backend.h:162) |
| GDT | `KVM_V2_GDT_GVA` / 8×8-1 — same helper; page at `vm->gdt_kva` (backend.h:166) |
| TR (TSS) | `vcpu->tss_gva` / 103 — per-vCPU; body at `vcpu->tss_kva` (backend.h:327) |
| LDTR | untouched — KVM-default unusable |

### 1i. Pending-event state (6)

`struct kvm_vcpu_events`: arch/x86/include/uapi/asm/kvm.h:340-374. At
`run->s.regs.events.*`. Sync bit `KVM_SYNC_X86_EVENTS`; fallback
`KVM_GET/SET_VCPU_EVENTS`. **Not used by v2** — `kvm_valid_regs` at
vcpu.c:924-925 sets only REGS|SREGS, NOT EVENTS.

| Slot | Use |
|---|---|
| `exception.{injected,nr,has_error_code,error_code}` | unmanaged |
| `interrupt.{injected,nr,soft,shadow}` | unmanaged (no APIC) |
| `nmi.{injected,pending,masked}` | unmanaged |
| `sipi_vector` | unmanaged |
| `smi.{smm,pending,...}` | unmanaged |
| `triple_fault.pending` | unmanaged |

`exception_payload` similarly unmanaged: v2 reads post-vmexit CR2
directly from `sregs.cr2` (the trampoline-stub design, backend.h:142-148
surfaces guest exceptions as `KVM_EXIT_IO`).

---

## Section 2 — Per-task state

`struct arch_thread.kvm_v2`: arch/x86/um/asm/processor_64.h:28-104.
Init at `INIT_ARCH_THREAD` (processor_64.h:109-118). `arch_flush_thread`
clears `fpu_valid` (processor_64.h:130). **Ownership rule:** every
field below is owned exclusively by the owning task; only `current` (or
`arch_copy_thread`'s parent→child copy) reads or writes them.

| Field | Width | Set when | Read when |
|---|---|---|---|
| `fpu` | `kvm_fpu` (~512 B) | fork-time (vcpu.c:1898-1933); switch-out (vcpu.c:1964) | first-run install (vcpu.c:1589) |
| `fpu_valid` | bool | true on capture; false on fail / `arch_flush_thread` | gates install |
| `ist_frame[6]` | 48 B | `kvm_v2_handle_io_pf` (syscall_trap.c:912-918); `kvm_v2_ist_frame_snapshot_raw` at EINTR-mid-stub (syscall_trap.c:967-978) | `kvm_v2_ist_frame_restore_pending` rewrites IST stack pre-KVM_RUN (syscall_trap.c:937-944) |
| `ist_pending` | bool | true on snapshot; false on consumption | gates restore (syscall_trap.c:934) |
| `iotrap_fpu` | ~512 B | post-KVM_RUN `KVM_GET_FPU` (vcpu.c:1653-1655) | pre-KVM_RUN `KVM_SET_FPU` (vcpu.c:1614-1617) — H.1b |
| `iotrap_fpu_valid` | bool | true on capture; false on consumption | gates install |
| `saved_cr2_at_eintr` | 8 B | EINTR-mid-IDT-delivery snapshot (#121; processor_64.h:79-103) | `load_user_sregs` re-installs into `sregs.cr2` (vcpu.c:1216-1218) |
| `saved_cr2_valid` | bool | true on snapshot; cleared on consumption (vcpu.c:1218) | gates restore |

---

## Section 3 — Per-vCPU state

`struct kvm_v2_vcpu`: arch/um/backend/kvm-v2/kvm_v2_backend.h:288-357.
Pool: static `vcpus[NR_CPUS]` (vcpu.c:1058); accessor
`kvm_v2_vcpu_get(cpu)` (backend.h:368). **Per-host-CPU, NOT per-task** —
every UML task on host CPU N shares vCPU N. `preempt_disable` around
the pick keeps it valid for one dispatch.

| Field | Width | Sharing | Owner / write |
|---|---|---|---|
| `vcpu_fd` | int | **shared** | set at create (vcpu.c:906) |
| `kvm_run` | ptr | **shared** — every task's regs travel through it | mmap'd at create (vcpu.c:889); per-dispatch r/w |
| `kvm_run_size` | u32 | shared | create (vcpu.c:908) |
| `cpu` | int | shared (= pool index) | create (vcpu.c:909) |
| `cpuid_primed` | bool | **shared** sticky one-shot | first KVM_RUN's CPUID install (backend.h:282-286) |
| `ist_stack_kva/gpa/top_gva` | ptr/phys/u64 | **shared physical IST page** — see Notable obs #1 | E.2 install (backend.h:421) |
| `tss_kva/gpa/gva` | ptr/phys/u64 | **shared per-vCPU TSS body** | E.2 (backend.h:323-329) |
| `kick_pending` | atomic_t | **shared**, cmpxchg dedup | set 0→1 by `kvm_v2_tlb_kick_others` (backend.h:336); reset by `load_user_sregs` (vcpu.c:1270) |
| `last_seen_tlb_gen` | atomic64_t | **shared** | snapshot per-dispatch from `mm->context.tlb_gen` (vcpu.c:1273-1274) |
| `current_mm` | `mm_struct *` | **shared** advisory | `WRITE_ONCE` per-dispatch (vcpu.c:1271); read by kicker for narrowing |

**No field on this struct is per-task.** The IST stack and TSS body
are physically shared by every UML task that picks this vCPU — that
sharing is exactly what `arch_thread.kvm_v2.ist_frame[]` snapshots
defend against (processor_64.h:32-58).

---

## Section 4 — Per-mm state (v2-relevant)

`mm_context_t`: arch/um/include/asm/mmu.h:16-81. Skipping `id`,
`turnstile`, `list`, `worker` (predate v2 / belong to seccomp).

| Field | Width | Lock | Purpose |
|---|---|---|---|
| `sync_tlb_lock` | spinlock | self | guards range below (mmu.h:23) |
| `sync_tlb_range_from`/`_to` | ulong | `sync_tlb_lock` | UML deferred-TLB range awaiting CR4.PGE flush at next dispatch (mmu.h:24-25) |
| `deferred_free_lock` | spinlock | self | mmu.h:60 |
| `deferred_free_pages` | list_head | `deferred_free_lock` | mmu_gather hand-off; pages held with extra ref until next vcpu_run drains them (mmu.h:39-58 — H.1b residual fix) |
| `deferred_free_count` | uint | `deferred_free_lock` | accounting |
| `tlb_gen` | atomic64_t | atomic | per-mm guest-TLB generation; bumped by `um_tlb_sync`, compared against `vcpu->last_seen_tlb_gen` for kick targeting (mmu.h:64-80) |

---

## Section 5 — Per-VM state

`struct kvm_v2_vm`: arch/um/backend/kvm-v2/kvm_v2_backend.h:49-168.
Single instance per UML invocation. Mutators take `vm->lock`.

| Field | Width | Owner / set by |
|---|---|---|
| `kvm_fd` | int | `kvm_v2_vm_create` open(/dev/kvm) (backend.h:50) |
| `vm_fd` | int | `KVM_CREATE_VM` (backend.h:51) |
| `caps` | u64 | negotiated KVM caps incl. SYNC_REGS bit 0 (backend.h:52, vcpu.c:921-922) |
| `cpuid` | `kvm_cpuid2 *` | curated buffer; install deferred to first KVM_RUN (backend.h:53) |
| `memslots` | list_head | Phase B per-region records (backend.h:54) |
| `memslot_bitmap[]` | bitmap | KVM_V2_MAX_USER_MEM_SLOTS bits (backend.h:38, 63) |
| `lock` | spinlock | guards memslots + bitmap (backend.h:64) |
| `trampoline_page`/`trampoline_gpa` | ptr/phys | LSTAR trampoline (D.1; backend.h:80-81) — `out + sysretq` 5 bytes at off 0x40 |
| `physmem_memslot_id` | int | identity-offset memslot at gpa=0/hva=uml_physmem (D.4b-pre; backend.h:99) |
| `trampoline_pud_kva`/`_pmd_kva`/`_pte_kva` | ptr×3 | PT chain for kernel-half PML4[448] (D.4b; backend.h:123-125) |
| `trampoline_pud_gpa` | phys | written into swapper_pg_dir[448] + init_mm.pgd[448] (backend.h:126) |
| `idt_kva`/`idt_gpa` | ptr/phys | 256×16 IDT page (E.1; backend.h:162-163) |
| `handlers_kva`/`handlers_gpa` | ptr/phys | exception-handler stub page (E.1; backend.h:164-165) |
| `gdt_kva`/`gdt_gpa` | ptr/phys | 8×8 GDT page (E.1; backend.h:166-167) |

Per-region records: `struct kvm_v2_memslot` (backend.h:179-186) =
`{gpa, host_va, size, slot_id, flags}`, linked off `vm->memslots`.

---

## Notable observations

1. **No field on `struct kvm_v2_vcpu` is per-task.** "Per-vCPU" IST
   stack (backend.h:324) and TSS body (backend.h:327) are physically
   *shared* by every UML task picking this pool entry. The only
   cross-task isolation is the per-task `ist_frame[]` snapshot/restore
   dance. A window between snapshot and the next task's restore =
   cross-task corruption (the failure class #121 closed via the
   EINTR-mid-IDT-delivery snapshot at vcpu.c:1216-1218 +
   backend.h:514-533).

2. **`KVM_SYNC_X86_EVENTS` is NOT enabled.** vcpu.c:924-925 sets only
   REGS|SREGS. Pending exceptions / interrupts / NMI / SIPI / SMI
   never round-trip through the mmap. v2 surfaces guest exceptions as
   `KVM_EXIT_IO` via the trampoline-stub (backend.h:142-148). Any
   future need to inject an exception requires wiring EVENTS.

3. **CR8 unmanaged** — KVM-default 0; no APIC.

4. **`MSR_KERNEL_GS_BASE` unmanaged** — explicit (vcpu.c:310-312).
   New MSR if E.x ever needs per-vCPU GS.

5. **DR0–DR7 unmanaged** — `arch_thread.debugregs[8]` is the ptrace
   path. v2 issues no `KVM_GET/SET_DEBUGREGS`. Guest hw breakpoints
   would not work today.

6. **EFER double-written** — install (vcpu.c:520) AND every dispatch
   (vcpu.c:1307). Per-dispatch is redundant; vcpu.c:1296-1303 defers
   hoisting to Phase H.

7. **CR2 has three potential per-dispatch writers**: (a) per-dispatch
   zeroing (vcpu.c:1220), (b) `saved_cr2_at_eintr` restore
   (vcpu.c:1217), (c) `KVM_SYNC_X86_SREGS` round-trip from the prior
   exit. Interaction was the #121-D15 bug; current code reads "zero
   unless `saved_cr2_valid`" but silently relies on no other path
   leaving `sregs.cr2` non-zero between exit and entry.

8. **`vcpu->current_mm` is plain pointer, not RCU.** backend.h:351-356
   notes reads in the kicker are advisory (stale read = unneeded IPI,
   bounded by `kick_pending` dedup). If the kicker ever needs the
   mm's *contents* (not identity comparison), this breaks.

9. **`vcpu->cpuid_primed` is shared sticky state.** First task on a
   vCPU triggers the install; subsequent tasks see the curated CPUID.
   Correct under v2's single-VM model; needs rethinking for per-task
   vCPUs.

10. **`mm->context.tlb_gen` per-mm, `vcpu->last_seen_tlb_gen`
    per-vCPU.** A migrating task dispatches on different vCPUs each
    with its own `last_seen` (desired — each vCPU's hw TLB needs its
    own flush). Kicker narrows on (mm match AND gen-stale).

11. **`ist_frame[]` and `iotrap_fpu` are per-task snapshots of state
    on per-vCPU shared pages.** Snapshot/restore must bracket every
    KVM_RUN exit/entry pair. #121 closed the missed gap (EINTR
    mid-IDT-delivery, before the stub captured CR2).
