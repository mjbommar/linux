# A-03: Wrap merged 6.16 seccomp work as seccomp backend

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** A-01, A-02 (need ptrace as reference impl)
**Blocks:** A-05 (conformance), C profiles that prefer seccomp

## Goal

Take Benjamin Berg's merged seccomp-mode code and wrap it as
`um_backend_seccomp` implementing the ops table. After this task,
selecting `backend=seccomp` at boot uses the seccomp trap path
instead of ptrace.

## Approach

1. Read the merged seccomp series (linux-um list, 6.16-rc1 pull).
   Map every function to an ops-table op.
2. Move seccomp-specific code from wherever it landed (likely
   mixed in `arch/um/os-Linux/`) into `arch/um/backend/seccomp/`.
3. Implement each ops-table op as a wrapper around the existing
   seccomp impl.
4. Where seccomp lacks an op the ops table requires (e.g., maybe
   some debug op), add it. This is real engineering work.
5. Pass the conformance suite (A-05).

## Deliverable

- `arch/um/backend/seccomp/` directory
- `um_backend_seccomp` ops-table instance
- Conformance suite passes
- Performance: faster syscall path than ptrace (target: 2× or
  better)

## Validation

- All A-05 conformance tests pass on seccomp backend
- Microbenchmark: `getpid()` syscall cycles ≤ 50% of ptrace
  backend
- LTP, kselftest pass identically to ptrace backend (invariant
  I4)

## Open questions

- **Q1**: Berg's seccomp work currently requires `seccomp=on` boot
  param. Do we wire it under `backend=seccomp` instead? (Plan: yes;
  deprecate `seccomp=on` after one release.)
- **Q2**: Seccomp mode has had open security review concerns.
  How do we close them out? (Plan: list them in the conformance
  suite; each becomes a CI test.)
- **Q3**: Does seccomp backend interact differently with SMP than
  ptrace does? (Plan: probably yes for IPI delivery via signals;
  per-CPU stub state may differ. Verify.)

## Risk: seccomp backend was 4 years to land

Berg's seccomp work took years of review. Wrapping it should be
much faster (the hard architectural work is done) but the security
review concerns may resurface.

**Mitigation:**
- Engage Berg directly during this task; he knows the gotchas.
- Document any behavioral difference between ptrace and seccomp
  paths in `arch/um/Documentation/backends.rst`.
