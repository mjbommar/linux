# D-04: vCPU bring-up + ring transitions (kernel ↔ user)

**Status:** **D-04a + D-04b architecturally complete**
(2026-04-23). Backend boots under `backend=kvm` through
start_kernel, vCPU executes UML's own kernel binary via the
guest MMU, VMEXIT round-trip measured at-floor on two
microarchitectures (22252 cyc median on Skylake-W Xeon
W-2123, 5568 cyc boost-locked on Alder Lake i7-12700K).

**Commit trail:**
  - D-04a   (vCPU + KVM_RUN skeleton)       — 340eaae2bdc9
  - D-04b.0 (defer memslot, EINVAL fix)     — 153017d8eeda
  - D-04b.1a (SREGS helpers)                — 06d1cb4063c2
  - D-04b.1b (harness wiring)               — ebdfcb5d0fcc
  - stacktrace walker bound fix             — f68398447394
  - harness os_info visibility              — d46f5227b845
  - D-04b.1c (per-iter cycle counts)        — c461e178da68
  - D-04b.2a (paging-range helper)          — 9c3ce166f667
  - D-04b.2a harness 1 GiB coverage         — 62287f311f60
  - D-04b.2b.alt (arbitrary-RIP validation) — bc97d1692c10
  - D-05a (real time ops — unblocker)       — aa8789bc4311
  - D-04b.2b.1 (late_initcall relocation)   — 0fb063d1ad2e
  - D-04b.2b.2 (UML-kernel-text execution)  — 3518ff9c562b

Remaining for the naive-backend phase-1:
  - D-04c: LSTAR trampoline + SYSCALL dispatch. Reuses
    spike 07's ~270-cyc SYSCALL+SYSRETQ pattern. Demonstrates
    guest-user → ring-0 → sys_call_table → resume round-trip.
  - D-05 full: signal delivery + IPI. Beyond D-05a's time ops.
  - D-06: conformance + KGDB integration.

**Effort:** 4–5 weeks (widest D task with D-03 at ~6 weeks);
.2 + .c are the remaining pieces.
**Dependencies:** D-02 ✓ (64aa06142e8f), D-03 ✓
             (eefc8fc50acc).
**Blocks:** D-05, D-06.

## Goal

Make `backend=kvm` actually run a guest. That decomposes into
three independent pieces, each landable on its own but all
needed for the first syscall round-trip:

  1. vCPU creation + KVM_RUN loop (D-04a). `run_userspace` goes
     from pr_warn_once stub to an entry into KVM_RUN with a
     minimal exit-reason switch. Targets: `backend=kvm` under
     sudo stops pre-console-panicking; first KVM_RUN call
     returns (likely with KVM_EXIT_SHUTDOWN until SREGS is
     set up) without crashing the UML kernel.
  2. Long-mode SREGS + CR3 (D-04b). Initialize the vCPU's
     CR0/CR3/CR4/EFER + GDT/IDT so ring-0 execution inside
     the guest is legal. CR3 points at UML's current
     `mm->pgd` (translated via the Policy A identity
     mapping). Target: KVM_RUN enters long-mode ring-0 at
     the UML kernel's code, executes a HLT, returns
     KVM_EXIT_HLT cleanly.
  3. LSTAR trampoline + SYSCALL dispatch (D-04c). Populate
     MSR_LSTAR with the address of a ring-0 "bounce"
     trampoline (~30 bytes of assembled bytes, pattern from
     spike 04/07) that VMCALLs on entry. The VMEXIT reason
     becomes KVM_EXIT_HYPERCALL; the host-side loop calls
     `sys_call_table[rax]` on the cached regs and resumes.
     Target: the D-workstream bookend — first guest
     `getpid()` through KVM with measured round-trip,
     compared against the spike-04/07 bare-metal floor in
     measurements.md.

## Shape per the design memo

Per `design-memo.md` §"Concrete shape":

  - One `kvm_vcpu` per UML CPU. SMP UML has `ncpus` vCPUs
    sharing one `vm_fd`.
  - Per-vCPU state: fd, mmap'd `kvm_run` struct, cached
    `pt_regs`, dirty bit.
  - Entry: host thread attaches to vCPU, calls KVM_RUN,
    dispatches on `kvm_run->exit_reason`.
  - Exit: syscall_hook → sys_call_table[rax] → write result
    → KVM_RUN.

See `design-memo.md` §"Sequence: guest syscall" for the t0..t13
trace this implements; and `design-memo.md` §"The bounce
trampoline" for the LSTAR/vmcall mechanism.

## Approach (by sub-step)

### D-04a — vCPU creation + KVM_RUN skeleton

Minimal sub-step that can land without the ring-0 guest code
executing anything. Enough to verify the host-side plumbing:

  - Extend `struct kvm_um` (or add a sibling struct) to hold
    a vCPU array, each entry with `vcpu_fd`,
    `struct kvm_run *run`, `size_t run_size`.
  - `thread_start_idle(cpu_id)` or equivalent: call
    KVM_CREATE_VCPU, KVM_GET_VCPU_MMAP_SIZE, mmap the shared
    state page. For ncpus=1 UML this happens once.
  - `run_userspace()` becomes: ioctl(KVM_RUN) on the current
    CPU's vcpu_fd, then a switch on exit_reason that panics
    on anything other than the expected stub-stage exits
    (KVM_EXIT_SHUTDOWN because SREGS is zero; explicitly
    documented as intentional).
  - Test: sudo+backend=kvm reaches the panic(KVM_EXIT_...) or
    the run_userspace stub's guarded exit without killing
    UML's printk console. Matters because it's the first
    time backend=kvm has a surviving error message.

Deliverable: ~200 LOC split across `arch/um/backend/kvm/thread.c`
(new) and updates to `arch/um/backend/kvm/stubs.c` (remove
`run_userspace`).

### D-04b — long-mode SREGS + CR3

  - Populate CR0 (PE+PG+NE+AM+WP), CR4 (PAE+OSFXSR),
    EFER (LME+LMA+SCE), GDT with ring-0 and ring-3 segments,
    IDT.
  - CR3 ← UML's current `mm->pgd`, translated through the
    memslot identity map (guest_phys = host-VA).
  - Initial RIP = the address of a HLT-terminated
    "hello world" function in UML kernel memory, or the
    entrypoint of the idle loop. First goal is KVM_EXIT_HLT,
    not any real execution.
  - Spike 04's long-mode bring-up sequence
    (`spikes/04-long-mode-vmcall/`) is the reference —
    port its GDT/IDT/SREGS setup into
    `arch/um/backend/kvm/long_mode.c`.

Deliverable: ~200 LOC + a spike-derived assembly trampoline.

### D-04c — LSTAR + SYSCALL dispatch

  - MSR_LSTAR ← address of a bounce trampoline that VMCALLs.
  - Pattern from spike 07's variant-B (the ~270-cycle
    silicon-invariant floor established under P-state lock).
  - VMEXIT handler: read vCPU regs (via mmap'd kvm_run,
    not KVM_GET_REGS — save ~200 ns per the design memo
    §"Sequence: guest syscall" open question), invoke
    `sys_call_table[rax]`, write result back, resume.

Deliverable: ~300 LOC including trampoline bytes + exit
dispatcher; brings the bookend target into reach.

## Validation

Cumulative across the three sub-steps:

  - D-04a: first KVM_RUN returns without crashing the UML
    kernel. Observable via `backend=kvm` boot not losing
    printk output. Manual test + (if we can arrange it)
    a CI boot-smoke under sudo on a /dev/kvm-capable runner.
  - D-04b: `kvm_stat` shows KVM_EXIT_HLT on a guarded HLT
    placed in UML's init path. Same pattern as the D-02
    design-memo validation gate.
  - D-04c: first guest `getpid()` measurable. Log entry in
    `measurements.md` per spike-07 P-state-locked
    methodology, compared against the ~270-cycle floor
    Spike 07 validated.

A-05 conformance (20 KUnit tests) runs under `backend=kvm`
once D-04c is stable — that's the D-06 gate, not D-04.

## Not in D-04 scope

Per design-memo.md §"Not in first-phase scope":

  - IPI between vCPUs (D-05; requires vCPU scheduling +
    inter-thread signal path).
  - Signal delivery to guest userspace (D-05).
  - Timer / interrupt injection from host clock (D-05).
  - KGDB register access (D-06).

## Open questions (resolve in code review)

  1. Where exactly does D-04a's vCPU creation live — under
    `thread_start_idle` (per UML CPU idle bring-up) or a
    new init-time hook after KVM_CREATE_VM? The design memo
    says "attach to the idle thread" but the scaffolding
    may prefer eager creation for the ncpus=1 common case.
  2. How to surface early-boot panics readably. Pre-console-
    panic issue from D-03a still applies; D-04a should at
    minimum mmap a pre-console log target so the "KVM
    backend failed to create vCPU" class of error is
    debuggable without a core dump.
  3. CR3 translation arithmetic. Memo says guest-phys =
    host-VA via an offset; we're using offset=0 (Policy A
    identity), so `CR3 = __pa(mm->pgd)` but `__pa()` is
    defined against UML's notion of physical which differs
    from the guest-physical we need. Spelled out in D-04b's
    design note (TBW).
  4. vCPU thread affinity. Each vCPU should run on its
    own host thread. UML's existing per-CPU kernel thread
    is the natural host for this — no new thread machinery
    needed, just attach vCPU to that thread via
    KVM_SET_TSS_ADDR or equivalent.

## Risk

Highest execution risk in the D workstream. Ring-0 ASM stubs
cause silent data corruption on bugs. Mitigations:

  - Land in small sub-steps that each compile + boot-smoke
    cleanly (A-04a's "first KVM_RUN returns" is a concrete
    checkpoint).
  - Spike 04 + 07 already prototype the hard bits; port with
    minimal divergence.
  - Test matrix: gcc + clang, all three dispatch modes,
    KVM=y with `backend=seccomp` (fall-through) + explicit
    `backend=kvm` under sudo.

## References

- `design-memo.md` — scope + sequence diagrams.
- `03-page-table-mgmt.md` — D-03 mm-ops landing; feeds the
  memslot Policy A that D-04's CR3 arithmetic depends on.
- `03b-memslot-policy.md` — the one-giant-memslot decision.
- `spikes/04-long-mode-vmcall/` — long-mode bring-up
  sequence (GDT+PAE+EFER).
- `spikes/07-syscall-lstar-ab/` — ~270 cyc SYSCALL+SYSRETQ
  floor; the D-04c reference.
- `measurements.md` — timing-log home for D-04 results.
- decisions-log D55, D56 — the vision-line update informing
  D-04's perf targets.
