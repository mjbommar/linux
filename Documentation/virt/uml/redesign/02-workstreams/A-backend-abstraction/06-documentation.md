# A-06: Documentation — contract + cost model

**Status:** **complete (2026-04-18)** — 3 docs landed, Sphinx
builds clean.
**Effort:** 2 weeks budget; consumed ~3 hours since most content
already existed (backend-contract.rst from A-01.9, status text
from notes/) and only needed digestion + cross-linking.
**Dependencies:** A-01..A-05 (must reflect the actual implementation) — all landed
**Blocks:** maintainer review for upstream submission

## Status detail

| Doc | Location | Audience | Status |
|---|---|---|---|
| `backends.rst` | `Documentation/virt/uml/backends.rst` | end users | landed; ASCII trap-path diagrams per backend; cross-links to contract spec |
| `backend-contract.rst` | `Documentation/virt/uml/backend-contract.rst` | developers writing/extending backends | polished from A-01.9 draft; planning vocabulary stripped; `:ref:` anchor `backend-contract` for cross-doc linking |
| `backend-cost-model.md` | `Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/backend-cost-model.md` | planning artifact tracking measured numbers | landed with current boot wall-time per backend; placeholder for A-07 microbenchmarks |
| Sphinx index | `Documentation/virt/index.rst` | site nav | added `uml/backends` and `uml/backend-contract` to toctree |
| Sphinx build | `make htmldocs SPHINXDIRS=virt` | – | clean (only `dot(1) not found` env warning, unrelated) |

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
