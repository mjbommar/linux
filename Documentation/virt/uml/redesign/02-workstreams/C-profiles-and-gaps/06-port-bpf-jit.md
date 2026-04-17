# C-06: Port BPF JIT to UML

**Status:** planned
**Effort:** 3 weeks
**Dependencies:** B-04 (`.text` section + JIT allocator)
**Blocks:** research profile getting realistic BPF perf

## Goal

`select HAVE_EBPF_JIT` for UML. The host kernel x86_64 JIT
backend is what we want; reuse it.

## Approach

1. UML on x86_64 host can use the existing `arch/x86/net/bpf_jit_*.c`
   essentially unchanged — it emits x86_64 machine code, which is
   what UML executes.
2. Wire UML's Kconfig to `select HAVE_EBPF_JIT`.
3. Implement BPF JIT allocator using B-04's W^X mechanism for
   `.text.patchable`.
4. Run BPF selftests.

## Deliverable

- `arch/um/Kconfig` selects
- `arch/um/net/bpf_jit_glue.c` (probably small — calls into
  shared x86 JIT code)
- BPF selftests pass

## Validation

- `tools/testing/selftests/bpf/test_progs` runs
- BPF program load + JIT visible via `bpftool prog show`
- bpftrace one-liners work

## Open questions

- **Q1**: Does UML's JIT allocator need to be different from
  x86_64's because of the patchable-section model? (Plan: probably
  same allocator; just different `.text` section.)
- **Q2**: ARM64 UML JIT (when ARM64 lands) — reuse ARM64 host JIT
  similarly? (Plan: yes; same pattern.)

## Risk

Low for x86_64. The BPF JIT allocator is well-understood; the
arch-specific JIT code itself is reused.
