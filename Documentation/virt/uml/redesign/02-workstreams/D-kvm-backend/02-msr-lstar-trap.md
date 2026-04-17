# D-02: MSR_LSTAR trap path

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** D-01
**Blocks:** D-04 (ring transitions), D-06 (conformance)

## Goal

When a guest userspace process executes `syscall`, control lands
in UML kernel's ring-0 handler at the address in `MSR_LSTAR` —
inside the same KVM guest, no VMEXIT to host. Implement and
verify this path.

## Approach

1. Configure the KVM guest's `MSR_LSTAR` to point at UML kernel's
   syscall entry (`entry_SYSCALL_64` or UML equivalent).
2. Configure the guest's GDT so ring-0 has the right code/data
   segments.
3. Userspace executes `syscall` → hardware switches to ring-0,
   jumps to MSR_LSTAR addr.
4. Ring-0 handler (UML kernel) services the syscall; returns via
   `sysretq`.
5. Measure round-trip cost.

## Deliverable

- `arch/um/backend/kvm/syscall.S` (the ring-0 entry stub)
- `arch/um/backend/kvm/setup.c` (MSR_LSTAR + GDT setup)
- Microbenchmark showing syscall round-trip <100 ns

## Validation

- A userspace `getpid()` returns the correct value
- Round-trip cost measured with `rdtscp` is <100 ns
- 1M sequential `getpid()` calls complete without VMEXIT
  (verify via host-side counter)

## Open questions

- **Q1**: How do we handle syscalls that need host help (I/O,
  scheduling)? (Plan: ring-0 handler does the syscall work
  inline if possible; VMCALLs out to host-mode for things that
  need host action like blocking I/O or scheduling.)
- **Q2**: Does the existing UML syscall entry need rework?
  (Plan: probably yes. Each backend gets its own entry stub;
  the dispatch logic is shared.)

## Risk

MSR setup at KVM guest init is fiddly. Bugs here manifest as
hangs, triple-faults, or "vCPU keeps resetting".

**Mitigation:**
- Validate against gVisor's known-working setup
- Use KVM's tracing (`perf kvm`) to debug
