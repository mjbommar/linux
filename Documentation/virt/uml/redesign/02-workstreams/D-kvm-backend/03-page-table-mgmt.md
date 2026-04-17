# D-03: Page-table management (host vs guest)

**Status:** planned
**Effort:** 6 weeks (largest D task)
**Dependencies:** D-02
**Blocks:** D-06

## Goal

Manage the KVM guest's page tables so each UML guest userspace
process sees the right address space. Host-mode UML kernel
controls these page tables; ring-0 UML kernel inside the KVM
guest reads them via CR3.

## Approach

1. **Address-space model**:
   - One KVM guest per UML kernel.
   - Multiple "address spaces" within the KVM guest, one per UML
     guest userspace process.
   - Switching between address spaces = `mov %cr3, ...` inside
     the guest, done by ring-0 on context_switch.
2. **Page table population**:
   - When UML kernel maps memory into a guest userspace process,
     it updates that process's page tables (which live in KVM
     guest memory).
   - When the guest userspace executes and faults, hardware
     uses guest CR3; if no entry, traps to ring-0 page-fault
     handler.
3. **EPT/NPT**: KVM's host-side page tables (Extended Page
   Tables on Intel) map guest physical to host physical. Host-
   mode UML kernel populates these via KVM ioctls.
4. **Section split** (B-04): `.text.frozen` is mapped RO in EPT;
   `.text.patchable` is RW.

## Deliverable

- `arch/um/backend/kvm/mm.c` — page-table management
- `arch/um/backend/kvm/ept.c` — EPT/NPT helpers
- Documentation: how guest CR3 reflects UML's mm_struct

## Validation

- A guest userspace process with mmaped regions reads/writes
  correctly
- Page faults route to UML's existing fault handler
- Multiple guest processes have independent address spaces
- KASAN shadow region maps correctly in EPT (KASAN-instrumented
  build still works under KVM backend)

## Open questions

- **Q1**: Do we use KVM's IOMMU support or pure EPT? (Plan: pure
  EPT for simplicity; IOMMU only if we need device passthrough,
  which prod-fast doesn't.)
- **Q2**: Hugepages — do we use 2 MB / 1 GB pages in EPT for
  faster TLB? (Plan: yes when guest mappings are large enough.
  Standard optimization.)
- **Q3**: ASLR — is the KVM-guest ring-0 layout randomized?
  (Plan: yes, same as host kernel. KASLR support.)

## Risk

Highest of D's tasks. Page-table management has many subtle
edge cases (TLB shootdowns, COW after fork, executable
permission for JITted BPF, etc.).

**Mitigation:**
- Reuse upstream x86 mm code wherever possible
- Validate KVM page-table invariants with `kvm_stat` and KVM
  selftests
- Stress test with many concurrent processes
