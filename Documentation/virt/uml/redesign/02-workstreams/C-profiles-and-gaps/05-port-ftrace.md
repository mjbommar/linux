# C-05: Port ftrace to UML

**Status:** planned (design pass 2026-04-18 — see "Approach" below
for the concrete slice plan; implementation pending)
**Effort:** 4 weeks (function tracer + dynamic ftrace); function
graph deferred to a follow-up task per D27
**Dependencies:** B-04 (.text section split, landed 2026-04-18 —
see `arch/um/include/asm/patchable.h` and `arch/um/kernel/section_split.c`)
**Blocks:** research profile having function tracer; trace events
become useful at function granularity; unblocks
`HAVE_KPROBES_ON_FTRACE` path in C-04

## Goal

`select HAVE_FUNCTION_TRACER` and `HAVE_DYNAMIC_FTRACE` for UML
on x86_64. Enables `tracefs`, `function` tracer, and the
runtime-patched nop↔call machinery that every subsequent trace
feature builds on.

**Out of scope for this task** (per D27):
`HAVE_FUNCTION_GRAPH_TRACER`, which rewrites return addresses on
the kernel stack and has a concrete race against UML's signal-
based preemption. Function graph lands in a follow-up after a
signal-stress test validates the interaction.

**Prior art check (2026-04-18):** no prior patch series
proposing `HAVE_FUNCTION_TRACER` for UML surfaced on
`lore.kernel.org`. This is a greenfield port; expect extra
reviewer scrutiny accordingly.

## Approach

### The slice plan (six compile-boundary commits, per D23)

Each commit builds and boots cleanly under `ARCH=um` (gcc) and
`ARCH=um LLVM=1`. Each commit is the minimum unit that leaves
the tree consistent.

1. **B-04 extension — `um_kernel_text_patch_begin/end` helpers.**
   - Extend `arch/um/include/asm/patchable.h` with a new
     helper pair scoped to `[_stext, _etext)` and validated
     single-page. Existing `um_text_patch_begin/end` stays as
     is (scoped to `.um_patch_text`).
   - Implement in `arch/um/kernel/section_split.c` alongside
     the existing helpers; reuse `os_protect_memory()`.
   - No Kconfig change yet; no caller yet. Builds are a no-op
     in behavior.
   - A KUnit self-test for the helpers is the minimum evidence
     that they work (sparse/smatch pass + one `mprotect`
     round-trip on a decoy page).
   - Motivation is preparatory: reviewers see the patchable
     infrastructure before the ftrace machinery that needs it.

2. **Arch ftrace header + trampoline + patching ops.**
   - `arch/um/include/asm/ftrace.h` (minimal: `MCOUNT_INSN_SIZE`,
     `ftrace_call_adjust()`, `struct dyn_arch_ftrace`).
     UML's ftrace does **not** expose `__fentry__` — we use
     compiler-emitted NOP5 via `-fpatchable-function-entry=5,0`
     (per D29), not `-pg -mfentry`. The patched 5 bytes become
     a direct `call ftrace_caller`; there is no `__fentry__`
     indirection.
   - `arch/um/kernel/mcount.S` — just `ftrace_caller` and
     `ftrace_stub`. Model on the subset of
     `arch/x86/kernel/ftrace_64.S` that `ftrace_caller` needs
     (save mcount regs → load `function_trace_op` → indirect
     `call ftrace_ops_list_func` → restore → RET). No
     `__fentry__` thunk, no `ftrace_regs_caller`, no
     `ftrace_graph_caller`.
   - `arch/um/kernel/ftrace.c` with real implementations:
     `ftrace_make_nop`, `ftrace_make_call`, `ftrace_modify_call`,
     `ftrace_update_ftrace_func`, `arch_ftrace_update_code`.
     Patching sequence per D28: acquire `text_mutex`,
     `stop_machine_cpuslocked()`, new
     `um_kernel_text_patch_begin(addr, 5)`, `memcpy`,
     `um_kernel_text_patch_end(addr, 5)`, return. No
     `text_poke_bp`; no INT3 emulation.
   - `arch/um/Makefile`: mirror arm64/riscv/parisc pattern —
     ```
     ifeq ($(CONFIG_DYNAMIC_FTRACE),y)
       KBUILD_CPPFLAGS += -DCC_USING_PATCHABLE_FUNCTION_ENTRY
       CC_FLAGS_FTRACE := -fpatchable-function-entry=5,0
     endif
     ```
   - `arch/um/kernel/Makefile`: add
     `CFLAGS_REMOVE_ftrace.o = $(CC_FLAGS_FTRACE)` and the
     equivalent for early-boot TUs (audit against
     `arch/x86/kernel/Makefile`; expected: at minimum
     `early_printk.o`, the boot startup path, and any TU
     whose compile-time runs before `ftrace_init()` reaches
     `arch_ftrace_update_code`).
   - `arch/um/Kconfig`:
     ```
     select HAVE_FUNCTION_TRACER
     select HAVE_DYNAMIC_FTRACE
     select FTRACE_MCOUNT_USE_PATCHABLE_FUNCTION_ENTRY \
         if DYNAMIC_FTRACE
     ```
   - Kernel-wide `CC_FLAGS_FTRACE` override is the standard
     kernel mechanism — see arch/arm64/Makefile:142, 145 for
     the exact pattern UML replicates.
   - No `scripts/recordmcount` dependency. The compiler emits
     `__patchable_function_entries` directly; the generic
     ftrace core reads it. Confirmed by `objdump -h` on a
     one-TU probe during the design pass.
   - Verification during the commit: build with
     `CONFIG_DYNAMIC_FTRACE=y`, run
     `objdump -h vmlinux | grep __patchable_function_entries`
     — section must be nonzero. Build both `ARCH=um` (gcc) and
     `ARCH=um LLVM=1` (clang) clean.

3. **Research profile defconfig enables it.**
   - `arch/um/configs/profiles/research.config`:
     add `CONFIG_FUNCTION_TRACER=y`,
     `CONFIG_DYNAMIC_FTRACE=y`.
   - `arch/um/configs/profiles/fuzz.config` is **NOT** changed
     in this task. The fuzz profile's existing design intent
     (comment at the top of `fuzz.config`) is to stay minimal —
     "We enable FTRACE just enough to unlock USER_EVENTS; no
     other tracers turn on." Function tracer adds a 5-byte NOP
     to every traced function, which is near-free when off but
     still pays an i-cache cost at fuzz-reboot density. Fuzz
     consumers who want function tracing in a fuzz context can
     either opt in at `make menuconfig` time or use the
     `research` profile for a targeted fuzz-with-trace pass.
     Fuzz-deep follows the same rationale.
   - **Invariant check (already satisfied 2026-04-18):**
     `arch/um/configs/profiles/prod-fast.config` and
     `arch/um/configs/profiles/sandbox.config` both contain
     `# CONFIG_FTRACE is not set`. The per-function `-pg`
     mcount overhead and the D28 patch window are therefore
     not present in those profiles. C-05 must not regress
     this. The profile matrix in
     `03-profiles/README.md` should be updated in commit 6 to
     name the invariant explicitly ("`DYNAMIC_FTRACE` = on in
     research, off everywhere else").

4. **Selftest harness row.**
   - Add `tools/testing/selftests/um/ftrace-smoke/` (or extend
     the existing UML selftests layout — check
     `tools/testing/selftests/um/` for the current pattern).
   - At minimum: boot a research-profile UML, write `function`
     to `current_tracer`, read `trace`, assert at least one
     kernel function is recorded, no oops/WARN in dmesg.
   - Long-form: run a curated subset of
     `tools/testing/selftests/ftrace/test.d/` that doesn't
     depend on features we don't support yet (skip
     function_graph tests, skip direct-call tests).

5. **Benchmark baseline.**
   - Run `uml-perf-compare.sh` on the research profile with
     tracer off. Record the `-pg` mcount-nop static cost in the
     `perf-baseline.json` delta. Expected: <1% regression when
     tracing is off (the NOP5 on a non-taken path is near-free).
     If regression >2%, stop and investigate — probably an
     mcount stub escaped a `CFLAGS_REMOVE`.
   - Record tracer-on cost on a `getpid()` microbench. Expected:
     ~50–100 ns per traced call at function granularity; reports
     the cost of the ftrace dispatch (ftrace_caller + ops-list
     walk), not UML-specific.

6. **Docs + decisions.**
   - `Documentation/virt/uml/profiles/research.rst`: note
     tracer availability and tracefs path.
   - `Documentation/virt/uml/ftrace.rst` (new): one-pager on
     what works, what doesn't (function-graph deferred),
     CFLAGS_REMOVE rationale, patching mechanism (cross-ref
     D28 and B-04).
   - Decisions log: D27 (scope split — graph deferred) and D28
     (patching mechanism — simple mprotect + memcpy, no INT3).

### Why this order

- Commit 1 is a self-contained B-04 extension. Reviewers
  familiar with B-04's `.um_patch_text` scope see the companion
  helpers for kernel-text-wide patching, with the validation
  invariants stated alongside B-04's. No new Kconfig.
- Commit 2 is the ftrace port proper: header + trampoline +
  patching ops + Makefile flag override + Kconfig selects, all
  together because they are functionally inseparable (the Kconfig
  selects reference the Makefile flag which depends on the
  trampoline which uses the commit-1 helpers). Splitting commit
  2 further produces patches that don't compile-to-a-useful-state
  individually.
- Commits 3–6 are additive: profile default, tests, benches,
  docs.

### What this task explicitly does NOT do

- No `HAVE_FUNCTION_GRAPH_TRACER`. See D27. Graph needs return-
  address rewrite on the kernel stack; UML's SIGALRM-driven
  preemption can race with the trampoline's in-flight state.
  Not unsolvable; not in scope here.
- No `HAVE_DYNAMIC_FTRACE_WITH_REGS` /
  `HAVE_DYNAMIC_FTRACE_WITH_ARGS`. Those require a REGS
  variant of the mcount caller that spills full `pt_regs`.
  Add later when a consumer needs it (kprobes-on-ftrace in
  C-04 will want REGS; fold it into C-04's patch series, not
  this one).
- No `HAVE_DYNAMIC_FTRACE_WITH_DIRECT_CALLS`. BPF-specific,
  C-06 concern.
- No `HAVE_FTRACE_REGS_HAVING_PT_REGS`. Follows from the REGS
  variant above.

## Deliverable

- `arch/um/kernel/mcount.S` (fentry thunk, function tracer
  caller; no graph return trampoline)
- `arch/um/kernel/ftrace.c` (patching ops, `text_mutex` +
  `stop_machine_cpuslocked` + new helpers below)
- `arch/um/include/asm/patchable.h` extended with
  `um_kernel_text_patch_begin/end` (kernel-text-wide, single
  page, `[_stext, _etext)` validated) alongside B-04's existing
  `um_text_patch_begin/end` (scoped to `.um_patch_text`)
- `arch/um/include/asm/ftrace.h` (if anything arch-specific
  beyond the generic header is required; otherwise keep
  deferring to generic)
- Kconfig selects: `HAVE_FUNCTION_TRACER`, `HAVE_DYNAMIC_FTRACE`
- Research and fuzz profile fragments updated; prod-fast and
  sandbox invariants (`# CONFIG_FTRACE is not set`) preserved
- `tools/testing/selftests/um/ftrace-smoke/` harness
- KUnit patching stress test (10k nop↔call flips on a decoy
  site under spurious-signal load; assert no corruption, no
  lockdep splat)
- `Documentation/virt/uml/ftrace.rst` + profile doc update

## Validation

- `echo function > /sys/kernel/debug/tracing/current_tracer`
  succeeds; `cat trace` shows traced events.
- `set_ftrace_filter` works (whitelist one symbol; trace only
  that).
- `ftracetest` selftest subset (function + dynamic, excluding
  graph/direct) passes.
- Boot matrix PTRACE_ONLY / SECCOMP_ONLY / DYNAMIC all boot
  with tracer off AND with tracer on.
- `uml-perf-compare.sh`: research profile tracer-off within
  2% of pre-series baseline. Tracer-on cost characterized in
  commit 5's message.
- Q1 quality bar: gcc + clang + sparse + smatch clean vs the
  research-profile baseline (per
  `scripts/uml-quality-q1-diff.sh`).
- `checkpatch.pl --strict` clean on each commit.

## Open questions

- **Q1 (resolved by D27):** function-graph — deferred, not
  skipped forever.
- **Q2 (resolved):** `-mrecord-mcount` vs post-link
  `recordmcount` vs `-fpatchable-function-entry`. GCC ≥ 5
  accepts `-mrecord-mcount` and the kernel's
  `FTRACE_MCOUNT_USE_CC` path takes it. **Clang rejects
  `-mrecord-mcount` on x86_64** (it is a SystemZ-only flag
  upstream in LLVM — see `reviews.llvm.org/D71627`), so the
  `cc-option` probe returns false under `LLVM=1` and the build
  falls through to `FTRACE_MCOUNT_USE_OBJTOOL` if objtool is
  available for UML or otherwise to `FTRACE_MCOUNT_USE_RECORDMCOUNT`
  (the post-link tool, which handles `EM_X86_64` relocations
  unmodified — see `scripts/recordmcount.c:565..607`). No
  UML-specific Kconfig selection needed for either toolchain.
  Confirm during commit 2 that both `ARCH=um` and
  `ARCH=um LLVM=1` builds of a `DYNAMIC_FTRACE=y` config
  produce a populated `__mcount_loc` section (`objdump -h vmlinux
  | grep __mcount_loc`; size must be nonzero).
- **Q3:** UML has a pre-existing `unprofile` macro in
  `arch/um/scripts/Makefile.rules:24..27` that strips `-pg`
  from USER TUs (host-side objects that call into libc and
  must not reach the kernel's mcount path). Confirm this is
  still strictly correct after we add mcount stubs; USER TUs
  compile against `USER_CFLAGS`, not kernel `CFLAGS`, so in
  principle they never had `-pg` to strip anyway. If they did
  for gprof builds, `unprofile` handles it.
- **Q4:** which subset of `tools/testing/selftests/ftrace/`
  passes unmodified under UML? Needs a shakedown pass; any
  failures drive follow-up fixes or explicit skip rules.

## Risk

- **Patching path correctness.** The `text_mutex + stop_machine +
  page-scoped mprotect` mechanism (D28) is well-defined but the
  details matter. The key invariants: (a) every patch is under
  both `text_mutex` and `stop_machine_cpuslocked`; (b) the RW
  window covers exactly one 4 KB page, validated to lie within
  `[_stext, _etext)`; (c) `stop_machine` is not skipped for
  "small" single-site patches — the bare-metal arm/arm64/riscv
  equivalents avoid it because they use fixmap aliasing, which
  UML cannot replicate. Mitigation: a KUnit test in the same
  commit series that patches a decoy site 10k times under
  fuzzer-induced load (spurious signals, contending vCPUs) and
  asserts no `.text` corruption and no lockdep splat.
- **Memory-safety blast radius during the window.** During
  `stop_machine`, no UML kernel path is running that could issue
  a stray write, so the window is secure against internal
  memory-safety bugs for its duration. If the UML redesign later
  grows a bottom-half path that bypasses the scheduler (e.g. a
  signal handler reaching kernel C without going through
  `irq_enter`/`irq_exit`), this invariant must be re-audited.
  Call this out in any invariant doc touched by C-05.
- **Early-boot mcount recursion.** Every TU that runs before
  `ftrace_init()` must have `-pg` stripped. An escaped stub
  would fault early in boot because `ftrace_caller` hasn't been
  wired up. Mitigation: mirror the x86 CFLAGS_REMOVE list
  faithfully; boot-matrix a `CONFIG_FUNCTION_TRACER=y` +
  `CONFIG_DYNAMIC_FTRACE=n` config during commit 1 to surface
  any missing removals before commit 2 lands the real patcher.
- **Sparse/Smatch on mcount.S.** Assembly files don't run
  through sparse or smatch, so the asm thunk gets no static
  analysis. Mitigation: keep it minimal — model exactly on
  x86's `ftrace_64.S` fentry path and resist any clever
  deviation. Differences land in the decisions log.

## Cross-references

- `Documentation/virt/uml/redesign/02-workstreams/B-static-key-hot-paths/04-section-split.md`
  (the unblocking workstream; C-05 extends its helper API)
- `arch/um/include/asm/patchable.h` (existing `um_text_patch_*`
  scoped to `.um_patch_text`; C-05 adds
  `um_kernel_text_patch_*` scoped to `[_stext, _etext)`)
- `arch/x86/include/asm/text-patching.h:134..216` (`CONFIG_UML_X86`
  excludes UML from INT3 emulation; rationale for D28)
- `arch/arm/kernel/patch.c`, `arch/arm64/kernel/patching.c`,
  `arch/riscv/kernel/patch.c` (fixmap-based per-page alias
  precedent; not replicable on UML per D28)
- `arch/x86/kernel/alternative.c:2503..2637` (`text_poke_mm`
  dedicated-mm alias; also not replicable on UML — cited in D28
  as forward-looking hardening if C-10 lands)
- `Documentation/virt/uml/redesign/04-risks/decisions-log.md`
  D19 (JIT deferred to B-04; superseded by this task landing),
  D27 (graph deferred), D28 (patching mechanism, post-security-
  review)
- `arch/x86/kernel/ftrace.c`, `arch/x86/kernel/ftrace_64.S`
  (primary reference implementation)
- `scripts/recordmcount.c:565..607` (x86_64 handling; unchanged
  for UML)
- `Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/10-host-launcher-crosvm.md`
  (future hardening path: patch via host helper process)
- `Documentation/livepatch/reliable-stacktrace.rst`
  (upstream ack of function_graph unwinder tricksiness; D27
  rationale)
