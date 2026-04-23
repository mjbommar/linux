# D-04b design note: long-mode SREGS + fixed memslot

**Status:** landed (2026-04-22) — shipped as the D-04b.1
           series: long-mode SREGS helpers in D-04b.1a
           (`06d1cb4063c2`), harness wiring in D-04b.1b
           (`ebdfcb5d0fcc`) first KVM-exit on a handcrafted
           2 MiB identity-mapped slab, per-iteration cycle
           accounting in D-04b.1c (`c461e178da68`). D-04b.1c
           measurements on both hosts (Skylake-W + Alder Lake
           w1) sit at the spike-floor per
           `measurements.md`. The "CR3 over UML's own VA"
           half of the original scope moved to `04b2-uml-cr3.md`
           after examination showed UML's `init_mm.pgd` isn't
           a hardware-valid tree.
**Follows:** D-04a (vCPU + KVM_RUN skeleton) ✓ 340eaae2bdc9
**Companion to:** `04-ring-transition.md` (D-04 sub-step
           breakdown), `03b-memslot-policy.md` (the memslot
           policy D-03c shipped), `04b2-uml-cr3.md` (the
           successor scope covering D-04b.2).

D-04a left two known-broken pieces on the floor that D-04b
needs to land together:

  1. `KVM_SET_USER_MEMORY_REGION` returned `-EINVAL` on the
     D-03c "one giant slot at init" parameters
     (`userspace_addr=0`, `memory_size=task_size`). The
     registration was downgraded to non-fatal in D-04a so
     boot progresses; no memslot means KVM_RUN can't reach
     any guest code yet.
  2. SREGS / CR3 / EFER / GDT / IDT are all zero on the
     vCPU. The first `KVM_RUN` inevitably fails-enter.

Both are needed before a guest can execute a single
instruction. This note records the shape D-04b wants.

## Memslot fix

The design-memo's "one memslot covers the whole UML
address space" maps cleanly to `guest_phys_addr=0`,
`memory_size=task_size`, `userspace_addr = UML's min
host-VA` — not `0`. UML's own process VA doesn't start at
0; it starts somewhere above `brk_start` (roughly
`0x60000000` on x86_64 linux hosts, but the exact value
comes from `linux_main()`'s `host_task_size` /
`stub_start` computation in `arch/um/kernel/um_arch.c`).

Rejected alternatives:

  - `userspace_addr=0` — what D-03c tried. KVM rejects it
    because a memslot's userspace_addr must be a valid VMA
    in the host process; address 0 isn't (overcommit aside,
    KVM validates against `find_vma`).
  - Tiny staging slot allocated via mmap — would work but
    diverges from the Policy A "cover everything" model
    and requires a separate per-mm slot scheme later.
    Rejected; simpler to fix the full-range slot.

Implementation:

  - Add `unsigned long kvm_guest_mem_base` to `struct kvm_um`.
    Value = UML's host-mapped kernel memory base (the
    lowest address in `kvm_init()`'s process VA that we
    want the guest to see).
  - Compute at init from an existing UML global. Candidates:
    - `stub_start` — upper bound of the addressable
      region, not the right side.
    - `phys_base` — matches UML's notion of guest-physical
      start. Plausible.
    - `uml_physmem` / `brk_start` — starts lower.
    Picking this is the first piece of D-04b to resolve
    with code + experimentation.
  - Register slot with `userspace_addr = kvm_guest_mem_base`,
    `memory_size = task_size - kvm_guest_mem_base`,
    `guest_phys_addr = 0`.
  - `mm_map` is unchanged semantically: mmap into UML's own
    VA at `virt`. Guest sees it at
    `virt - kvm_guest_mem_base`.

## SREGS / CR3 / long-mode entry

Port spike 04's bring-up sequence
(`spikes/04-longmode-lstar-vmcall/spike.c`) into a new
`arch/um/backend/kvm/sregs.c`. Spike 04 hand-allocates its
own GDT + page tables in a minimal guest memory region and
sets up ring-0 long mode. For D-04b we need the same
result but using UML's own kernel page tables — CR3 should
point at UML's `init_mm.pgd` (translated to guest-physical
via `pgd_host_va - kvm_guest_mem_base`) so the first
KVM_RUN enters UML's actual kernel code, not a scratch
trampoline.

Sub-steps within D-04b:

  1. **Sanity harness**: first, replicate spike 04's
     hand-crafted GDT+pagetable trampoline inside the
     backend, running to a guarded HLT. Not reaching UML
     kernel code yet — just proving the SREGS setup works
     from inside the scaffold. This is D-04b.1, verifiable
     via `KVM_EXIT_HLT` in run_userspace's existing switch.
  2. **UML-page-table CR3**: swap the handcrafted page
     tables for UML's own `init_mm.pgd`. CR3 ← physaddr of
     pgd. Verify KVM_RUN enters UML kernel code at a known
     RIP (e.g. a specially-placed `hlt` in early setup).
     This is D-04b.2.
  3. **LSTAR-populated bounce trampoline**: write the
     ~30-byte bounce trampoline (from spike 07's variant-B)
     into a guest-visible page and set MSR_LSTAR. Enables
     D-04c.

Sub-step 1 is landable on its own and worth its own
commit. Sub-steps 2 and 3 may share a commit depending on
how the CR3 arithmetic resolves.

## CR3 arithmetic

Per the design memo:

> Guest virtual addresses are UML virtual addresses; CR3
> is whatever UML's current mm->pgd points to, translated
> to a guest-physical offset via the same arithmetic UML
> already uses.

UML's existing arithmetic is `__pa(x) = x - uml_physmem +
0` (roughly). For the KVM backend with `guest_phys_addr=0`
memslot and `userspace_addr = kvm_guest_mem_base`:

  `guest_phys(host_va) = host_va - kvm_guest_mem_base`

So `CR3 = (unsigned long)init_mm.pgd - kvm_guest_mem_base`.

That requires `init_mm.pgd` to be in the memslot-covered
range, which it is by construction if `kvm_guest_mem_base`
is chosen correctly.

## Validation

D-04b.1 (hand-crafted trampoline): run_userspace returns
with `exit_reason == KVM_EXIT_HLT`. Add a dmesg line and
panic-with-context rather than "success". Manual sudo
test only — the pre-console-panic issue still blocks CI
verification of this path (UML infra issue, orthogonal).

D-04b.2 (UML-CR3): same `KVM_EXIT_HLT`, but the HLT must
be in UML's own kernel text (arrange via a dedicated
early-init routine the backend points RIP at). Proves
CR3 translation and the identity memslot both work.

D-04b full: measure first vCPU KVM_RUN round-trip
(entry → HLT → exit) in cycles. Log to
`measurements.md` with date + host + CPU. First data
point on the actual backend vs the spike floor.

## Open questions (resolve in code)

  1. **Which UML global is `kvm_guest_mem_base`?** The
     spike-04 pattern uses self-allocated memory, so
     there's no direct analog. The right choice depends
     on where `init_mm.pgd` physically lives in UML's
     address space — can be determined by adding
     `pr_info("init_mm.pgd=%px\n", init_mm.pgd)` to a
     test build.
  2. **Page-table format mismatch.** UML's `init_mm.pgd`
     uses the kernel's pgd format. Is that the same 4-
     level x86_64 format KVM's hardware-assisted paging
     (EPT+guest-page-tables) expects? UML's x86_64 port
     uses the same pgd layout as host x86_64 — so yes,
     but worth confirming with `kvm_stat` on the first
     run.
  3. **IDT.** Spike 04 uses `idt.base = 0, idt.limit = 0`
     (no IDT). Works for HLT-only guest. For real UML
     kernel code, interrupts / exceptions will need an
     IDT; that lands with D-05 (signal delivery).
     D-04b can follow spike 04's pattern until then.

## Not in D-04b scope

- LSTAR bounce trampoline + SYSCALL dispatch — D-04c.
- Real IDT + exception vectors — D-05.
- KVM_RUN loop with signal interrupt handling — D-05.
- Per-mm CR3 switching on context_switch — D-05.

## Cross-references

- `design-memo.md` §"Address-space model" — parent
  shape.
- `03b-memslot-policy.md` — memslot Policy A.
- `spikes/04-longmode-lstar-vmcall/spike.c` — the
  reference SREGS + page-tables + GDT setup this
  commit ports.
- `spikes/07-syscall-lstar-ab/spike.c` — LSTAR pattern
  for D-04c.
- `measurements.md` — timing log for D-04b's first
  real backend round-trip.
- decisions-log D55/D56 — vision-line perf targets.
