# R2: RWX `.text` patches vs KVM RO mappings

## The risk

ftrace/kprobes/static_branch all patch executable code at
runtime. KVM mode wants `.text` mapped read-only inside the KVM
guest. These collide.

If unresolved, KVM backend can't ship with prod-with-hooks
profile (the killer combo). Profile becomes "prod-with-hooks
on seccomp only".

## Why this is real but not catastrophic

The kernel has solved this before. `__ro_after_init` sections
exist. x86_64 marks much of `.text` RO post-init.

The conflict is bounded: only specific call sites need to be
patchable. Splitting `.text` into `.text.frozen` (RO) and
`.text.patchable` (RW or W^X-toggled) resolves it.

## Mitigation: workstream B-04

`02-workstreams/B-static-key-hot-paths/04-section-split.md`
covers this directly. Outline:

1. Identify patchable code: every static_branch call site,
   mcount stub, kprobe insertion point.
2. Linker script puts patchable code in `.text.patchable`.
3. Init-time mprotect makes `.text.frozen` RO.
4. KVM backend EPT-maps `.text.frozen` RO; `.text.patchable`
   RW.

## What could still go wrong

- **W^X discipline**: if `.text.patchable` is permanently RW,
  it's an attack surface. Better: toggle RW around the patch,
  RX otherwise. Adds complexity.
- **BPF JIT**: JITted BPF code is in a different region (managed
  by `bpf_jit_alloc`); standard pattern but verify it doesn't
  collide.
- **Module loading**: if/when UML supports modules in research
  profile, modules' text needs the same treatment.

## What we accept

- Slightly larger binary (extra section padding).
- Slightly higher TLB cost (two text sections vs one).
- Discipline around `__patchable_function` annotation: every
  function called from a patchable site needs the annotation.

## Triggers for revisit

- KVM backend perf measurements show TLB miss dominating
  syscall cost (probably means `.text.frozen` is too small or
  paged-in poorly).
- A patch lands a new patching mechanism (e.g., new tracing
  framework) that doesn't go through static_branch or mcount.

## Reference

- Existing `__ro_after_init` mechanism in `include/linux/init.h`
- x86_64's text mprotect in `arch/x86/kernel/cpu/bugs.c`
- gVisor's section management for KVM mode
