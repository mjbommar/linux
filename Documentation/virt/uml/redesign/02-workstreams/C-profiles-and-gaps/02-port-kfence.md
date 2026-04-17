# C-02: Port KFENCE to UML

**Status:** planned
**Effort:** 2 weeks (cheapest port)
**Dependencies:** A-01 (just so the ops table exists)
**Blocks:** research, fuzz, fuzz-deep profiles having KFENCE

## Goal

`select HAVE_ARCH_KFENCE` works for UML. KFENCE catches
heap-corruption bugs at the SLAB allocator level via guard
pages.

## Approach

1. Read `Documentation/dev-tools/kfence.rst`.
2. Identify arch-specific hooks KFENCE expects.
3. Implement using existing UML page-table infrastructure
   (`mmap`/`mprotect`).
4. `select HAVE_ARCH_KFENCE` in `arch/um/Kconfig`.
5. Run KFENCE kunit tests.

## Why this is cheap

KFENCE's arch dependency is mostly: "give me guard pages and
page-fault delivery to a handler". UML already has both.
No shadow region (unlike KASAN). No instrumentation passes
(unlike KMSAN).

## Deliverable

- `arch/um/include/asm/kfence.h`
- `arch/um/mm/kfence.c` (probably small)
- Kconfig select
- KFENCE kunit tests pass

## Validation

- `kunit_test_kfence` passes
- Inject a known heap corruption; KFENCE catches it
- Microbench: KFENCE-on UML is no slower than KFENCE-on x86_64
  (UML pays the same sampling cost)

## Open questions

- **Q1**: Does KFENCE's sample interval (a static_branch) interact
  with workstream B's gates? (Plan: KFENCE uses its own static
  branch from existing `kfence.h`. No conflict.)

## Risk

Lowest of the C-port tasks. Recommend doing this first as
warm-up; validates the workstream's port methodology.
