# D-01: KVM platform design

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** A-01 (ops table)
**Blocks:** D-02..D-06

## Goal

A complete design memo for `um_backend_kvm`. Adapt gVisor's KVM
platform model to UML's needs. Identify everywhere we deviate
from gVisor and why.

## Approach

1. Study gVisor's KVM platform end-to-end
   (`pkg/sentry/platform/kvm/`).
2. Map every gVisor mechanism to UML equivalent:
   - `Machine` → UML KVM context
   - `vCPU` → KVM vCPU per UML vCPU
   - `addressSpace` → UML guest mm_struct
   - bouncing thread → host-mode UML kernel thread
3. Identify deviations:
   - gVisor is in Go; UML is in C — affects allocator,
     thread mgmt
   - gVisor has Sentry as the kernel; UML has real Linux —
     more arch code to handle inside the guest
   - gVisor has userland-managed page tables; UML has
     kernel page tables already
4. Write the memo: `02-workstreams/D-kvm-backend/design-memo.md`.

## Deliverable

- `02-workstreams/D-kvm-backend/design-memo.md` (~10 pages)
- Map of gVisor mechanisms → UML equivalents
- Deviation list with rationale
- Sequence diagram: syscall trap path
- Sequence diagram: page fault path
- Sequence diagram: I/O path

## Validation

- Hypervisor expert reviews (look for someone with KVM internals
  experience)
- gVisor team reaches out to confirm we're not misreading their
  design
- All workstream A maintainers sign off on the ops table fit

## Open questions

- **Q1**: Single KVM guest with multiple vCPUs, or multiple KVM
  guests for SMP? (Plan: single guest, multiple vCPUs. Standard.)
- **Q2**: How does timekeeping work — host TSC, KVM clock,
  emulated PIT? (Plan: KVM clock if available, host TSC fallback.)
- **Q3**: Does the KVM guest need a BIOS / EFI / boot loader?
  (Plan: no. We construct the guest's initial state directly.
  No firmware boot.)

## Risk

The design memo is the highest-leverage document in workstream
D. If we get the design wrong, 5 months of work follow into a
dead end.

**Mitigation:**
- Long review (4 weeks).
- Spike: prototype the simplest possible "KVM guest that returns
  from MSR_LSTAR" before committing to the full design.
