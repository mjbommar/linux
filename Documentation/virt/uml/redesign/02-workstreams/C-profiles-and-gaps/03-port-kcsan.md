# C-03: Port KCSAN to UML

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** SMP merged (currently Tiwei Bie v4 RFC)
**Blocks:** fuzz-deep profile (needs KCSAN)

## Goal

`select HAVE_ARCH_KCSAN` for UML. KCSAN detects data races at
runtime via watchpoints + delays. Critical now that UML kernel
is SMP.

## Approach

1. KCSAN's arch dependency: per-CPU state, instrumented
   memory access wrappers, `set_intr_state`/`get_intr_state`
   hooks.
2. UML's SMP brings real spinlocks + per-CPU data; KCSAN can
   build on those.
3. `select HAVE_ARCH_KCSAN` and `HAVE_ARCH_KCSAN_BARRIER` in
   `arch/um/Kconfig` (gated on SMP).
4. Run KCSAN kunit tests.
5. Find a known race in UML's own kernel code (probably exists);
   verify KCSAN reports it.

## Deliverable

- `arch/um/include/asm/kcsan.h`
- KCSAN compile-time wraps work in UML build
- Kconfig select
- KCSAN kunit tests pass

## Validation

- `kunit_test_kcsan` passes
- Synthesize a race in UML driver code; KCSAN reports
- KCSAN-on UML detects races in a known SMP-kernel testcase
  (e.g., a contrived race in a UM stub)

## Open questions

- **Q1**: KCSAN uses signal delivery for delays. Does this
  conflict with UML's signal-driven trap machinery? (Plan:
  carefully; UML uses SIGSEGV, SIGTRAP, SIGSYS; KCSAN uses
  SIGURG or similar. Verify.)
- **Q2**: Does KCSAN work with all three backends? (Plan:
  yes. KCSAN is purely in-guest; backend-orthogonal.)

## Risk

KCSAN port may turn out to be hard if UML's SMP signal handling
conflicts. If so, falls back to ftrace-based watchpoints
(KCSAN supports both modes).

**Mitigation:** start with ftrace-based mode; upgrade to
signal-based if practical.
