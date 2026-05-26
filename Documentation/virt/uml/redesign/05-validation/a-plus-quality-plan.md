# A+ Quality Plan

How to drive the UML redesign toward the highest practical bar for
correctness, safety, maintainability, and performance using the kernel's
existing compiler, analysis, sanitizer, testing, and profiling toolchain.

This document is intentionally ambitious. It is not a claim that every
tool listed here should be enabled in every build. The point is the
opposite: use the full tool chest deliberately, in the right build, at the
right phase, for the right question.

## What "A+ quality" means here

For this project, "A+ quality" means all of the following at once:

- the hot path stays fast in production-oriented profiles;
- safety bugs are aggressively surfaced in debug, fuzz, and research
  profiles;
- compiler- and tool-driven diagnostics catch entire classes of mistakes
  before runtime;
- regressions are hard to introduce silently because CI exercises multiple
  compilers, profiles, and validation depths;
- the project is using the kernel ecosystem's mainstream tools, not
  inventing an ad-hoc local quality culture.

The bar is therefore:

- **performance**: no unmeasured hot-path cost; optimizer experiments are
  workload-driven, not folklore-driven;
- **quality**: every change is checked by style, static analysis, and at
  least one runtime path;
- **safety**: memory, race, and initialization bugs are hunted
  systematically;
- **portability**: both GCC and Clang stay healthy;
- **upstreamability**: the same tool and testing story survives maintainer
  review.

## Core principle

Do **not** chase "all checks in one kernel image."

That is the wrong model. Many tools are mutually expensive or are designed
for different phases:

- `prod-fast` wants minimum hot-path disturbance;
- `prod-with-hooks` wants low-overhead observability;
- `research` wants maximum visibility;
- `fuzz` wants coverage and crash quality;
- `fuzz-deep` wants concurrency and determinism diagnostics;
- milestone / release validation wants a broad matrix, not one magical
  config.

So the A+ strategy is:

1. **dual-compiler builds** for structural quality;
2. **static analysis** for bug classes that should never reach runtime;
3. **sanitizer/profile specialization** for runtime bug hunting;
4. **selftests/KUnit/LTP/fuzzing** for behavior;
5. **representative profiling** for performance work.

## Tool taxonomy

The tools below are grouped by what question they answer.

### 1. Build and compiler diversity

- `GCC`
  Baseline kernel compiler. Keep healthy at all times.
- `Clang/LLVM`
  Second compiler and alternate linker/toolchain surface. Already builds
  `ARCH=um` and should be part of regular validation.
- `Containerized builds`
  Not a code-quality tool by itself, but useful for reproducibility, old
  toolchain / old-userland testing, and CI portability.

### 2. Style and semantic patching

- `checkpatch.pl`
  Patch hygiene and style gate.
- `clang-format`
  Spot tool for touched hunks and large new files, not for tree-wide churn.
- `Coccinelle`
  Semantic patching and API migration sweeps.

### 3. Static analysis

- `Sparse`
  Type and annotation-aware kernel static analysis.
- `Smatch`
  Practical bug finding on real kernel code.
- `Compiler-Based Context Analysis`
  Clang-only context/locking analysis.
- `gcc -fanalyzer`
  Useful adjunct for path-sensitive analysis where it stays low-noise.

### 4. Runtime bug detectors

- `KASAN`
  Out-of-bounds and use-after-free.
- `KMSAN`
  Use of uninitialized values.
- `UBSAN`
  Undefined-behavior detection.
- `KCSAN`
  Data-race detection.
- `KFENCE`
  Low-overhead heap corruption detection.
- `kmemleak`
  Leak hunting.

### 5. Coverage and testing

- `KCOV`
  Coverage-guided fuzzing.
- `gcov`
  Structural code coverage for targeted test accounting.
- `Linux kernel selftests`
  Behavior / subsystem validation.
- `KUnit`
  In-kernel unit and subsystem-level tests.
- `KTAP`
  Standard test output format used by KUnit and other test flows.

### 6. API and model validation

- `UAPI Checker`
  Only relevant when the redesign creates or changes genuine user-visible
  ABI / UAPI surfaces.
- `LKMM`
  Memory-model validation and reasoning aid for concurrency-sensitive code.

### 7. Performance optimization

- `AutoFDO`
  Profile-guided compiler optimization using hardware sampling.
- `Propeller`
  Link-time layout optimization on top of profile data.

### 8. Reference / likely out-of-scope tools

- `Linux Kernel GPIO based sloppy logic analyzer`
  Valuable for some hardware bring-up work, but not meaningfully relevant
  to the UML redesign as currently scoped.

### 9. Developer navigation and comprehension

These tools do not directly prove correctness, safety, or performance, but
they materially improve the quality of engineering decisions by making it
easier to understand a large kernel tree accurately.

- `cscope`
- `ctags`
- `clangd`
- fast structural search tools such as `rg`

## Recommended project stance on each tool

This section turns the raw list into a project policy.

### Mandatory now

These should become part of the default quality bar for active
development.

#### `checkpatch.pl`

Use on every patch and series.

Why it matters here:

- the redesign is broad and refactor-heavy;
- style noise makes architectural review harder;
- we want maintainers reviewing semantics, not indentation and trailer
  mistakes.

Project policy:

- patch/series must be clean or have every warning explained;
- never allow "we'll clean that later" style debt to accumulate.

#### `GCC` + `Clang/LLVM`

Both compilers should build `ARCH=um`.

Why it matters here:

- compiler diversity catches different bugs and layout assumptions;
- this project is manipulating low-level arch code, link sections, and
  hot-path structure;
- `ARCH=um` is documented upstream as maintained for `LLVM=1`.

Project policy:

- every major milestone must build with both compilers;
- `ARCH=um LLVM=1` should be in CI;
- clang-only or gcc-only warnings must be triaged, not ignored forever.

#### `Sparse`

Use continuously.

Why it matters here:

- UML code moves pointers, user/guest state, and arch-private data through
  unusual paths;
- Sparse is already a mainstream kernel expectation.

Project policy:

- touched code should stay Sparse-clean or warning deltas must be justified.

#### `Smatch`

Use continuously.

Why it matters here:

- it is often better than generic compiler warnings at finding practical
  kernel bugs;
- good fit for lifecycle/state-machine mistakes in backend code.

Project policy:

- review all new warnings on touched files;
- keep false positives documented, not hand-waved.

#### `KUnit`

Use for narrow contracts and subsystem-specific behavior.

Why it matters here:

- Workstream A already proves the value with backend contract tests;
- it gives fast local confidence for invariants that do not require a full
  guest boot.

Project policy:

- every new internal contract worth naming should be considered for a KUnit
  test;
- use KUnit especially for backend API contracts, parser logic, state
  transitions, and sanitizer-specific support code.

#### `Linux kernel selftests`

Use for end-to-end feature checks.

Why it matters here:

- many redesign claims are runtime claims, not unit claims;
- profile behavior and runtime toggles belong in selftests.

Project policy:

- anything user-visible or operator-visible should have a selftest if
  feasible;
- profile-defining features should each have at least one selftest.

#### `KTAP`

Use as the standard result format.

Why it matters here:

- standardized output makes automation and CI simpler;
- KUnit already aligns with it.

Project policy:

- prefer KTAP-compatible reporting where a custom test harness emits
  structured results.

### Mandatory in specific profiles or workstreams

These are first-class parts of the roadmap, but not global defaults.

#### `KCOV`

Use in `fuzz`; optional runtime-controlled use in `prod-with-hooks`.

Why it matters here:

- core to the fuzzing story;
- part of Workstream B/C value proposition.

Project policy:

- `fuzz` profile must keep KCOV healthy;
- any KCOV-on path should be validated with real coverage flow, not just
  "it compiled."

#### `KASAN`

Use in `research`, `fuzz`, and related validation kernels.

Why it matters here:

- memory bugs in backend, trap, and mm code are some of the highest-risk
  bug classes in the redesign.

Project policy:

- KASAN-positive synthetic tests should be part of milestone validation;
- regressions in KASAN support are blockers for the research/fuzz story.

#### `UBSAN`

Use in `research`.

Why it matters here:

- catches structural C issues that can survive normal testing.

Project policy:

- research profile keeps UBSAN healthy;
- new UBSAN reports are triaged like real bugs, not shrugged off unless
  proven false-positive or irrelevant.

#### `KCSAN`

Use in `fuzz-deep` and targeted concurrency validation.

Why it matters here:

- backend transitions, SMP, IRQ/state interactions, and future record/replay
  work all increase race risk.

Project policy:

- do not claim SMP robustness without periodic KCSAN validation;
- use KCSAN for concurrency-focused milestone gates, not as a default dev
  kernel.

#### `KFENCE`

Use in `research` / `fuzz` where lower overhead than KASAN is useful.

Why it matters here:

- gives an additional memory-corruption detection mode with different
  trade-offs.

Project policy:

- keep the port healthy once landed;
- use it to widen runtime coverage when KASAN is too heavy.

#### `KMSAN`

Use as a specialized high-value debugging kernel once the UML port lands.

Why it matters here:

- uninitialized-data bugs are exactly the kind of subtle defect that can
  hide in complex backend and low-level control-flow code;
- KMSAN is one of the strongest arguments for keeping Clang healthy.

Project policy:

- treat KMSAN as a milestone-capability, not an always-on CI default;
- use it for periodic sweeps and targeted bug hunts.

#### `kmemleak`

Use as a periodic diagnostic kernel, especially around long-lived profile
and lifecycle testing.

Why it matters here:

- backend/session/context lifecycle work can leak in ways fuzzing will not
  necessarily notice quickly.

Project policy:

- periodic leak sweeps for long-running tests;
- not required in every PR gate.

### Strongly recommended, but scoped

These are valuable, but should be applied carefully.

#### `Coccinelle`

Best when changing interfaces or sweeping patterns.

Why it matters here:

- the redesign has cross-tree API migrations by design;
- semantic patching is much safer than hand-editing dozens of sites.

Project policy:

- use when changing shared APIs, wrappers, annotations, or call-site
  conventions;
- do not force Coccinelle into every small patch.

#### `clang-format`

Use for spot cleanup only.

Why it matters here:

- useful on newly added files and patch-local cleanup;
- harmful if used to generate wide style churn.

Project policy:

- apply to touched regions or new files, not whole subsystems;
- never mix broad reformatting with semantic review patches.

#### `gcov`

Useful for targeted test-accounting, not as the main fuzz/coverage story.

Why it matters here:

- can answer "did this test suite execute the code we think it did?"
- complements KCOV rather than replacing it.

Project policy:

- use for milestone coverage studies or targeted subsystem validation;
- do not build the whole project culture around gcov percentages.

#### `Containerized builds`

Useful for reproducibility and toolchain variance.

Why it matters here:

- makes it easier to exercise old userspace / old host combinations and
  package exact toolchains in CI;
- useful for validating "it builds the same way everywhere."

Project policy:

- use containerized runners for CI and old-toolchain coverage;
- do not confuse this with runtime performance realism.

### High-value future additions

These are where the project can move from "good engineering" to
"exceptionally strong engineering."

#### `Compiler-Based Context Analysis`

This is one of the most promising Clang-specific quality tools for this
project.

Why it matters here:

- the redesign has lock/context-sensitive code in IRQ, scheduler, MM, and
  backend transition paths;
- this analysis can statically verify that required contexts are held or
  absent.

Important constraint:

- kernel docs require **Clang 22+**;
- the local environment today is **Clang 21.1.8**, so this is not yet
  usable here.

Project policy:

- once Clang 22+ is available, pilot it on one narrow area first;
- likely candidates:
  - backend state/lifecycle code,
  - per-mm ownership transitions,
  - scheduler / irq-related UML internals,
  - hook control surfaces.

Do **not** enable tree-wide initially. The upstream docs explicitly warn
that tree-wide enablement is noisy.

#### `LKMM`

Not a runtime tool, but important for concurrency-heavy future work.

Why it matters here:

- once SMP, replay, and more concurrency-sensitive hooks deepen, memory
  ordering questions will matter;
- LKMM gives a principled way to reason about barriers and race semantics.

Project policy:

- use for design review of nontrivial concurrent algorithms and barrier
  placement;
- especially relevant if lockless or per-CPU fast paths grow.

#### `UAPI Checker`

Low priority today, but should be used if the redesign introduces real
userspace ABI surfaces.

Why it matters here:

- most of the redesign is internal or debugfs/profile-driven today;
- if stable UAPI grows later, compatibility checking becomes important.

Project policy:

- out of the default path now;
- move to active use if stable UAPI / ABI becomes a real deliverable.

### Performance-specialized tools

These can materially help performance, but they are not first-wave tools.
Use them after the baseline backend and profile story is stable.

#### `AutoFDO`

Why it matters here:

- official kernel support exists for Clang;
- profile-guided optimization can improve front-end behavior and hot-path
  layout;
- if `prod-fast` and later KVM backend become serious deployment targets,
  this becomes very interesting.

Project policy:

- only use after representative workloads exist;
- never optimize on `getpid()` alone;
- build profiles from real target workloads (`prod-fast`, service loads,
  launcher-assisted incident paths, etc.).

#### `Propeller`

Why it matters here:

- complements AutoFDO by improving code layout, especially around block and
  function placement;
- official kernel documentation recommends using it on top of AutoFDO or
  related profile-guided builds.

Project policy:

- future performance engineering phase, not present-day default;
- only after representative perf data and stable backend choices exist.

### Out of scope or low relevance

#### `Linux Kernel GPIO based sloppy logic analyzer`

This is not meaningfully relevant to the UML redesign as currently scoped.

Project policy:

- no planning effort here unless the project shifts into hardware-facing
  device emulation work where this somehow becomes directly useful.

### Developer navigation and comprehension tools

These are worth using aggressively, but they belong in a different bucket
from analysis and test gates.

#### `cscope`

Why it matters here:

- the redesign crosses many call paths, headers, and workstream boundaries;
- understanding who calls a function, where a symbol is assigned, and how
  old and new paths overlap is half the battle in avoiding bad refactors.

Project policy:

- maintain a usable code-navigation index for the tree;
- use it to answer structure questions before changing call paths or
  ownership boundaries;
- do not cite "I couldn't find all the sites" as an excuse for partial API
  migrations.

#### `ctags`

Why it matters here:

- fast symbol lookup matters in a tree this large;
- many review and design tasks are easier when jumping between definitions
  is instant.

Project policy:

- keep tags generation easy and documented for contributors;
- treat tags as a local productivity tool, not a substitute for semantic
  analysis.

#### `clangd`

Why it matters here:

- useful for symbol navigation, rename assistance, and compiler-aware
  cross-reference in editors;
- especially useful in new code and local refactors where accurate
  compiler-backed indexing helps.

Project policy:

- optional but encouraged for developers using editor integration;
- useful adjunct to, not replacement for, `Sparse`, `Smatch`, and real
  builds.

#### `rg`

Why it matters here:

- for kernel work, fast textual search is one of the highest-leverage tools
  available;
- many architectural mistakes come from missing one call site, one Kconfig
  dependency, or one documentation copy.

Project policy:

- use fast tree search by default when auditing impact;
- combine `rg` with semantic tools, not instead of them.

## Adoption order

This is the recommended order for rolling the full A+ quality strategy
into the project.

### Phase Q1: raise the baseline immediately

- make `ARCH=um LLVM=1` a standard CI build;
- keep `checkpatch`, `Sparse`, `Smatch`, GCC, and Clang all green;
- require selftest or KUnit coverage for each user-visible or contract-like
  feature;
- keep profile smoke tests running in CI.

This is the highest-value near-term work.

### Phase Q2: strengthen bug-finding depth

- keep KASAN / UBSAN / KCOV healthy in the intended profiles;
- keep KCSAN and KFENCE on the roadmap with explicit validation gates;
- use kmemleak for long-lived lifecycle tests;
- add gcov selectively where test accounting matters.

This is where the project becomes much harder to break silently.

### Phase Q3: add Clang-specific quality leverage

- move the toolchain to Clang 22+ where feasible;
- pilot `CONFIG_WARN_CONTEXT_ANALYSIS=y` on one narrow UML area;
- decide whether more of `arch/um/` should opt into context analysis;
- document local annotation conventions for context analysis so the feature
  scales cleanly.

This is where clang becomes more than "a second compiler."

### Phase Q4: performance optimization phase

- once workload traces and representative benchmarks exist, experiment with
  `AutoFDO`;
- if gains are real and stable, experiment with `Propeller` on top;
- measure on production-like workloads, not only microbenchmarks;
- gate any such optimizations on reproducibility, debuggability, and
  acceptable build complexity.

This is where clang can start helping not just safety and quality, but
also real performance.

## Proposed CI policy

If we want the project to actually achieve this quality bar, the policy
must be explicit.

### Every PR / patch series

- GCC build: required
- Clang build (`ARCH=um LLVM=1`): required
- `checkpatch.pl`: required
- `Sparse`: required
- `Smatch`: required on touched files
- profile smoke tests: required
- KUnit or selftest coverage for new runtime-facing behavior: expected

### Every merge / nightly

- research/fuzz/profile matrix builds
- KASAN / KCOV / selected selftests
- KUnit suites
- LTP / kselftest subsets
- periodic `kmemleak` and KCSAN sweeps

### Milestones / release candidates

- sanitizer-positive tests for KASAN / KFENCE / KCSAN / KMSAN (where
  supported)
- representative performance benchmarks
- cross-backend conformance
- release-grade Clang and GCC parity review

## Project-specific cautions

### 1. Do not let clang-only work destabilize the baseline

Clang support should improve confidence, not become a second refactor
stream. If clang exposes real issues, fix them. If it exposes one-off
toolchain quirks, isolate them carefully.

### 2. Do not confuse debug kernels with production kernels

KMSAN, KASAN, KCSAN, and some tracing configurations are for bug-hunting,
not for proving production performance.

### 3. Do not optimize before workloads exist

AutoFDO and Propeller are powerful, but only after the project has
representative production-like workloads and a reasonably stable backend
story.

### 4. Do not accept "builds with one compiler" as enough

The redesign is exactly the kind of low-level code where compiler diversity
is useful.

### 5. Do not treat all tools as equally urgent

The order matters.

Highest immediate value:

- GCC + Clang
- checkpatch
- Sparse
- Smatch
- KUnit / selftests
- KASAN / KCOV in the intended profiles

Highest medium-term value:

- Context Analysis
- KMSAN
- KCSAN / KFENCE maturity

Highest later-phase performance value:

- AutoFDO
- Propeller

## Concrete next steps

If we want to start moving toward this now, the next actions should be:

1. Add `ARCH=um LLVM=1` to CI and treat new clang warnings as first-class
   triage items.
2. Update the validation checklist to explicitly require dual-compiler
   health for major milestones.
3. Keep KASAN / KCOV / KUnit / selftest validation honest in the profile
   matrix.
4. Track a future toolchain bump to Clang 22+ so Context Analysis becomes
   available.
5. Keep `AutoFDO` / `Propeller` parked until the workload and KVM/backend
   story is mature enough to justify serious tuning.

## Blocked on host toolchain: Context Analysis (clang-22+)

**Status as of 2026-04-23:** host clang is **21.1.8** (Ubuntu
package `clang`). Compiler-Based Context Analysis was added
to mainline Linux in 2026 and requires **clang 22 or newer**.
Until the host toolchain is bumped we cannot exercise this
surface under UML, so no `CONFIG_WARN_CONTEXT_ANALYSIS=y`
pilot is scheduled.

Captured as post-Q1 push **Lift #10** in
`06-sequencing/post-q1-push.md`, where it lives in Phase VII
(deferred) precisely because the only engineering lever here
is "wait for Ubuntu/Debian to ship clang 22". No in-tree
fix. Revisit triggers:

- A system clang rebase to 22.x or newer on any CI runner
  (`clang --version` ≥ `22.`).
- A kernel patch series that moves Context Analysis behind a
  fallback so clang-21 can opt in with reduced coverage —
  unlikely but worth watching `lore.kernel.org` filter
  `subject:"context analysis"`.

When the unblock happens, the implementation plan is the
`CONFIG_WARN_CONTEXT_ANALYSIS=y` narrow-pilot in bullet 3
of §"Tools I think are worth serious consideration" above
(line ~645 of this document). Not blocked on any in-tree
UML work.

## Sources

- Linux kernel: Building with Clang/LLVM
  https://docs.kernel.org/kbuild/llvm.html
- Linux kernel: Compiler-Based Context Analysis
  https://docs.kernel.org/next/dev-tools/context-analysis.html
- Linux kernel: KASAN
  https://docs.kernel.org/6.15/dev-tools/kasan.html
- Linux kernel: KMSAN
  https://docs.kernel.org/6.8/dev-tools/kmsan.html
- Linux kernel: AutoFDO
  https://docs.kernel.org/next/dev-tools/autofdo.html
- Linux kernel: Propeller
  https://docs.kernel.org/dev-tools/propeller.html
- Linux kernel: clang-format
  https://docs.kernel.org/6.11/dev-tools/clang-format.html
