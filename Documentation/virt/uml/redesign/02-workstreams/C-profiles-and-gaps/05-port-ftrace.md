# C-05: Port ftrace to UML

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** B-04 (.text section split)
**Blocks:** research profile having ftrace; tracepoints become useful

## Goal

`select HAVE_FUNCTION_TRACER`, `HAVE_DYNAMIC_FTRACE`,
`HAVE_FUNCTION_GRAPH_TRACER` for UML. Enables `tracefs`,
function-graph profiling, dynamic ftrace.

## Approach

1. Compile with `-pg` (enables mcount stubs at function entry).
2. Implement `mcount` thunk in `arch/um/kernel/mcount.S`:
   trampoline calls `__fentry__` → ftrace dispatch.
3. mcount stubs land in `.text.patchable` (B-04).
4. Implement `ftrace_modify_call` to patch in/out.
5. `function-graph` adds a return trampoline.
6. Run ftrace selftests.

## Deliverable

- `arch/um/kernel/mcount.S`
- `arch/um/kernel/ftrace.c`
- `arch/um/include/asm/ftrace.h`
- Kconfig selects
- `tools/testing/selftests/ftrace/` passes

## Validation

- `echo function > /sys/kernel/debug/tracing/current_tracer` works
- `echo function_graph >` works
- `set_ftrace_filter` works
- Trace overhead matches host x86_64 numbers

## Open questions

- **Q1**: -pg cost on UML overall — does it regress prod-fast?
  (Plan: ftrace is research/fuzz only; prod-fast doesn't enable
  -pg.)
- **Q2**: Function-graph trampoline interacts with stack
  unwinding; does our unwinder cope? (Plan: this is also
  invariant I2's responsibility — fix unwinder if needed; fix is
  separable.)

## Risk

Mcount stubbing is well-understood territory in the kernel; the
port should be mechanical. The tricky part is interaction with
UML's signal-based scheduling — function-graph's return trampoline
must survive cross-stub returns.

**Mitigation:** start with function tracer (no return trampoline);
add function-graph as a follow-up.
