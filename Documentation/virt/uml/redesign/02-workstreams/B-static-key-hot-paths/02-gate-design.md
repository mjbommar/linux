# B-02: Gate design and hook-helper macros

**Status:** planned
**Effort:** 3 weeks
**Dependencies:** B-01 (know where gates go)
**Blocks:** B-03 (debugfs controls them), B-05 (benchmarks need
            something to measure), C profile defaults

## Goal

Define the gate-helper macros, declare the static keys, insert the
gates at every hot path identified in B-01.

## Approach

1. Write `arch/um/include/asm/um-hooks.h` with `DECLARE_STATIC_KEY_FALSE`
   for each gate and `__always_inline` helpers.
2. Write a generic `um_hook_dispatch(GATE, slow_path, ...)` macro
   so that gate-and-call patterns are uniform.
3. Insert `um_on_*()` calls at each hot path.
4. Verify with `objdump -d` that off-state JITs to NOPs.
5. Add slow-path handlers (calling existing host-kernel
   subsystems where possible).

## Deliverable

- `arch/um/include/asm/um-hooks.h`
- `arch/um/kernel/hooks.c` (gate definitions, slow-path glue)
- ~10 inserted gates across hot paths
- objdump verification artifacts saved in `02-workstreams/B-static-key-hot-paths/objdump/`

## Validation

- `objdump -d arch/um/backend/seccomp/syscall_dispatch.o` shows
  NOPs at gate sites in default-off build
- `echo 1 > /sys/kernel/debug/um/hooks/trace_syscalls` (after
  B-03) flips the gate; objdump shows JMPs
- Microbench (B-05): off-state cost <2 ns per gate

## Open questions

- **Q1**: Do we use `static_branch_unlikely` (off-default,
  unlikely-to-be-on) or `static_branch_likely` (on-default)?
  (Plan: `unlikely` for everything except possibly the trace
  gate in research profile, which can be likely.)
- **Q2**: Can multiple hooks be combined into one gate for
  correlated features? (Plan: no. One gate per concern. Cheaper
  to flip independently than to manage gate-set state.)

## Risk

Gate insertions in tight loops accidentally regress prod-fast.

**Mitigation:** B-05's microbenchmarks. Plus invariant I2's CI
enforcement (workstream A-07).
