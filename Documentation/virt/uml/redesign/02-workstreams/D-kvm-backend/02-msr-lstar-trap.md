# D-02: MSR_LSTAR trap path

**Status:** landed (2026-04-22) — shipped as D-04c
           (`3aeaeb8b6f5a`) after the D-04b harness proved the
           backend sits at spike floor. The SYSCALL + LSTAR
           dispatch path was measured via spike-07 variant-B
           methodology and `measurements.md` documents the
           silicon-invariant ~370-cyc delta
           (`d6d510bf497b`). The microbenchmark goal in this
           memo ("<100 ns round-trip") became the harness's
           per-iteration cycle-counter bar rather than a
           separate benchmark — D-04b.1c landed that surface
           (`c461e178da68`). D-02's original 4-week effort
           estimate was folded into the D-04 family; no
           separate D-02 landing commit exists.
**Effort:** 4 weeks (folded into D-04 family; see status)
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
