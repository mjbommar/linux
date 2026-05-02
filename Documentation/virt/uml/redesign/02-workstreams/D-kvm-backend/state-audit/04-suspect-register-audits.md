# UML KVM v2 — Per-Suspect-Register Deep Audit (RBP / RAX / RDX / FS_BASE)

**Created:** 2026-05-01
**Tip at audit:** `e5f231656ead`
**Scope:** every R/W site in `arch/um/backend/kvm-v2/` (+ the UML
kernel hooks that feed `gp[]`) for the four registers mt-mini-diag
shows corrupted under T=8/ncpus=4 (~50% PASS).

## Pre-audit invariants

- **Per-task `regs`.** `kvm_v2_vcpu_run` is always called with
  `&current->thread.regs.regs` (the only `userspace()` callers,
  `arch/um/kernel/process.c:129,:146`, pass that). `uml_pt_regs` lives
  per-task, so `gp[]` is **not** a cross-task shared structure.
- **Whole-dispatch preempt_disable.** `kvm_v2_vcpu_run` holds
  `preempt_disable` from vcpu.c:1483 to vcpu.c:1788 (EINTR exit) /
  vcpu.c:1870 (normal exit). `current` cannot change inside one
  dispatch.
- **Per-host-CPU vCPU pool.** `vcpus[NR_CPUS]` (vcpu.c:92) means
  `vcpu->kvm_run` (mmap) and `vcpu->ist_stack_kva` are **per host
  CPU**. Two tasks on the same CPU share them *serially*
  (preempt_disable forbids overlap); different CPUs each have their
  own copies.
- **Per-task EINTR snapshot.** vcpu.c:1693-1696 stack-snapshots
  `kvm_run->s.regs.{regs,sregs}` BEFORE `unblock_signals()`, defending
  against the live mmap being overwritten by a context-switched
  task on the same CPU. Marshal-out at vcpu.c:1814-1815 reads from
  the snapshot, never the live mmap.
- **HOST_* indices** (from `arch/x86/um/user-offsets.c:41-68`):
  HOST_BP=4, HOST_AX=10, HOST_DX=12, HOST_DI=14, HOST_IP=16, HOST_SP=19,
  HOST_FS_BASE=21, HOST_GS_BASE=22.
- **Guest CPUID curated.** FSGSBASE/AVX/XSAVE all masked
  (vcpu.c:128-167). Guest cannot `wrfsbase` / `wrgsbase` — FS/GS
  base only changes via `arch_prctl`.

The audits below assume these invariants and only flag races the
invariants do not already kill.

---

## Register: RBP / HOST_BP (gp[4])

### Lifecycle in one dispatch

1. **User-mode at fault:** USER_RBP in vCPU register file (VMCB.save.rbp
   on SVM / `vcpu->arch.regs[VCPU_REGS_RBP]` on VMX).
2. **SYSCALL / IDT delivery:** RBP is **NOT pushed** by hardware.
   The LSTAR trampoline (`out + sysretq`, syscall_trap.c:137-140) and
   IDT stubs (e.g. PF stub at exception.c:213-227) do not touch RBP.
   So at vmexit RBP still equals USER_RBP.
3. **Post-vmexit:** KVM stores via SYNC_REGS to `run->s.regs.regs.rbp`.
4. **Snapshot + marshal_from:** `eintr_regs.rbp` taken at vcpu.c:1694;
   marshal_from (vcpu.c:1384) writes `gp[HOST_BP] = src->rbp` into
   `current->thread.regs.regs.gp[4]`.
5. **handle_syscall path:** `gp[HOST_BP]` is **never read or written**
   by handle_syscall (skas/syscall.c:19) — none of `UPT_SYSCALL_ARG1..6`
   alias RBP (sysdep/ptrace_64.h:51-56). RBP survives unchanged.
6. **marshal_to (vcpu.c:1350):** `dst->rbp = gp[HOST_BP]` writes back
   into the per-CPU mmap; KVM_SYNC_X86_REGS dirty bit set.
7. **SYSRETQ / iretq:** does not pop RBP. KVM-restored register file
   value (= our value) becomes user RBP.

### Read sites

| File:line | Context | Race-free? |
|---|---|---|
| vcpu.c:1350 (`marshal_to_kvm_regs`) | preempt_disable; per-task gp[]. | YES |
| syscall_trap.c:1128 (LOW-PF diag print) | preempt_disable; read-only. | YES |
| test_marshal.c | KUnit. | n/a |

### Write sites

| File:line | Context | Race-free? |
|---|---|---|
| vcpu.c:1384 (`marshal_from_kvm_regs`) | source = stack-local `eintr_regs` snapshot taken before unblock_signals. | YES |
| (no other writers in v2 backend or arch/x86/um/) | | |

### Conclusion — RBP

**No v2-backend site can write `gp[HOST_BP]` from another task's
data.** The only writer is `marshal_from_kvm_regs` from a stack-
local snapshot under preempt_disable.

The mt-mini-diag capture `RBP=0x440371e0 (tid-4 stack)` therefore
does **not** come from a v2 marshal race. Three remaining hypotheses:

- **(A) Cross-vCPU host-side leak via `vcpu->arch.regs[]`.** Walked
  through: task-1 leaves CPU-A, gets scheduled to CPU-B, task-4
  ran on CPU-B in between writing CPU-B's mmap. When task-1 lands
  on CPU-B, task-1's marshal_to overwrites task-4's rbp before
  KVM_RUN. **Safe.**
- **(B) Snapshot races a second vmexit on same vCPU.** Impossible:
  preempt_disable forbids another KVM_RUN before our preempt_enable.
- **(C) Stale-TLB guest-side load.** The user code did `mov 8(%rsp),
  %rbp` (function epilogue), and the *guest TLB* mapped that stack
  page to the wrong physmem PFN — RBP is loaded with another task's
  stack content. **PLAUSIBLE.** CR4.PGE-toggle flush at vcpu.c:1259
  covers the dispatching vCPU; cross-vCPU `kvm_v2_tlb_kick_others`
  (declared kvm_v2_backend.h:543) is the remote-flush plumbing — if
  its dedup or narrowing skips a vCPU that needed the flush, the
  wrong user data lands in RBP.

**Instrumentation:** add tracepoints in `marshal_from`/`marshal_to`
emitting `(pid, cpu, gp[BP])`. If sequence shows
`enter cpu=A pid=1 BP=0x42xxx; exit cpu=A pid=1 BP=0x440xxx` with
no intervening pid-1 entry, corruption is guest-side (C). If
marshal_to writes the wrong value, bug is in v2.

---

## Register: RAX / HOST_AX (gp[10])

The most touched register: SYSCALL NR on entry, return value on
exit, push/pop'd inside the in-guest #PF stub.

### Lifecycle (syscall path)

1. **User SYSCALL:** RAX = syscall_nr.
2. **Hardware SYSCALL:** RAX unchanged. CPU sets RIP=MSR_LSTAR,
   RCX=user_RIP, R11=user_RFLAGS.
3. **LSTAR trampoline runs at CPL=0** (`out %al, $0xf4 ; sysretq`,
   syscall_trap.c:137-140). `out %al` reads `%al` to bus — KVM
   discards the value (dispatch is by `run->io.port`, not `data`).
   RAX in live register file unchanged. vmexit.
4. **Post-vmexit:** `run->s.regs.regs.rax = syscall_nr`. Snapshot
   + marshal_from puts it in `gp[HOST_AX]`.
5. **kvm_v2_handle_io_trap (syscall_trap.c:1542):** reads
   `syscall_nr = regs->gp[HOST_AX]`, stashes to PT_SYSCALL_NR
   (= gp[15]). gp[HOST_AX] still holds NR.
6. **handle_syscall (skas/syscall.c):**
   - line 26: `PT_REGS_SET_SYSCALL_RETURN(regs, -ENOSYS)` →
     `gp[HOST_AX] = -ENOSYS` (via arch/x86/um/asm/ptrace.h:46).
   - line 82: `PT_REGS_SET_SYSCALL_RETURN(regs, ret)` →
     `gp[HOST_AX] = ret`.
7. **marshal_to (syscall_trap.c:1693):** copies `gp[HOST_AX]` →
   `run->s.regs.regs.rax`. SYNC dirty bit set.
8. **Next KVM_RUN:** SYNC_REGS loads, SYSRETQ drops to CPL=3 with
   `RAX = ret`.

### Read sites

| File:line | Context | Race-free? |
|---|---|---|
| vcpu.c:1344 (`marshal_to_kvm_regs`) | preempt_disable, per-task. | YES |
| syscall_trap.c:1542 (`syscall_nr = regs->gp[HOST_AX]`) | preempt_disable. | YES |
| syscall_trap.c:1131,:1699 (LOW-PF diag, trace event) | read-only. | YES |

### Write sites

| File:line | Context | Race-free? |
|---|---|---|
| vcpu.c:1378 (`marshal_from_kvm_regs`) | snapshot source. | YES |
| skas/syscall.c:26,:82 (handle_syscall PT_REGS_SET_SYSCALL_RETURN) | preempt_disable, per-task. | YES |
| syscall_trap.c:1693 (`marshal_to_kvm_regs(&run->s.regs.regs, regs)`) | preempt_disable, writes per-vCPU mmap; safe under whole-dispatch preempt. | YES |

### Conclusion — RAX

The 4 sites around `gp[HOST_AX]` are race-free. The "RAX=0 = MMAP_NULL"
mt-mini signature has no v2 race that can produce it because:

- **(A) No backend site zeroes `gp[HOST_AX]` after handle_syscall.**
  `rg 'gp\[HOST_AX\]'` confirms only the 4 sites above touch it.
- **(B) Wrong `regs` pointer.** `regs` flows from
  `userspace(&current->thread.regs.regs)` under preempt_disable;
  current is pinned. Safe.
- **(C) `run->s.regs.regs.rax` clobber post-marshal-to.** Inside
  preempt_disable nothing else can run KVM_RUN on the same vCPU.
  Safe.
- **(D) #PF stub clobbers RAX.** Stub does
  `push %rax ; mov %cr2, %rax ; ... ; pop %rax` (exception.c:213-227)
  — restores RAX before `out`. Safe.
- **(E) EINTR mid-IDT-delivery race.** vcpu.c:1761-1776 detects RIP
  in PF stub range `[HANDLERS_GVA+0x140, +0x180)` and bypasses via
  `kvm_v2_handle_pf_eintr_inline`, which does not modify RAX. Safe
  for the PF stub case. **Window:** an EINTR caught one byte AFTER
  the stub's first `push %rax` completed but before `mov %cr2,%rax`
  — the bypass would still trigger (RIP still in range), the
  inline handler would run on the IST frame the partial stub left,
  and on resume the original stub never re-runs. Inline handler
  rewrites IST frame for a clean iretq, marshals back regs, and the
  next KVM_RUN starts at the user_rip from the IST frame. RAX in
  the live register file may be a transient value from `mov %cr2,
  %rax` — but `marshal_from_kvm_regs` ran on `eintr_regs` (the
  vmexit snapshot), then segv_handler+interrupt_end wrote RIP/RSP
  back, then marshal_to re-shipped `gp[HOST_AX]` (which still holds
  the user's pre-fault RAX from when KVM_RUN began). **Safe** —
  marshal_to ships our gp[HOST_AX], not the transient register file.

**Most likely "RAX=0" mechanism:** symptom-side. mt-byteset's
verifier `got=0 expect=1` reports the value READ from
`*p` (not the syscall return). The user's `mov (%rdx), %al`
hit a stack-page-with-stale-TLB → loaded byte 0 from a page that
should contain 1. **Not a v2-backend race.**

**Instrumentation:** `pr_emerg` in `marshal_from_kvm_regs` if
`src->rax == 0 && PT_SYSCALL_NR(gp) == __NR_mmap`. If never fires
under failure, RAX=0 is downstream (symptom-side write).

---

## Register: RDX / HOST_DX (gp[12])

Used by mt-mini's `slow_memset` as `mov %al, (%rdx)` store
destination. Also `UPT_SYSCALL_ARG3`. The LOW-PF symptom shows
`gp[DX]=0x42011000`.

### Lifecycle

Identical to RBP at the marshaler level (vcpu.c:1347 read,
vcpu.c:1381 write). Differences:

- **In-guest #PF stub stashes RDX** at exception.c:222
  (`mov %rdx, -16(%rsp)` → IST page byte `PAGE_SIZE - 72`). Stub
  never overwrites RDX in the live register file. Post-vmexit
  `run->s.regs.regs.rdx` = USER_RDX (the value the faulting
  `mov %al, (%rdx)` was using).
- **`captured_rdx` read at syscall_trap.c:1090** consumes that stash;
  used by the cr2-rescue heuristic at syscall_trap.c:1152-1155
  (write fault + cr2=0 + captured_rdx > 0x10000 → cr2 = captured_rdx).

### Read sites

| File:line | Context | Race-free? |
|---|---|---|
| vcpu.c:1347 (`marshal_to_kvm_regs`) | preempt_disable. | YES |
| syscall_trap.c:1090 (`captured_rdx` from IST page) | preempt_disable; IST page exclusive to this dispatch on this CPU. | YES |
| syscall_trap.c:1102,:1127,:1130 (LOW-PF diag), :1152-1155 (cr2-rescue) | read-only / preempt_disable. | YES |

### Write sites

| File:line | Context | Race-free? |
|---|---|---|
| vcpu.c:1381 (`marshal_from_kvm_regs`) | snapshot source. | YES |
| (no other backend writers) | | |

### Conclusion — RDX

No v2 race writes `gp[HOST_DX]` from another task's data. The IST-
page stub stash (`captured_rdx`) is per-CPU and serialized by
preempt_disable: even though task-1 and task-4 share `vcpu->ist_stack
_kva`, they push frames serially; whichever task is reading the
slot in handle_io_pf is the one whose stub just ran.

The LOW-PF `gp[DX]=0x42011000` reflects what the **guest CPU's RDX
register actually held at vmexit**. Two ways the guest RDX can be
wrong:

- **(C-DX) Stale guest TLB on a CoW page.** User code `mov 16(%rsp),
  %rdx` from a memory location whose physmem PFN was recycled, but
  guest TLB cached old GVA→PFN. PLAUSIBLE — same root cause as RBP-(C).
- **(C-DX-2) Cross-mm TLB issues.** mt-mini is one process / one mm.
  Doesn't apply.

**Sentinel diagnostic already wired:** exception.c:218 writes
`0xffffffffffffffff` to `ist_top - 80` before stub captures CR2/RDX
— so a `captured_rdx` that matches the sentinel means stub never
ran for this dispatch. Cross-check existing trace output for
sentinel-vs-captured agreement.

---

## Register: FS_BASE / HOST_FS_BASE (gp[21])

Backs glibc TLS (`fs:[40] = __stack_chk_guard`). Wrong FS_BASE
explains canary-corruption SIGSEGV.

### Lifecycle

1. **User:** guest MSR_FS_BASE = USER_FS_BASE (set via `arch_prctl(ARCH
   _SET_FS, ...)`).
2. **arch_prctl(ARCH_SET_FS):** `arch/x86/um/syscalls_64.c:23` writes
   `current->thread.regs.regs.gp[FS_BASE/8] = arg2`. **Only writer
   in `arch/x86/um/`.**
3. **Next vcpu_run:** vcpu.c:1578-1581 calls
   `kvm_v2_load_user_sregs(vcpu, __pa(active_mm->pgd), gp[HOST_FS_BASE],
   gp[HOST_GS_BASE])`. Helper (vcpu.c:1185-1186) writes `sregs->fs.base
   = fs_base; sregs->gs.base = gs_base;` to `vcpu->kvm_run->s.regs
   .sregs`, ORs KVM_SYNC_X86_SREGS at vcpu.c:1323.
4. **KVM_RUN entry:** SYNC consumes sregs, programs `vcpu->arch.fs.base`
   → vmenter loads MSR_FS_BASE.
5. **vmexit:** KVM stores `run->s.regs.sregs.fs.base = vcpu->arch.fs.base`.
   Since guest can't `wrfsbase` (CPUID-masked), this equals what we
   loaded.
6. **marshal_sregs_back (vcpu.c:1430-1434):** reads from snapshot
   `eintr_sregs.fs.base`, writes `dst->gp[HOST_FS_BASE] = src->fs.base`.
7. Loop closes: per-task `gp[HOST_FS_BASE]` is the truth.

### Read sites

| File:line | Context | Race-free? |
|---|---|---|
| vcpu.c:1580 (load_user_sregs arg) | preempt_disable, per-task. | YES |
| arch/x86/um/syscalls_64.c:33 (ARCH_GET_FS) | inside handle_syscall; per-task gp[]. | YES |

### Write sites

| File:line | Context | Race-free? |
|---|---|---|
| arch/x86/um/syscalls_64.c:23 (`gp[FS_BASE/8] = arg2`) | inside handle_syscall, preempt_disable. | YES |
| vcpu.c:1185 (`sregs->fs.base = fs_base` in mmap) | per-CPU mmap, exclusive this dispatch. | YES |
| vcpu.c:1433 (`marshal_sregs_back`: `gp[HOST_FS_BASE] = src->fs.base`) | source = `eintr_sregs` snapshot. | YES |

### Conclusion — FS_BASE

All marshalers are race-free under preempt_disable + snapshot. Three
subtleties checked:

1. **Cross-task sregs.fs.base clobber on shared per-CPU mmap.** Each
   dispatch re-writes the field in load_user_sregs BEFORE KVM_RUN.
   Safe.
2. **marshal_sregs_back races SIGALRM-driven re-entry.** Snapshot
   was taken before unblock_signals; even if another task reaches
   KVM_RUN on this CPU during the gap, our `eintr_sregs` is stack-
   local. Safe.
3. **vcpu.c:1814-1815 overwrites `gp[HOST_FS_BASE]` BEFORE the
   syscall dispatcher runs.** Sequence: marshal_from_kvm_regs (1814)
   → marshal_sregs_back (1815) → switch(exit_reason) → handle_io_trap
   → handle_syscall → arch_prctl(SET_FS, B) → `gp[HOST_FS_BASE] = B`.
   The 1815 overwrite copies `eintr_sregs.fs.base` (= the value KVM
   loaded on entry, unchanged because guest can't wrfsbase) into
   gp[HOST_FS_BASE] — it's a no-op. Then arch_prctl writes B. Next
   dispatch ships B. Safe.

**The "FS_BASE wrong" symptom is most likely misattribution.** vcpu.c:
1414 documents that `fs.base = 0x0 throughout the run` for fork-tree-
3level (static-glibc binaries don't issue arch_prctl(SET_FS) — canary
lives at globally-mapped __stack_chk_guard, not a TLS slot). If
mt-mini's glibc is similar, FS_BASE is constant 0; the canary
corruption is a write into the globally-mapped guard's physmem
PFN — not a FS_BASE marshaling bug.

**Instrumentation:** add `pr_emerg` in load_user_sregs that fires
when `fs_base != current->thread.arch.kvm_v2.last_seen_fs` (would
need to add `last_seen_fs`). If never fires under failure, FS_BASE
never changes and the bug is downstream.

---

## Cross-register conclusion

For all four registers, the v2 marshal sites are serializable under
preempt_disable + per-task gp[]. There is **no direct cross-task
write** into `gp[HOST_BP/AX/DX/FS_BASE]` that could leak another
task's value.

The mt-mini-diag captures (RBP=tid-4-stack, RAX=0, RDX=low,
RIP-in-data-page) are most consistent with **guest-side memory or
TLB corruption** delivering wrong DATA to the guest's load
instructions, not a marshal-out race.

The single largest remaining risk surface — and the one that would
explain ALL four corruptions simultaneously — is:

**Guest TLB staleness on a stack page after CoW or freelist
recycling, on a vCPU that did NOT receive a proper TLB flush.**

The CR4.PGE-toggle flush at vcpu.c:1259 runs on the **dispatching**
vCPU; UML's per-mm TLB invalidations originate elsewhere
(mremap/mprotect/munmap walks). `kvm_v2_tlb_kick_others` (declared
kvm_v2_backend.h:543) is the remote-flush plumbing. A bug there —
the kick missing a vCPU running the same mm, or the kick arriving
but the cmpxchg dedup (`vcpu->kick_pending`, kvm_v2_backend.h:336)
skipping the flush — would leave one vCPU with stale TLB. Guest
loads then land on the wrong physmem PFN, and ALL FOUR register
corruptions appear in a single fault — exactly mt-mini's
signature.

**Top-priority next step:** audit `kvm_v2_tlb_kick_others`
(definition not yet read in this audit) plus the
`vcpu->kick_pending` / `last_seen_tlb_gen` / `current_mm` fields'
R/W ordering across `um_tlb_sync`, `load_user_sregs` (vcpu.c:1270-
1275), and the IPI handler. Treat `mm->context.tlb_gen` as the
central state item in Layer 2's operations inventory and trace
every reader/writer end-to-end.

The vcpu.c:1244-1245 v1-archive comment is decisive: "narrowing the
toggle to tlb_stale && same_cr3 regressed v1's gate to ~70% pass
rate vs 100%" — exactly mt-mini's failure rate under v2.
