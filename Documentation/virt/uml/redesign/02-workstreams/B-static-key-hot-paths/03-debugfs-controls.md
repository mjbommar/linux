# B-03: debugfs controls

**Status:** planned
**Effort:** 2 weeks
**Dependencies:** B-02 (gates exist)
**Blocks:** B-06 (end-to-end demo needs runtime control)

## Goal

Expose every gate as a debugfs file. Reading shows current state;
writing 0/1 toggles the gate. Minimal surface, conventional
semantics.

## Approach

1. Mount-time create `/sys/kernel/debug/um/hooks/`.
2. For each gate: `debugfs_create_bool` or
   `debugfs_create_u32` (for state machines, e.g.,
   record-replay's record / replay / off).
3. Add `/sys/kernel/debug/um/stats` showing per-hook on-time and
   slow-path call count.
4. Add `/sys/kernel/debug/um/backend` showing current backend.
5. Document in `Documentation/virt/uml/debugfs.rst`.

## Deliverable

- `arch/um/kernel/debugfs.c`
- `Documentation/virt/uml/debugfs.rst`

## Validation

- `cat /sys/kernel/debug/um/hooks/trace_syscalls` returns 0|1
- Writing toggles the gate (verify with stats counter)
- Permissions: root-only (KCOV's coverage data is sensitive)

## Open questions

- **Q1**: Should we gate the gates themselves behind a Kconfig
  for sandbox profile? (Plan: yes — sandbox has no debugfs
  exposure of these. `CONFIG_UM_HOOKS_DEBUGFS=n` for sandbox.)
- **Q2**: Per-CPU stats? (Plan: aggregated. Per-CPU is too verbose.)

## Risk

Low.
