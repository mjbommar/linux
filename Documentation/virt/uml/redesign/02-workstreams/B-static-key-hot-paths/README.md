# Workstream B: Static-key hot-path gates

**Effort:** ~4 engineer-months
**Owner role:** Performance engineer
**Critical path:** Yes — gates C profile defaults
**Bookend:** Demonstrates one runtime-flippable hook end-to-end

## What this workstream produces

The Layer 2 infrastructure: static-key gates inserted at every hot
path (syscall entry/exit, page fault, context switch, IPI, clock
read), debugfs controls to flip them, and a section-split linker
script so KVM-mode RO text and ftrace patchable text coexist.

Deliverables:

1. `arch/um/include/asm/um-hooks.h` — gate macros and inline helpers
2. ~10 static-key declarations (one per hook)
3. Insertions at every hot-path call site
4. `/sys/kernel/debug/um/hooks/*` debugfs interface
5. `arch/um/kernel/vmlinux.lds.S` section split for `.text.frozen`
   vs `.text.patchable`
6. Microbenchmarks proving off-state cost <2 ns per gate

## Why this matters

This is the workstream that makes "have your cake and eat it" real.
Without it, profiles are just static Kconfig sets. With it, you can
ship one binary and let users flip observability on at runtime.

## Tasks

| # | Task | Effort | Status |
|---|---|---|---|
| 01 | [Audit hot-path entry points](01-audit-entry-points.md) | 2 wk | **complete (2026-04-18); 6 sites × 7 gates; see notes/hot-paths.md** |
| 02 | [Gate design and hook-helper macros](02-gate-design.md) | 3 wk | **complete (2026-04-18); 7 gates × 6 hook sites; C-fallback pending B-04 (D19)** |
| 03 | [debugfs controls](03-debugfs-controls.md) | 2 wk | **complete (2026-04-18); /sys/kernel/debug/um/{backend,hooks/*,stats}; end-to-end flip verified** |
| 04 | [.text section split (RWX vs frozen)](04-section-split.md) | 4 wk | **complete (2026-04-18); .um_patch_text + mprotect helpers; verified in 3 configs** |
| 05 | [Benchmark gate cost: off and on](05-benchmark-targets.md) | 3 wk | **complete (2026-04-18); I3 met (0.45–0.68 ns/gate off); baseline + compare script landed** |
| 06 | [First end-to-end: KCOV gate flippable mid-run](06-first-flip-demo.md) | 2 wk | **complete (2026-04-18); trace_syscalls flip demo PASSes under kselftest harness (KCOV deferred to C)** |

Total: ~16 weeks ≈ 4 EM.

## Milestones

- **Month 1**: Hot-path audit complete; gate placement plan
  reviewed
- **Month 2**: Helper macros + first 3 gates merged; off-state
  cost measured at <2 ns
- **Month 3**: All gates merged; debugfs interface live;
  KCOV-flip demo
- **Month 4**: Section split deployed; CI gates established

## Open questions

- **Q1**: Does Linux's `static_branch_unlikely` work inside the UML
  guest the same as on bare metal? (Plan: yes; verify in
  task 01.)
- **Q2**: How do we handle gate state across kernel reboots in
  snapshot mode? (Plan: gate state is part of snapshot.)
- **Q3**: Do we need per-CPU gates or are global gates enough?
  (Plan: global. SMP UML kernel-side runs same code paths on
  every CPU; per-CPU is unnecessary complexity.)

## What success looks like

- A user runs prod-with-hooks UML, observes a problem, runs
  `echo 1 > /sys/kernel/debug/um/hooks/trace_syscalls`, and the
  next syscall is in the trace ring buffer. No reboot. No rebuild.
- prod-fast benchmarks unchanged after gates added (invariant I2).
- ftrace and KVM mode coexist: `.text.patchable` accepts mcount
  patches; `.text.frozen` is RO inside the KVM guest.
