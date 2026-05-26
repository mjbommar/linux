# Kernel development checklist (project-scaled)

A composite checklist drawn from the kernel's own process docs, scoped
to our 24-engineer-month UML redesign rather than a one-off patch.

**Sources** (read in full when in doubt):
- `Documentation/process/submit-checklist.rst` — the canonical
  per-patch checklist.
- `Documentation/process/submitting-patches.rst` — describe-your-
  changes, split rules, tag conventions.
- `Documentation/process/coding-style.rst` — kernel C style.
- `Documentation/process/programming-language.rst` — `-std=gnu11`,
  attribute conventions, Rust support.
- `Documentation/process/4.Coding.rst` — abstraction discipline,
  preprocessor caution, locking, regressions, code-checking tools.
- `Documentation/process/5.Posting.rst` — when to post, patch series,
  changelogs, tags, mailing logistics.
- `Documentation/process/coding-assistants.rst` — AI attribution rules.
- `Documentation/process/generated-content.rst` — tool-generated
  content disclosure (April 2026).
- `Documentation/process/3.Early-stage.rst` — problem-first framing.
- `Documentation/process/maintainer-kvm-x86.rst` — applies once
  workstream D starts.

This checklist is organized by **scale of work** rather than by
chronology, because at any given moment we're operating at multiple
scales simultaneously (one task in flight, one workstream in flight,
the project as a whole).

---

## Scale 0: project-level gates (run once per quarter or milestone)

These are the gates that protect the whole 24-EM effort from going
sideways.

- [ ] Architecture decisions are recorded in
      `04-risks/decisions-log.md` with date, alternatives, and
      revisit triggers (D1–D8 today; growing).
- [ ] Each workstream README's task table reflects current state
      (planned / in progress / complete / blocked).
- [ ] Cross-workstream synchronization points (per
      `02-workstreams/README.md`):
   - [ ] Backend interface freeze (after A's first 2 months) — A's ops
         table commits; B and D can begin.
   - [ ] Profile matrix freeze (after C's first 4 months) — C commits
         to which 6+ defconfigs ship.
- [ ] Slip handling per `06-sequencing/24-month-plan.md`: any task
      slipping >50% of its budget triggers a sequencing review.
- [ ] Bail-out criterion (per `05-validation/upstream-strategy.md`):
      if no significant series has landed in mainline after 6 months,
      stop and reassess.
- [ ] Public progress: monthly status post to linux-um per the
      upstream-strategy cadence.

---

## Scale 1: per-workstream gates (run once per workstream task)

These are the gates that protect each ~4–8 EM workstream task. Apply
when starting and finishing every numbered task in
`02-workstreams/<W>/<NN>.md`.

### When starting a task

- [ ] Read `02-workstreams/<W>/<NN>.md` in full. Match its
      Goal / Approach / Deliverable / Validation / Open-questions
      structure in any notes you produce.
- [ ] Read its dependencies (the "Dependencies" line). If any is
      unfinished, stop and either work on the dependency or escalate.
- [ ] Read the workstream README's Sequencing rules and Open
      questions sections.
- [ ] Mark the task `in_progress` in the project's task tracker
      (TaskUpdate) and in the task spec's Status line.
- [ ] If the task introduces a new directory (notes/, backend/, etc.),
      create it before writing files into it.

### Early-stage problem framing (per `3.Early-stage.rst`)

- [ ] Restate the problem in one sentence (not the proposed solution).
      *Audio-LSM cautionary tale:* don't conflate the problem with the
      solution.
- [ ] Identify who the affected users/maintainers are.
- [ ] Confirm the kernel doesn't already address it in a way you
      missed.
- [ ] Discuss with the community before significant code is written —
      for our project, that means circulating an RFC memo before
      writing the refactor (cf. A-01.10 / A-02 split).

### When finishing a task

- [ ] Validation criterion from the task spec is met (with evidence —
      compile output, smoke test, etc.).
- [ ] Open questions in the spec are either resolved (logged in
      `04-risks/decisions-log.md`) or explicitly deferred with a
      named follow-up task.
- [ ] Status updated to `complete` with date in both the task spec
      and the workstream README's task table.
- [ ] Decisions log appended if any architectural decision was made.
- [ ] Notes/working files committed (don't leave un-tracked artifacts
      that future sessions will rediscover painfully).
- [ ] Mark the task `completed` in the project's task tracker.

---

## Scale 2: per-patch-series gates (run before posting any series)

These mirror `submit-checklist.rst` and `5.Posting.rst`, scoped to a
single git-send-email batch.

### Before creating patches (per `5.Posting.rst`)

- [ ] Tested to the extent possible: build with applicable Kconfigs,
      cross-compile if affecting non-x86, run kunit/kselftest if
      relevant.
- [ ] Coding style is compliant.
- [ ] Performance impact characterized: if HOT-path code (5 ops in
      our table — see A-01.2), include cycle-level numbers.
- [ ] Right to post: GPL-2.0-only headers; SPDX identifiers; if any
      employer is involved, their release is on file.

### Patch series structure (per `5.Posting.rst`, section "Patch preparation")

- [ ] Series matches `upstream-strategy.md`: keep series small
      (3–5 patches typical; 50-patch monsters out).
- [ ] Each patch is *one logical change* — don't mix bug fix +
      reformat + structure rearrangement.
- [ ] Each intermediate patch leaves the kernel in a buildable,
      working state (bisectability requirement).
- [ ] Don't add infrastructure that's unused until the last patch —
      if a series adds regressions, bisect would finger the wrong
      patch. Activate code in the patch that introduces it whenever
      possible.

### Per-patch hygiene (per `submit-checklist.rst`)

For every patch in the series:

- [ ] Includes only headers it actually uses; doesn't depend on
      transitive includes.
- [ ] Memory barriers (`barrier()`, `rmb()`, `wmb()`) have a
      comment explaining the why.
- [ ] New Kconfig options:
   - [ ] don't muck up the menu;
   - [ ] default to off unless they meet the exception criteria;
   - [ ] have help text;
   - [ ] reviewed for combinations.
- [ ] kernel-doc on global APIs (not required for static functions
      but allowed).
- [ ] New `/proc` entries documented under `Documentation/`.
- [ ] New boot params documented in
      `Documentation/admin-guide/kernel-parameters.rst` (relevant
      for our `backend=` boot param in A-04).
- [ ] New module params documented with `MODULE_PARM_DESC()`.
- [ ] New userspace interfaces documented in `Documentation/ABI/`
      and CC'd to linux-api@vger.kernel.org.

### Tool gates (per `submit-checklist.rst` "Check your code with tools")

- [ ] `scripts/checkpatch.pl` — clean or every remaining warning
      is justified.
- [ ] `sparse` — clean (`make C=1`).
- [ ] `make checkstack` — no function uses >512 bytes of stack
      without justification.
- [ ] `smatch` (built at `~/src/uml-tools/smatch/smatch`) — review any
      new warnings on touched files.
- [ ] `coccicheck` if API changes are wide (`make coccicheck`).
- [ ] `clang-format` and `clang-tidy` available; use to spot-check
      large new files.

### Build matrix (per `submit-checklist.rst` "Build your code")

- [ ] Builds cleanly with the modified Kconfigs as `=y`, `=m`, `=n`
      where applicable.
- [ ] `allnoconfig` and `allmodconfig` pass for `ARCH=um`.
- [ ] `O=builddir` out-of-tree build works.
- [ ] `make htmldocs` succeeds with no new warnings if any
      `Documentation/` change is in the series.
- [ ] If the patch is portable code, build for at least one non-x86
      arch (workstream A is x86_64 only initially; revisit when ARM64
      port lands per `00-vision.md`).
- [ ] `make KCFLAGS=-W` to surface signed/unsigned + dead-store
      warnings on the new code.

### Runtime test gates (per `submit-checklist.rst` "Test your code")

- [ ] Boots and runs with the debug-Kconfig set:
      `CONFIG_PREEMPT`, `CONFIG_DEBUG_PREEMPT`, `CONFIG_SLUB_DEBUG`,
      `CONFIG_DEBUG_PAGEALLOC`, `CONFIG_DEBUG_MUTEXES`,
      `CONFIG_DEBUG_SPINLOCK`, `CONFIG_DEBUG_ATOMIC_SLEEP`,
      `CONFIG_PROVE_RCU`, `CONFIG_DEBUG_OBJECTS_RCU_HEAD` all on
      simultaneously.
- [ ] Runs with and without `CONFIG_SMP` and `CONFIG_PREEMPT`
      (when applicable to our profile matrix).
- [ ] Lockdep is enabled and shows no new splats on touched paths.
- [ ] Fault injection (slab + page-allocation) tried at least once
      (`Documentation/fault-injection/`).
- [ ] Tested against current `linux-next` to catch interaction with
      other in-flight work.

### Changelog and tags (per `submitting-patches.rst` and `5.Posting.rst`)

- [ ] Subject line: `subsys: short imperative description` (e.g.
      `um: introduce typed backend ops table`).
- [ ] Body in **imperative mood** ("make foo do bar", not "this
      patch makes…").
- [ ] Describes the *problem*, not just the solution.
- [ ] User-visible impact described (regressions, performance
      changes, dmesg excerpts).
- [ ] Performance numbers quantified if relevant; trade-offs called
      out (don't pretend it's free).
- [ ] References cited in correct form: `commit abcdef123456 ("oneline")`,
      with at least 12 chars of SHA.
- [ ] `Fixes:` / `Closes:` / `Link:` tags as appropriate.
- [ ] **`Signed-off-by:` is added by the human submitter only.**
      Per `coding-assistants.rst`: AI agents MUST NOT add `Signed-off-by`.
      The human Signed-off-by is the legal DCO certification; no
      proxy.
- [ ] **`Assisted-by:` tag** added when AI tools were used:
      `Assisted-by: Claude:claude-opus-4-7 sparse smatch coccinelle`
      (only list non-trivial tools; not gcc/git/make).
- [ ] Tool-generated content disclosed in cover letter per
      `generated-content.rst`: which tools, which prompts (or
      summary), which portions.
- [ ] `Co-developed-by:` only with explicit permission and matched
      with a `Signed-off-by:` for that co-author.
- [ ] `Reviewed-by:` / `Tested-by:` / `Acked-by:` tags only when
      the named person actually said so.

### Recipient list (per `5.Posting.rst` "Sending the patch")

- [ ] `scripts/get_maintainer.pl` run on every file in the series;
      every named maintainer is on To: or Cc:.
- [ ] linux-um (for our work) on Cc:.
- [ ] LKML on Cc: if cross-cutting.
- [ ] If fixing a bug also relevant to stable, `Cc: stable@vger.kernel.org`
      in the patch trailer.
- [ ] Subject: `[PATCH nn/mm] subsys: short description` (or `[RFC]`
      / `[RESEND]` / `[v2 PATCH]` as appropriate).
- [ ] Series sent with proper threading (cover letter + replies for
      each patch); avoid `--chain-reply-to` on long series.
- [ ] Plain text only; no HTML; no attachments.
- [ ] Self-mail test passed (sent to yourself, applied cleanly with
      `git am`).

---

## Scale 3: design discipline gates (apply continuously while writing)

These are the *don't-do* list from `4.Coding.rst`. Re-read the full
section if a reviewer ever cites them.

### Coding style

- [ ] 8-character tabs, no spaces for indentation outside
      Kconfig/comments.
- [ ] No trailing whitespace.
- [ ] If a function exceeds 3 levels of indentation, refactor.
- [ ] Don't reformat existing code "just because" — it's noise.
      Style fixes happen incidentally to other work, not as their
      own patches.

### Abstraction discipline (the section that most directly applies
to A-01)

- [ ] Don't add abstraction layers beyond what the immediate task
      requires. Hardware-hiding shims are explicitly frowned upon.
- [ ] Don't add function arguments "just in case." Always-zero args
      get removed by reviewers.
- [ ] If you're copying significant code from another subsystem,
      consider lifting it into a shared library instead.

### Preprocessor and inline functions

- [ ] Don't sprinkle `#ifdef` through .c files. Confine `#ifdef` to
      headers; conditional code goes in functions that become empty
      stubs in the off-state.
- [ ] Prefer inline functions to function-like macros — type safety,
      no double evaluation.
- [ ] Inline functions stay small. Big inlines bloat the i-cache;
      "space *is* time."
- [ ] Trust the compiler — `inline` is a hint, not a command.

### Locking

- [ ] Every concurrently-accessible resource has a documented lock.
- [ ] Lock ordering is documented at the data-structure level.
- [ ] Lockdep enabled in test builds; no splats.
- [ ] Retrofitting locking is harder than writing it correctly the
      first time — design for SMP from day 1.

### Regressions (the cardinal sin)

- [ ] Change does not break existing users. "Two steps forward, one
      step back" is rejected on principle (Linus, July 2007).
- [ ] No user-space ABI break. Internal kernel APIs are fluid;
      user-space ABIs are forever.
- [ ] If an internal API change is needed:
   - [ ] It's its own patch (not buried in a larger change).
   - [ ] All in-tree callers are fixed in the same series.
   - [ ] The compiler catches stale callers (use a typedef change
         or signature change, not a rename-only change that links
         silently).

---

## Scale 4: per-file gates (apply when creating a new source file)

- [ ] First line: `/* SPDX-License-Identifier: GPL-2.0 */`
      (or `// SPDX-License-Identifier: GPL-2.0` for .c with C++
      comments).
- [ ] No filename in the file header (checkpatch warns; the path
      is in the path).
- [ ] Header guards: `#ifndef __<PATH>_H` / `#define __<PATH>_H` /
      `#endif`.
- [ ] Header includes only what it directly references (use forward
      declarations where possible).
- [ ] Kernel-doc on every exported function in a `.h`.
- [ ] No new global mutable state without a comment justifying it.

---

## Scale 5: AI / tool transparency (apply at every contribution)

Per `coding-assistants.rst` + `generated-content.rst`:

- [ ] Cover letter discloses AI tool use: which tool, which model,
      what portion was AI-generated, what was human review.
- [ ] If a chunk was generated by a single prompt or short prompt
      sequence, include the prompt(s).
- [ ] For longer sessions, summarize the prompt chain.
- [ ] `Assisted-by:` trailer on patches that consumed AI assistance.
- [ ] **Submitter understands every line.** Per
      `generated-content.rst`: "You are expected to understand and
      to be able to defend everything you submit. If you are unable
      to do so, then do not submit the resulting changes."
- [ ] Expect *additional scrutiny* proportional to AI involvement.
      Plan for slower review; budget for it.
- [ ] Don't claim `Reviewed-by:` from a chatbot. Tools don't review;
      humans do.

---

## Scale 6: subsystem-specific (when workstream D / KVM begins)

Pulled from `Documentation/process/maintainer-kvm-x86.rst` (full
read required when D-01 starts):

- [ ] Base on `kvm-x86/next` for next-cycle work; on
      `kvm/master` for current-cycle fixes.
- [ ] Multi-arch series base on a common stable point (likely a
      release candidate), not a topic branch.
- [ ] Match existing style; **consistency is the #1 KVM x86
      priority**.
- [ ] Follow tip-tree maintainer style for files touched by both
      KVM and tip (likely for our backend code).
- [ ] No kernel-doc on KVM functions unless they're actually public
      consumers — most KVM functions are internal.
- [ ] Reverse XMAS variable declarations preferred (not strictly
      required).
- [ ] Testing is **mandatory** per the TL;DR. Don't skip the
      KVM-unit-tests run before posting.
- [ ] Pings are welcome but: between rc6 and rc1 there's typically
      radio silence; have realistic timing expectations.
- [ ] Don't ping a series that breaks the build or fails tests.

---

## Scale 7: project-specific UML redesign gates

Tying back to our own plan:

### Architecture-shape preservation

- [ ] Three-layer architecture (`01-architecture/three-layers.md`)
      is not violated: backend ops in Layer 1, static-key gates in
      Layer 2, compile-time wraps in Layer 3.
- [ ] Layer-1 ops table doesn't grow with backend-specific ops; KVM
      extensions stay internal to the KVM impl (per A-01.6).
- [ ] Profile defconfigs (`03-profiles/`) stay distinct; new code
      that fires across all profiles gets the profile-impact
      analysis from `03-profiles/README.md`.

### Performance discipline (invariant I2)

- [ ] HOT ops (5 of them: run_userspace, mm_map, mm_unmap,
      context_switch, read_clock_ns) are inlinable in single-backend
      builds.
- [ ] Multi-backend dispatch overhead measured (~5 cycles per call
      target).
- [ ] prod-fast profile syscall cost stays within 5% of pre-refactor
      baseline (workstream A-07 perf CI).
- [ ] No regression on benchmarks listed in `05-validation/benchmarks.md`.

### Conformance

- [ ] Every backend op has a kunit test in `arch/um/backend/contract/`
      (workstream A-05).
- [ ] Cross-backend equivalence tests pass on every available
      backend (LTP syscall subset).
- [ ] Conformance suite total runtime < 30 minutes
      (`05-validation/conformance-tests.md`).

### Upstream cadence

- [ ] First send is `[RFC]`, not `[PATCH]`.
- [ ] Architecture RFC pre-circulates before any code series
      (per `upstream-strategy.md` "Justify the architecture *before*
      sending code").
- [ ] Each maintainer's priorities respected (Berg/seccomp,
      Ivanov/networking, Bie/SMP).
- [ ] Co-author offers from maintainers accepted gladly.
- [ ] Bi-weekly cadence on patch series; monthly on linux-um status
      post.

---

## Build / test timing log

- [ ] **Time every multi-minute step.** Append a row to
      `02-workstreams/<W>/notes/timings.md` with wall clock, `-j`,
      and cache state (cold ccache vs warm; defconfig vs allmod;
      incremental vs from clean).
- [ ] Wrap manually-run timed steps with `t0=$(date +%s); …;
      t1=$(date +%s); echo "elapsed=$((t1-t0))s"`.
- [ ] For background tasks, derive wall time from the task-output
      file's mtime (creation) vs end-time (notification timestamp or
      `date +%s` at completion).
- [ ] Per Scale 2 "Performance impact characterized": surface runtime/
      build numbers in the commit message of any perf-relevant
      change. Build time is part of the baseline; record it.

## Quick-reference: command snippets

For copy-pasting into a session:

```bash
# Per-file checks
scripts/checkpatch.pl --strict --file <path>
scripts/get_maintainer.pl <path>

# Per-patch checks
scripts/checkpatch.pl --strict <patch-file>
git format-patch -1 -o /tmp/patches HEAD
git send-email --to=<maintainer> --cc=<list> /tmp/patches

# Per-series build/test
make ARCH=um O=/tmp/build allnoconfig && make ARCH=um O=/tmp/build -j$(nproc)
make ARCH=um O=/tmp/build allmodconfig && make ARCH=um O=/tmp/build -j$(nproc)
make C=1 ARCH=um O=/tmp/build           # sparse
make KCFLAGS=-W ARCH=um O=/tmp/build    # extra warnings
make ARCH=um coccicheck                 # coccinelle
make ARCH=um checkstack
~/src/uml-tools/smatch/smatch_scripts/build_kernel_data.sh  # smatch

# Doc build
make htmldocs SPHINXDIRS=virt/uml

# Self-mail test
git send-email --to=<your-own-address> /tmp/patches
git am /tmp/from-mail.mbox
```

---

## How to use this checklist

- **Don't run all of it for every change.** Match the scale of the
  checklist to the scale of the work. A one-task notes update needs
  only Scale 1's "When finishing a task" gates. A 5-patch series
  posting to LKML needs Scales 1, 2, 3, 4, 5, and the relevant parts
  of 6 and 7.
- **Treat as living.** When a reviewer cites a process doc that
  isn't reflected here, append it. When a gate proves to be redundant
  for our project, remove it.
- **Cross-reference, don't duplicate.** Where the kernel doc is
  authoritative (e.g. coding style minutiae, full submitting-patches
  spec), point to it rather than restating.

## Out of scope

- The full `coding-style.rst` content (1,294 lines). Read it in
  full once; refer back as needed.
- Subsystem checklists for areas outside our touch surface
  (netdev, soc, etc.).
- Stable-tree backporting rules (we won't be backporting).
