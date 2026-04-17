# D-04: Ring transitions (kernel ↔ user)

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** D-02, D-03
**Blocks:** D-06

## Goal

Enable robust switching between ring-0 (UML kernel) and ring-3
(guest userspace) inside the KVM guest, including signal
delivery, IRQ injection, and IPI.

## Approach

1. **Kernel → user**: `sysret` or `iret` from ring-0. Targets a
   userspace RIP saved at syscall entry.
2. **User → kernel**: `syscall` (handled in D-02), `int3`,
   page fault, exception. All trap to ring-0 handlers.
3. **Signal delivery**: when host kernel needs to interrupt a
   guest userspace thread, host injects an IRQ via KVM; ring-0
   handler delivers as POSIX signal.
4. **IPI between vCPUs**: ring-0 on one vCPU injects an IPI to
   another via KVM API; the other ring-0 handles.

## Deliverable

- `arch/um/backend/kvm/entry.S` — ring-3 → ring-0 entry stubs
- `arch/um/backend/kvm/exit.S` — ring-0 → ring-3 exit stubs
- `arch/um/backend/kvm/signals.c` — signal delivery
- `arch/um/backend/kvm/ipi.c` — IPI mechanism
- Tests for each path

## Validation

- Signal delivery to a guest userspace process works
  (`kill -USR1 $pid` from host)
- Multiple vCPUs can IPI each other (smp_call_function works)
- IRQ injection from host (e.g., for virtio device) reaches the
  expected ring-0 handler

## Open questions

- **Q1**: Signal masking — how do we ensure ring-0 doesn't
  deliver a signal during a critical section? (Plan: existing
  Linux signal-mask mechanism applies; ring-0 manages.)
- **Q2**: IPI vector allocation — share with host or KVM-private?
  (Plan: KVM-private; isolated from host IRQ landscape.)

## Risk

Subtle bugs in entry/exit stubs cause data corruption that's
hard to debug.

**Mitigation:**
- Extensive ASM review
- Match upstream x86 entry code where possible
- Use KVM tracepoints to debug entry/exit transitions
