# B-01: Audit hot-path entry points

**Status:** complete (2026-04-18) — see `notes/hot-paths.md`
**Effort:** 2 weeks
**Dependencies:** A-01 (ops table) merged
**Blocks:** B-02 (can't insert gates without knowing where)

## Goal

Catalog every hot path in `arch/um/` where a static-key gate
will go. Decide gate placement, name each gate, document
intended slow-path semantics.

## Approach

1. Static analysis: grep every backend op call site
   (`um_backend->`, `um_backend_dispatch`).
2. Profile-driven: boot prod-fast, run a workload, identify
   functions consuming >0.1% of cycles.
3. For each hot path, decide: 0, 1, or N gates here?
4. Document in a table: hook name, inserted at, slow-path
   handler, expected on-state cost.

## Deliverable

- `02-workstreams/B-static-key-hot-paths/hot-paths.md` — the table
- Sketch patches showing gate insertions for each path

## Validation

- Architects (workstream A owners) review the placement
- No gate placed in a function called <1k/sec (waste)
- No critical path missed (e.g., interrupt return)

## Open questions

- **Q1**: Do we gate IRQ handlers individually, or batch
  per-IRQ-vector? (Plan: batch.)
- **Q2**: Slow-path handlers — where do they live? (Plan:
  `kernel/trace/`, `kernel/kcov.c`, etc. — UML-specific only
  if there's no existing host-kernel slow path.)

## Risk: missing a hot path

A hot path without a gate can't be observed at runtime. This is
not a correctness bug but it's an architecture-erosion bug:
features added later that need observability there will
re-introduce conditional compilation.

**Mitigation:** review the hot-paths table broadly; rerun the
audit after the first profile is shipped.
