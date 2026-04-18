# Jump-label status in UML

**Date:** 2026-04-18
**Applies to:** B-02 gate infrastructure

## The question

B-01.Q1 / B-00.Q1 asks: *"Does Linux's `static_branch_unlikely`
work inside the UML guest the same as on bare metal?"*

## The answer

**Partially, today.** The macros work — `DECLARE_STATIC_KEY_FALSE` /
`DEFINE_STATIC_KEY_FALSE` / `static_branch_unlikely` all compile,
`static_branch_enable()` toggles the backing `struct static_key`,
and off-state call sites do fall through without invoking the slow
path. But we fall back to the **C implementation**, not the JIT-
patched NOP form, because `arch/um/Kconfig` does not
`select HAVE_ARCH_JUMP_LABEL`.

## Why UML doesn't JIT-patch yet

The JIT form requires:

1. A `.data..ro_after_init`-style `__jump_table` section holding
   (site, target, key) tuples — already present on every arch.
2. `arch_jump_label_transform()` to rewrite the branch instruction
   in place at runtime. On x86, this uses `text_poke_bp()`, which
   in turn requires a writable mapping of `.text` during the poke.

UML's `.text` is mmap'd read-only from the host ELF by
`kernel/execve(2)` (or equivalent), so we cannot `text_poke` until
we can **mprotect it writable first** — which is exactly what B-04
(".text section split") delivers. Until that lands, we cannot
safely transform `.text` instructions at runtime without
breaking the binary's own RO guarantees.

## Off-state cost today

Measured in B-05 (see `notes/objdump/handle_syscall.txt` for the
generated form around each gate). The C-fallback pattern at each
gate is:

```
  mov <key-addr>, %reg         ; PC-relative load of &key->enabled
  test %eax, %eax              ; is it > 0?
  jle <fallthrough>            ; predicted not-taken (branch hint)
```

That is ~3 instructions + 1 well-predicted branch per gate, or
~1–2 ns on a modern CPU. Within the "Off, before JIT init | ~1 ns"
budget the three-layers.md cost model assigns to this state.

Invariant **I3** ("every static-key gate must be JITted to a NOP")
is therefore **not yet met literally**. The letter of I3 is met
once B-04 unlocks mprotect-writable-then-poke. The spirit of I3
(observability is close to free when off) is met today.

This is recorded as decision **D19** in
`04-risks/decisions-log.md` so a reviewer asking "why aren't these
NOPs?" has a canonical answer.

## Path to full JIT patching

Work required, in order:

1. **B-04** (this workstream): split `.text` into `.text.frozen`
   (RO after init) and `.text.patchable` (holds static-branch
   call sites; RW during transform, RO otherwise). The existing
   x86 `text_poke_bp` machinery expects exactly this layout.
2. **`arch/um/kernel/jump_label.c`**: a thin wrapper that bridges
   the arch-generic `arch_jump_label_transform` to an mprotect-
   based poke helper. Models after `arch/x86/kernel/jump_label.c`.
3. **`arch/um/Kconfig`**: `select HAVE_ARCH_JUMP_LABEL`.
4. **Rebuild**; objdump proves gates are now 5-byte NOPs.

Until step 3, `static_branch_unlikely` is honest-but-slower than
the eventual JIT form. After it, off-state cost drops to the
~0.3 ns three-layers.md cost model predicts.

## What we verified today

- Gates compile and link in all three build modes
  (PTRACE_ONLY, SECCOMP_ONLY, DYNAMIC).
- The kernel boots and runs under both ptrace and seccomp with
  all seven gates off (default).
- `handle_syscall`, `segv`, `__switch_to`, `do_IRQ`, `timer_read`
  all contain the expected C-fallback sequence at each gate site;
  dumps saved in `notes/objdump/`.
- KUnit conformance (20 tests) unchanged — hooks have no
  observable effect when off.

## What B-04 will deliver for this

The section split is the common dependency for:
- static_branch JIT patching (this note)
- ftrace mcount patching (future workstream, unblocks once
  B-04 lands)
- kprobe insertion (future)
- bpf JIT code allocation (future)

B-04 is therefore the load-bearing piece for all runtime
code-modification features. B-02 lands the call sites; B-04
unlocks the patch mechanism.
