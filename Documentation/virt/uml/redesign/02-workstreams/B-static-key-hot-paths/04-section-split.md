# B-04: .text section split (RWX vs frozen)

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** A-01 (ops table), B-02 (gates exist)
**Blocks:** D KVM backend (depends on RO `.text.frozen`)

## Goal

Split the kernel's `.text` into `.text.frozen` (read-only after
init) and `.text.patchable` (RWX or W^X-toggled, holds mcount
stubs and static_key call sites). Resolves conflict C1 in
`01-architecture/conflicts.md`.

## Approach

1. **Identify patchable code**: every `static_branch_*()` call
   site, every mcount stub, every kprobe insertion point. These
   live in `.text.patchable`.
2. **Linker script**: extend `arch/um/kernel/vmlinux.lds.S` to
   place `.text.patchable` and `.text.frozen` in separate sections.
3. **Init-time mprotect**: after initcalls, mprotect `.text.frozen`
   read-only. `.text.patchable` stays RW.
4. **W^X discipline (optional)**: for stricter security, toggle
   `.text.patchable` between RW and RX around patches. Uses
   `set_memory_rw` / `set_memory_ro` style helpers.
5. **Verify on KVM backend**: when KVM mode lands (workstream D),
   `.text.frozen` can be marked RO in EPT/NPT for cacheable RO.

## Deliverable

- `arch/um/kernel/vmlinux.lds.S` updated
- `arch/um/kernel/section-split.c` (mprotect at init, W^X helpers)
- Helper attribute: `__patchable_function` for explicit placement
- Documentation in `arch/um/Documentation/section-split.rst`

## Validation

- `readelf -S vmlinux` shows both sections
- `cat /proc/kallsyms | grep '__patchable'` shows expected symbols
  in patchable section
- ftrace patches succeed; KVM RO mapping works (post-D)
- KASAN doesn't false-positive on patch sites

## Open questions

- **Q1**: Do we need a third section for `__init` code that's
  freed? (Plan: yes; that's the existing `__init` mechanism.
  Three sections total: init, frozen, patchable.)
- **Q2**: How do kprobes interact? (Plan: kprobes inserts in
  patchable section; existing host-kernel mechanism applies.)
- **Q3**: BPF JIT writes new code at runtime. Where does it go?
  (Plan: dedicated `bpf_jit` allocator that mprotect-toggles the
  region around emission. Standard pattern.)

## Risk

Linker script changes can break boot in subtle ways.

**Mitigation:**
- Extensive boot testing per backend
- Use the existing `__ro_after_init` mechanism as a model
- Don't be clever; copy x86_64's approach where possible
