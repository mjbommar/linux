# C-04: Port kprobes to UML

**Status:** planned (full-scope design pass 2026-04-19 — see
"Approach" below; implementation in progress)
**Effort:** 3–4 focused days (original 4-week estimate revised
down — C-05's ftrace plumbing covers most of the infrastructure,
and the architectural research below cleared the two risks the
original spec flagged)
**Dependencies:** B-04 (.text section split, landed; helpers
extended by C-05 commit 1), C-05 (ftrace port, landed 2026-04-19
— provides the mcount.S / ftrace.c baseline that commits 1 and
5 extend).
**Blocks:** research profile having kprobes + function_graph;
bpftrace; C-06 BPF JIT's kprobe-reachability validation.

## Goal

`select HAVE_KPROBES`, `HAVE_KRETPROBES`, and
`HAVE_FUNCTION_GRAPH_TRACER` for UML on x86_64. Delivers the full
user-visible dynamic-instrumentation surface — entry probes, mid-
function probes (both via int3 + single-step), return probes
(kretprobes), and function-call graphs (function_graph). The
vision in `00-vision.md` calls out "all sanitizers and tracing on"
for the research profile; C-04 closes the "tracing" half of that
target.

**Scope note (2026-04-19):** `HAVE_KPROBES_ON_FTRACE` is NOT in
this task. Enabling the ftrace fast path for function-entry
kprobes is a pure optimization (avoids int3 trap cost on entry
probes) that requires a separate `ftrace_regs_caller` /
`HAVE_DYNAMIC_FTRACE_WITH_REGS` implementation in mcount.S — a
substantial piece of delicate assembly that would add risk to
C-05's already-working ftrace. User-visible kprobe features
(entry, mid-function, returns, graphs) all work through the int3
+ kretprobe-trampoline paths this task delivers. The ftrace fast
path belongs in a follow-up task ("C-04b: HAVE_KPROBES_ON_FTRACE")
once there's concrete performance pressure justifying the
assembly investment. Per a-plus-quality-plan anti-pattern #3
("optimize before workloads exist"), the ftrace path is a
Q4-style perf tuning, not a C-04 prerequisite.

Per the user's scope-setting guidance on 2026-04-19: this task
delivers the full kprobes surface in one workstream rather than
a phased C-04a/C-04b split. Reasoning:

- The int3 path and the ftrace path serve genuinely different
  user populations (mid-function vs function-entry probes);
  shipping only one leaves a visible gap in what
  `make ARCH=um uml/research` delivers relative to the vision.
- Kretprobes and function_graph share the same return-address-
  rewrite pattern and therefore the same theoretical signal race
  that D27 cited as reason to defer graph from C-05.
- Reading the UML signal infrastructure in depth (see
  "Architectural research" below) showed D27's signal-race
  concern is theoretical rather than observed, and existing
  livepatch return-address rewriting on UML works without
  special handling. D27 is empirically superseded by commit 5's
  stress test in this series.

## Architectural research (2026-04-19)

Three green-lights from reading arch/um/ + the x86 references:

**Green-light 1: Kernel-mode SIGTRAP has a clean hook.**
`arch/um/kernel/trap.c:417` (`relay_signal()`) currently panics
on `!UPT_IS_USER(regs)` paths for SIGTRAP. That's the exact
insertion point for `kprobe_int3_handler()`: dispatch first,
panic only on unhandled. Replaces a "shouldn't happen" site
with a "handled correctly" site.

**Green-light 2: X86_EFLAGS_TF already propagates.**
`arch/x86/um/os-Linux/mcontext.c:81..84` has an existing
`single_stepping` mechanism that sets/clears `X86_EFLAGS_TF` in
the mcontext when resuming the stub. A kprobe handler setting
`regs->flags |= X86_EFLAGS_TF` before returning causes the next
guest instruction to execute and deliver another SIGTRAP —
standard x86 single-step, no UML-specific emulation required.

**Green-light 3: return-address rewrite is not novel on UML.**
Livepatch and the kernel stack unwinder already rewrite or walk
return-address slots in UML without signal-race issues. The
SIGALRM delivery path (`arch/um/os-Linux/signal.c:131..150`) runs
in host context against a captured mcontext, writes modified
registers back via `set_stub_state()`, and does not racily
mutate the guest kernel stack mid-trampoline. D27's theoretical
race does not manifest in practice; commit 5's stress test
below empirically validates this before graph/kretprobes ship.

## Approach — seven-commit slice plan

Each commit builds cleanly under `ARCH=um` and `ARCH=um LLVM=1`,
boots, passes existing selftests, and is individually bisectable.

Out of scope (tracked as potential follow-up task "C-04b"):
`HAVE_KPROBES_ON_FTRACE` — the optimization that routes
function-entry kprobes through the ftrace fast path instead of
through int3. Implementing it needs `HAVE_DYNAMIC_FTRACE_WITH_REGS`,
which means a new `ftrace_regs_caller` in mcount.S that saves a
full UML pt_regs frame on the stack. That's ~200 LOC of delicate
assembly affecting every traced function; it adds risk to C-05's
working ftrace without providing new user-visible capability (the
int3 path handles entry probes the same way, just slower). Per
a-plus-quality-plan anti-pattern #3 ("optimize before workloads
exist"), the ftrace fast path is Q4-style tuning. Ship int3
kprobes now; opt-in the ftrace fast path later if profiling ever
shows it matters.

### Commit 1 — HAVE_KPROBES via int3 + single-step

Lands the int3 breakpoint + single-step emulation path. This is
the "big" commit in the series.

- `arch/um/include/asm/kprobes.h`: arch header (MAX_INSN_SIZE,
  BREAKPOINT_INSTRUCTION, `struct arch_specific_insn`,
  `struct kprobe_ctlblk`, flush_insn_slot).
- `arch/um/kernel/kprobes/Makefile`: builds kprobes.o.
- `arch/um/kernel/kprobes/core.c`: arch_prepare_kprobe (allocate
  insn slot, copy original bytes), arch_arm_kprobe (write 0xCC
  using B-04 helpers), arch_disarm_kprobe (restore original
  byte), arch_remove_kprobe, setup_singlestep (set TF, point IP
  at insn slot), kprobe_handler (the int3 entry point),
  post_kprobe_handler, kprobe_debug_handler.
- `arch/um/kernel/trap.c`: extend `relay_signal()` — on
  SIGTRAP from kernel mode, call kprobe_int3_handler() (for
  int3) or kprobe_debug_handler() (for single-step trap);
  panic only if unhandled.
- `arch/um/Kconfig`: drop `generic-y += kprobes.h`, select
  `HAVE_KPROBES`, `HAVE_KRETPROBES` (trampoline stubbed until
  commit 2).
- Validation: `modprobe kprobe_example` loads, fires on
  `do_sys_open`, prints the entry message. Remove cleanly.

### Commit 2 — Kretprobes trampoline

Delivers return-probe support. Requires a trampoline that
captures the return from every probed call and dispatches to
user handlers.

- `arch/um/kernel/kprobes/core.c`: add `kretprobe_trampoline`
  assembly (saves regs, calls `__kretprobe_trampoline_handler`,
  restores), `arch_prepare_kretprobe` (install trampoline as
  the return address, save original in per-task ret-stack),
  `arch_deref_entry_point`.
- Validation: `modprobe kretprobe_example` on
  `kernel_clone` — reports return values. Trampoline balances
  (no leaked frames after 1000 probes).

### Commit 3 — HAVE_FUNCTION_GRAPH_TRACER (supersedes D27)

Lifts the function_graph deferral from C-05's D27 using the
same return-trampoline pattern kretprobes just validated.

- `arch/um/kernel/mcount.S`: add `ftrace_graph_caller` and
  `return_to_handler` trampolines.
- `arch/um/kernel/ftrace.c`: `prepare_ftrace_return` (overwrite
  return address, push real return address to the current
  task's `ret_stack`).
- `arch/um/include/asm/ftrace.h`: `FTRACE_GRAPH_TRAMP_ADDR`,
  `HAVE_FUNCTION_GRAPH_FUNC`.
- `arch/um/Kconfig`: `select HAVE_FUNCTION_GRAPH_TRACER if
  DYNAMIC_FTRACE`.
- Updates decisions-log D27 to "superseded by C-04 commit 3 +
  commit 5 stress test; see D32."
- Validation: `echo function_graph > current_tracer` works in
  research profile; trace output shows call graphs with
  enter/exit events.

### Commit 4 — research profile enables kprobes + graph

- `arch/um/configs/profiles/research.config`:
  `CONFIG_KPROBES=y`, `CONFIG_KRETPROBES=y`,
  `CONFIG_FUNCTION_GRAPH_TRACER=y`.

### Commit 5 — kprobes-stress selftest

Empirical validation that the D27 concern (function_graph
trampoline racing SIGALRM) does not manifest.

- `tools/testing/selftests/um/kprobes-stress/`: guest-side
  script enables function_graph, registers kretprobes on hot
  functions, runs a workload (`ls /` in a loop, syscall-heavy),
  and asserts no lockdep splat / no KASAN report / no kernel
  oops after 10 000 iterations. Reports `KPROBES_STRESS: PASS
  iters=10000 lockdep=clean kasan=clean`.
- Pattern mirrors the existing `ftrace-smoke` selftest
  (host-side launcher, guest-side init script, PASS/FAIL line).

### Commit 6 — docs + landed status

- `Documentation/virt/uml/kprobes.rst` (new): user-facing doc
  describing the four entry points (int3, ftrace, kretprobe,
  graph), how to use them from tracefs / bpftrace / perf, the
  research-profile enablement, and the empirical D27
  supersession.
- `Documentation/virt/uml/profiles/research.rst`: add kprobes +
  function_graph to the "What's on" section.
- `Documentation/virt/uml/redesign/03-profiles/README.md`:
  matrix row for research gains `kprobes`, `kretprobes`,
  `function_graph`.
- Task status header flipped to landed with evidence (commits,
  selftest pass line, boot times).

## Deliverable

- `arch/um/kernel/kprobes/{core.c,ftrace.c}` (~700 LOC total)
- `arch/um/kernel/mcount.S` extended with `ftrace_regs_caller`,
  `ftrace_graph_caller`, `return_to_handler`,
  `kretprobe_trampoline`
- `arch/um/kernel/ftrace.c` extended with `prepare_ftrace_return`
- `arch/um/include/asm/kprobes.h` (new)
- `arch/um/include/asm/ftrace.h` extended for WITH_REGS +
  function_graph
- `arch/um/kernel/trap.c` hooks `relay_signal()` for kernel-mode
  SIGTRAP
- Kconfig selects: `HAVE_KPROBES`, `HAVE_KPROBES_ON_FTRACE`,
  `HAVE_KRETPROBES`, `HAVE_FUNCTION_GRAPH_TRACER`,
  `HAVE_DYNAMIC_FTRACE_WITH_REGS`
- `arch/um/configs/profiles/research.config` enables the new
  features
- `tools/testing/selftests/um/kprobes-stress/` selftest harness
- `Documentation/virt/uml/kprobes.rst` user doc
- D27 superseded; D32 captures the rationale

## Validation

- `kprobe_example.ko` and `kretprobe_example.ko` (from
  `samples/kprobes/`) load, fire, report, unload cleanly.
- `echo function_graph > /sys/kernel/tracing/current_tracer`
  produces call-graph output.
- bpftrace one-liner such as
  `bpftrace -e 'kprobe:__x64_sys_openat { printf("%s\n", comm); }'
  works (requires C-06 BPF JIT for full bpftrace, but the
  kprobe-registration path validates independently).
- `tools/testing/selftests/um/kprobes-stress/`: PASS, 10k
  iterations lockdep-clean + KASAN-clean.
- Boot matrix (`uml-boot-matrix.sh`) passes all three backend
  dispatch modes.
- Q1 quality bar (`uml-quality-q1.sh research`) clean.

## Open questions

- **Q1 (resolved pre-implementation 2026-04-19):** single-step
  emulation — UML already propagates `X86_EFLAGS_TF` through
  the trap path (see Green-light 2 above); no new emulation
  needed.
- **Q2 (resolved 2026-04-19):** kretprobes — plan to add them;
  C-05's D27 concern about return-trampolines also applied here,
  but per D32 we validate empirically in commit 5 rather than
  defer indefinitely.
- **Q3 (open, expected to resolve during commit 5):** do the
  seccomp backend and the ptrace backend behave identically
  under heavy kprobe + graph load? The stress test runs against
  both (via the boot matrix) to find out.
- **Q4 (follow-up):** optimized kprobes (`arch/x86/kernel/
  kprobes/opt.c` replaces the int3 with a direct jump to the
  handler, faster path) — out of scope for C-04. Opens as a
  future perf-tuning task after representative workloads exist
  (per a-plus-quality-plan anti-pattern #3).

## Risk

- **Single-step through UML's stub paths.** C-04's original spec
  risk. Green-light 2 above says TF propagation is fine; the
  residual risk is that a kprobe placed inside one of UML's
  own stub sequences (e.g., syscall dispatch stubs in
  `arch/um/kernel/skas/`) triggers infinite re-entry. Mitigation:
  CFLAGS_REMOVE on stub TUs (mirroring what C-05 did for ftrace);
  also kprobe_blacklist the stub function names explicitly.
- **kretprobes trampoline signal race (original D27 concern).**
  Commit 7's stress test is the empirical validation. If it
  surfaces a concrete failure, we narrow-fix (e.g., brief
  signal masking across the trampoline window) or defer only
  the failing piece with specific evidence. Either outcome is
  preferable to the theoretical deferral D27 was carrying.
- **Interaction with KASAN + PROVE_LOCKING on research.** The
  research profile has both active; kprobe handlers may be
  called in contexts that lockdep-annotate unusually. Mitigation:
  stress test runs on research profile (full debug surface) so
  this is validated before landing.

## Cross-references

- C-05 (ftrace port): `02-workstreams/C-profiles-and-gaps/
  05-port-ftrace.md`
- Decisions log entries landing during this workstream: D32
  (scope + D27 supersession), possibly D33 if stress test
  surfaces an unexpected finding.
- `arch/x86/kernel/kprobes/{core.c,ftrace.c,opt.c}` — primary
  reference implementation.
- `arch/um/kernel/trap.c:417` (`relay_signal()`) — the hook site.
- `arch/x86/um/os-Linux/mcontext.c:81..84` — TF propagation
  source.
- `Documentation/livepatch/reliable-stacktrace.rst` — upstream
  acknowledgment that return_to_handler frames are architecturally
  tricky but survivable (cited against D27's deferral concern).
