# C-04: Port kprobes to UML

**Status:** partially landed (2026-04-20), commit 3 progress
(2026-04-21). HAVE_KPROBES (commits 1a–1d) and HAVE_RETHOOK/
KRETPROBES (commit 2) are in the tree and tested. Commit 3
(HAVE_FUNCTION_GRAPH_TRACER) is progressing in-tree under D45's
in-fork scope policy:
- **Source (3) closed** (commit `a2e01ee58c53`, 2026-04-21):
  narrow `notrace` annotations on `kernel/kthread.c::kthread`
  and `kernel/smpboot.c::smpboot_thread_fn` prevent the
  shadow-stack push that otherwise leaks because those wrappers
  end in `do_exit` rather than a return. `uml/research` build
  + `userspace-smoke` selftest pass with the notrace landed and
  function_graph deselected — the annotations are a pure win
  regardless of the rest.
- **Sources (1) and (2) still open**: the arch/um patch-site
  stripping (strip `-fpatchable-function-entry` from
  `arch/um/kernel/`, `arch/um/kernel/skas/`, and USER_CFLAGS)
  plus the `ftrace_graph_caller` + `return_to_handler`
  trampolines in `arch/um/kernel/mcount.S` sit in the git stash
  entry `c04-c3-wip-after-arch-strip-still-crashes`. Applying
  the stash as-is causes `userspace-smoke` init to segfault at
  IP=0 SP=0 — a new regression the 2026-04-19 investigation
  didn't surface because that session was focused on graph-
  active crashes, not boot-without-graph. Root cause to be
  identified in a follow-up session; candidates are the
  USER_CFLAGS filter (affects stub-loading TUs), the broad
  `ccflags-remove-y` (affects ftrace's view of arch/um/kernel/
  functions), or a combination.
**Effort:** 3–4 focused days for commits 1+2 (actual: ~2 days);
commit 3 split across two sessions — session 1 (2026-04-19)
surfaced the three-source leak; session 2 (2026-04-21) landed
source (3) and identified the new boot regression blocking
sources (1)+(2). Session 3 to investigate and close.
**Dependencies:** B-04 (.text section split, landed; helpers
extended by C-05 commit 1), C-05 (ftrace port, landed 2026-04-19
— provides the mcount.S / ftrace.c baseline that commits 1, 3,
and 5 extend).
**Blocks:** research profile having function_graph
(kprobes/kretprobes already unblocked); bpftrace's
`fentry:`/`fexit:`; future selftests that exercise the graph
surface.

## Goal

`select HAVE_KPROBES` and `HAVE_RETHOOK` (the latter auto-activates
`KRETPROBES` / `KRETPROBE_ON_RETHOOK`) for UML on x86_64. Delivers
entry probes + mid-function probes (int3 + single-step) and return
probes (rethook shadow stack). `HAVE_FUNCTION_GRAPH_TRACER` is
**out of scope** per D34; see the §"Commit 3 — deferred" section
below for the full analysis. The vision in `00-vision.md` calls
out "all sanitizers and tracing on" for the research profile;
C-04 closes the "tracing" half of that target except for the
graph tracer, which remains a known gap until the cross-subsystem
fix lands.

Original goal text (kept for historical context — supersede with
the §"Commit 3 — deferred" note below for current scope):

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

### Commit 2 — Kretprobes via rethook (revised 2026-04-19)

Delivers return-probe support. Implemented as a **rethook**
port, not the legacy `arch_prepare_kretprobe` /
`kretprobe_trampoline` pair that the earlier draft of this
section called for. Upstream x86_64 switched to rethook several
cycles ago (`arch/x86/kernel/rethook.c`, 128 lines); the legacy
`HAVE_KRETPROBES` shape is effectively a backward-compat stub
at this point. `arch/Kconfig:252` auto-selects
`CONFIG_KRETPROBE_ON_RETHOOK` whenever `HAVE_RETHOOK` is
selected, so `select HAVE_RETHOOK` is enough to bring up the
entire kretprobe surface. See D33 for the pivot rationale.

**UML pt_regs layout is trampoline-compatible with x86 native.**
`arch/x86/um/user-offsets.c` derives `HOST_xxx` indices from the
host `struct user_regs_struct`, which for the fields the
trampoline touches (ax, bx, cx, dx, si, di, bp, r8-r15, orig_ax,
ip, cs, flags, sp, ss) is byte-for-byte identical to the
in-kernel x86_64 `struct pt_regs` layout. Therefore the
register-save assembly from `arch/x86/kernel/rethook.c` produces
a memory image that UML's `UPT_IP()` / `UPT_SP()` / `UPT_EFLAGS()`
macros can read correctly — the kretprobe trampoline can be
ported almost verbatim, rewriting only the C callback to use
`UPT_xxx(&regs->regs)` accessors instead of native `regs->xxx`
member access.

Deliverable:

- `arch/um/kernel/rethook.c` (new): port of
  `arch/x86/kernel/rethook.c`.
  - Inline asm trampoline `arch_rethook_trampoline` saves regs
    in the x86-native pt_regs order (matches UML gp[] indexing).
  - `arch_rethook_trampoline_callback(struct pt_regs *)` uses
    `UPT_xxx` accessors; calls `rethook_trampoline_handler()`.
  - `arch_rethook_prepare` writes trampoline address to
    `stack[0]` at function entry.
  - `arch_rethook_fixup_return` writes the real return address
    back at the end of handler dispatch.
- `arch/um/kernel/Makefile`: `obj-$(CONFIG_RETHOOK) += rethook.o`
  with `CFLAGS_REMOVE_rethook.o = -pg` so the trampoline itself
  isn't traced.
- `arch/x86/um/shared/sysdep/ptrace.h`: add `UPT_ORIG_AX` macro
  (the existing UPT_ family doesn't define one; the trampoline
  callback sets orig_ax = ~0UL to signal "not a syscall").
- `arch/um/Kconfig`: `select HAVE_RETHOOK if X86_64`. Drops the
  `HAVE_KRETPROBES if X86_64` precedent — `KRETPROBES` is pulled
  in by `HAVE_RETHOOK` via `arch/Kconfig:250`.
- `arch/um/kernel/kprobes/core.c`: the `kretprobe_blacklist[]`
  stub (landed in commit 1a) stays; nothing else changes here.

Validation:

- `samples/kprobes/kretprobe_example.ko` loads, fires on
  `kernel_clone`, reports return values, unloads cleanly.
- 1000-iteration stress loop (open/close or fork) with an
  active kretprobe — no leaked frames, pre/post balanced.
- `/sys/kernel/debug/kprobes/list` shows active kretprobes.
- Trampoline symbol blacklisted:
  `cat /sys/kernel/debug/kprobes/blacklist | grep rethook` lists
  `arch_rethook_trampoline`.

Scope discipline:

- No changes to `arch/um/kernel/kprobes/core.c` beyond the
  blacklist entry (already landed).
- No mcount.S edits; function_graph lands in commit 3 via its
  own trampoline (same register-save pattern, different
  dispatch target).

### Commit 3 — progressing (HAVE_FUNCTION_GRAPH_TRACER)

**Status: source (3) landed 2026-04-21 under D34's 2026-04-21
addendum (partial resolution under D45); sources (1) and (2)
blocked on a newly-identified boot regression, session 3 to
close.**

The kernel/kthread.c + kernel/smpboot.c `notrace` annotations
(source (3)) landed in commit `a2e01ee58c53`. They are a pure
win even without the rest of commit 3 — they prevent the
unterminated graph-shadow-stack push in two well-known
never-returning wrappers, on every architecture, not just UML.
See `04-risks/decisions-log.md` D34 2026-04-21 addendum for
the scope policy that permitted them to land on the fork.

Sources (1) and (2) remain blocked. The stashed WIP
(`c04-c3-wip-after-arch-strip-still-crashes`) applies
`ccflags-remove-y := $(CC_FLAGS_FTRACE)` broadly across
`arch/um/kernel/` and `arch/um/kernel/skas/`, filters the same
flag out of `USER_CFLAGS` in `arch/um/Makefile`, and adds the
`ftrace_graph_caller` + `return_to_handler` trampolines in
`arch/um/kernel/mcount.S`. That combination builds clean but
fails `userspace-smoke` at boot: init segfaults at IP=0 SP=0
before the smoke script runs. The regression happens with
`CONFIG_FUNCTION_GRAPH_TRACER=n` too, so it is not a graph-
trampoline bug — it is caused by the patch-site stripping
itself. 2026-04-19's session didn't hit this because that
session never ran `userspace-smoke` under the stripped-arch
build.

Session-3 plan:

1. Narrow the strip: start from the pre-strip (committed) arch/um
   Makefiles, then progressively strip individual files
   (process.c, time.c, irq.c, …) until `userspace-smoke` breaks.
   The first broken file names the culprit. Likely candidates
   are `arch/um/os-Linux/` TUs via USER_CFLAGS — those build
   the seccomp stub loader and signal dispatcher.
2. Once the culprit is isolated, choose the minimum strip set
   that (a) closes sources (1)+(2) and (b) keeps
   `userspace-smoke` green.
3. Land the minimum strip set + trampolines + Kconfig select in
   one bisectable commit with the smoke test as validation.

Three distinct leak sources identified and measured (sources
(1) and (2) still open; source (3) closed in a2e01ee58c53):

1. Signal dispatch unwound via `rt_sigreturn` (not `ret`) — leaks
   one shadow-stack entry per signal delivery. Fixable by
   stripping patch sites from `arch/um/os-Linux/signal.c` etc.
2. `new_thread_handler` / `fork_handler` entered via
   `kernel_longjmp` — leaks one entry per task creation. Fixable
   by stripping patch sites from `arch/um/kernel/process.c`.
3. Generic `kthread()` / `smpboot_thread_fn()` in `kernel/kthread.c`
   end in `do_exit` and never return — leak one entry per kthread
   creation. **Closed (commit `a2e01ee58c53`, 2026-04-21) by
   adding `notrace` to those two functions** in
   `kernel/kthread.c` and `kernel/smpboot.c`. On native x86 this
   is a no-op (the graph leak there is harmless because frame
   unwinding doesn't dereference stale ret_stack entries); on
   UML it prevents the push that otherwise gets popped via a
   longjmp'd task's graphed-function return and jumped through
   as garbage. See D34's 2026-04-21 addendum for the merge-
   surface tracking.

Sources (1) and (2) remain open. The stashed WIP's broad
`ccflags-remove-y` + USER_CFLAGS filter closes them in
principle but also breaks `userspace-smoke` at boot (init
segfaults at IP=0 SP=0 with `CONFIG_FUNCTION_GRAPH_TRACER=n` —
so it is not a trampoline issue, it is the strip itself). The
session-3 plan (above) progressively narrows the strip until
the smoke test stays green.

See D34 for the full analysis including:
- What we tried (`tracing_graph_pause`, patch-site stripping via
  `ccflags-remove-y` and `CFLAGS_<file>.o += -fpatchable-function-entry=0`),
- Exact crash signatures (Segfault with no mm at `schedule+0x4a`
  under exec workload; user-memory access while reading
  `/sys/kernel/tracing/trace` with graph active),
- Alternatives considered and why each falls short.

### Commit 4 — research profile enables kprobes + function tracer

Adjusted per D34 to drop function_graph from the enabled set
(function_graph stays gated until commit 3's sources (1)+(2)
are closed).

- `arch/um/configs/profiles/research.config`:
  `CONFIG_KPROBES=y`, `CONFIG_KRETPROBES=y` (auto-selected via
  HAVE_RETHOOK). `CONFIG_FUNCTION_GRAPH_TRACER` intentionally
  NOT enabled — per D34, leave the Kconfig out-of-select until
  the arch/um strip-set is identified. Document the gap in
  `Documentation/virt/uml/profiles/research.rst`.

### Commit 5 — kprobes-stress selftest

Stress harness for the kprobe / kretprobe / (eventual)
function_graph surface. Originally framed as empirical
validation of D27 (function_graph racing SIGALRM); with graph
deferred per D34, the immediate value of this selftest is the
kretprobes regression guard, with graph exercise written but
gated on `CONFIG_FUNCTION_GRAPH_TRACER=y` so the harness is
ready the moment commit 3 lands.

- `tools/testing/selftests/um/kprobes-stress/`: guest-side
  script registers kretprobes on hot functions (`kernel_clone`,
  `do_sys_openat2`, `ksys_read`), runs a syscall-heavy
  workload (`ls /` in a loop), asserts no lockdep splat / no
  KASAN report / no kernel oops after 10 000 iterations. When
  `CONFIG_FUNCTION_GRAPH_TRACER=y`, additionally enables
  `function_graph` and stresses both surfaces together. Reports
  `KPROBES_STRESS: PASS iters=10000 lockdep=clean kasan=clean
  graph=<on|deferred>`.
- Pattern mirrors the existing `ftrace-smoke` selftest
  (host-side launcher, guest-side init script, PASS/FAIL line).
- Validates that C-04's landed commits (1a–1d + 2) don't
  regress under repeated workload. When someone unblocks
  commit 3, re-running this harness is the green-or-red gate.

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
