# C-04: Port kprobes to UML

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** B-04 (.text section split)
**Blocks:** research profile having kprobes; bpftrace

## Goal

`select HAVE_KPROBES` and `HAVE_KPROBES_ON_FTRACE` for UML.
Allows dynamic instrumentation at any kernel symbol.

## Approach

1. Implement arch-specific kprobe machinery:
   - `arch_prepare_kprobe` — generate the trampoline
   - `arch_arm_kprobe`, `arch_disarm_kprobe` — install/remove the
     int3 instruction
   - `kprobe_int3_handler` — invoked from UML's existing
     SIGTRAP path
   - Single-step emulation for post-handler
2. The int3 (or ud2) lands in our SIGTRAP/SIGSEGV handler;
   route to kprobe machinery.
3. Patches go in `.text.patchable` (B-04).
4. Run `kprobes` self-tests.

## Deliverable

- `arch/um/kernel/kprobes.c`
- `arch/um/include/asm/kprobes.h`
- Kconfig selects
- `kprobes` kunit tests pass

## Validation

- `samples/kprobes/kprobe_example.ko` works
- bpftrace can attach kprobes (after C-06 BPF JIT)
- kprobes don't interfere with KVM mode RO `.text.frozen`
  (they're in `.text.patchable`)

## Open questions

- **Q1**: x86_64 single-step emulation is non-trivial (Intel
  has the IST/INT 1 dance). UML inherits the host's; verify
  it works through our trap path.
- **Q2**: kretprobes (function returns) need a separate
  trampoline. Add or skip? (Plan: add; bpftrace uses them.)

## Risk

Single-step emulation through UML's stub paths is delicate.
Could uncover ptrace/seccomp interaction bugs.

**Mitigation:** validate per backend; failures here may need
backend-specific kprobe paths (acceptable; documented in
ops table).
