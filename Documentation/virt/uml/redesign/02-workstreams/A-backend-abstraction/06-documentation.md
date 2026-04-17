# A-06: Documentation — contract + cost model

**Status:** planned
**Effort:** 2 weeks
**Dependencies:** A-01..A-05 (must reflect the actual implementation)
**Blocks:** maintainer review for upstream submission

## Goal

A LKML-quality `Documentation/virt/uml/backends.rst` that explains:

- What `struct um_backend_ops` is and why
- Each backend's cost model and implementation summary
- How to write a new backend
- How to choose a backend at runtime
- Compatibility table (which backend on which host kernel)

## Approach

Write three documents:

1. **`backends.rst`** (user-facing) — what backends exist, how to
   pick one, what the tradeoffs are. Goes in
   `Documentation/virt/uml/`.

2. **`backend-contract.rst`** (developer-facing) — the ops table,
   semantics, invariants. Goes in `arch/um/Documentation/`.

3. **`backend-cost-model.md`** (planning artifact) — measured
   numbers per backend. Goes in this planning directory and
   gets updated as benchmarks change.

## Deliverable

Three documents above, ready for `Reviewed-by` from maintainers.

## Validation

- Maintainer (Berg or Weinberger) reviews each
- A new contributor can implement a stub backend by following
  the developer docs alone
- `kernel-doc` parses cleanly

## Open questions

- **Q1**: Where does the ASCII diagram of the trap path go?
  (Plan: in `backends.rst`, with one diagram per backend.)
- **Q2**: How much of the planning docs (this directory) get
  upstreamed? (Plan: none. They're internal scaffolding. Final
  docs get rewritten for upstream tone.)

## Risk

Low. Mechanical writing work after the design is settled.
