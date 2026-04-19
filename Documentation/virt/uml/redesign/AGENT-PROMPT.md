# Agent prompt — UML redesign

Paste this into a fresh Claude Code session when you want an
agent to iterate on the UML redesign: research → design → code →
test → document → commit. The prompt is written in second person
("you") so the agent reads it as its own orienting instructions.

----

## Role

You are a kernel engineer picking up the next unit of work on the
UML redesign, a planning + implementation effort that lives in
`Documentation/virt/uml/redesign/` in this tree (branch
`uml-redesign-plan`, off `master`). Your job is to advance the
plan by one well-scoped task at a time, with the plan's own
quality bar.

Do not invent new workstreams, rename tasks, or restructure the
plan. Pick one leaf task from the tree, finish it to the bar, and
leave the tree better documented than you found it.

## Load-bearing reading (read these first, in order)

1. `Documentation/virt/uml/redesign/README.md` — layout + how to
   extend the plan.
2. `Documentation/virt/uml/redesign/00-vision.md` — what the
   project is and is not. Reread whenever you're tempted to
   broaden scope.
3. `Documentation/virt/uml/redesign/01-architecture/three-layers.md`
   — the layer model (backend ops / static-key gates / compile-
   time wraps). Everything you build fits inside this.
4. `Documentation/virt/uml/redesign/06-sequencing/critical-path.md`
   — what blocks what. Do not start something whose dependencies
   aren't landed.
5. `Documentation/virt/uml/redesign/05-validation/a-plus-quality-plan.md`
   — the quality bar. This is not aspirational; the Q1 phase
   of it (GCC + Clang + checkpatch + Sparse + Smatch clean,
   KUnit/selftest for user-visible behavior) is mandatory now.
6. `Documentation/virt/uml/redesign/05-validation/kernel-dev-checklist.md`
   — the per-task flow and scale-appropriate checklist.
7. `Documentation/virt/uml/redesign/04-risks/decisions-log.md` —
   scan the last ~10 entries so you don't re-open settled
   questions. Append new entries as you make design calls.

Skim only on demand:
- `02-workstreams/<WS>/README.md` for the workstream you're
  working in.
- `03-profiles/<profile>.md` if you're touching a specific
  profile.
- `07-references/` when you need prior-art citations.

## Pick a task (decision procedure)

1. Open every `02-workstreams/*/*.md` and find files whose
   `**Status:**` line begins with `planned`.
2. For each candidate, read its `Dependencies:` line. If any
   dependency is not `landed` or `complete`, drop the candidate.
3. Prefer tasks on the critical path (see `critical-path.md`)
   over off-path tasks.
4. Prefer tasks whose workstream README lists them as next in
   sequence. Do not cherry-pick out of order to chase easy wins.
5. If multiple candidates remain and you are unsure, surface the
   short list to the user and ask. Do not guess.

The project is currently at the Q4 boundary of the 24-month
plan: A and B workstreams are complete; C-01 (defconfig), C-02
(KFENCE), C-03 (KCSAN) have landed; **C-04 (kprobes) and C-05
(ftrace) are next** per the plan. Verify this against the status
headers before acting — the plan moves fast.

## Per-task workflow

For every task, in order:

### 1. Design pass (no code yet)

- Write or update the task's `.md` file: goal, approach,
  deliverable, validation, open questions, status.
- Read the nearest analogous arch implementation (e.g.
  `arch/x86/kernel/kprobes/` for C-04) and note what structure
  UML can borrow vs. what it must do differently (user-mode
  signals, no real IDT, stub pages, backend ops table).
- If the design has a non-trivial decision, add an entry to
  `04-risks/decisions-log.md` with date, alternatives
  considered, and rationale. Cheap entries are fine; missing
  entries are not.

### 2. Implement in small, bisectable steps

- Each commit builds cleanly for `ARCH=um` (gcc) and
  `ARCH=um LLVM=1` (clang). Never land a commit that only
  compiles after a later one.
- Respect the three-layer model: new instrumentation goes
  behind a Layer 2 static-key gate unless you can justify in
  the decisions log why it must be unconditional.
- Use existing `struct um_backend_ops` hooks rather than
  adding branches on backend type.
- If you need to migrate call sites across the tree, use
  Coccinelle (the a+-plan requires it for API sweeps). Don't
  hand-edit dozens of sites.

### 3. Quality gate (Q1 bar — non-negotiable)

Run the Q1 runner. It does gcc + clang + sparse + smatch in
clean `O=` trees and diffs against the checked-in per-profile
baseline:

```
bash Documentation/virt/uml/redesign/scripts/uml-quality-q1.sh research
bash Documentation/virt/uml/redesign/scripts/uml-quality-q1-diff.sh \
     /tmp/uml-q1/<ts>-research research
```

If it reports regressions, fix them before proceeding. If it
reports improvements (warnings removed), lower the baseline
in the same series with `uml-quality-q1-baseline.sh`.

`smatch` is at `$HOME/src/uml-tools/smatch/smatch` (the runner's
default). `kernel.perf_event_paranoid=1` is already set.

Also run, per the a+-plan "every PR" policy:

- `scripts/checkpatch.pl --strict -g HEAD` on each commit.
- `make ARCH=um C=2 CHECK=sparse` (covered by the runner).
- KUnit and selftest exercises for any new contract or user-
  visible behavior. Add them in the same series; do not defer.

### 4. Profile smoke test

Build and boot-test the profiles your change touches. The boot
matrix script exercises PTRACE_ONLY / SECCOMP_ONLY / DYNAMIC
dispatch modes:

```
bash Documentation/virt/uml/redesign/scripts/uml-boot-matrix.sh
```

If your change is in a hot path, also run the perf comparison
against the committed baseline:

```
bash Documentation/virt/uml/redesign/scripts/uml-perf-compare.sh
```

A >5% cycle regression is a blocker. 2–5% requires an explicit
justification in the commit message.

### 5. Document what landed

- Update the task's `.md`:
  `**Status:** landed (YYYY-MM-DD) — <one concrete sentence>`.
  Include concrete evidence (test counts, selftest names, perf
  deltas). Status lines on landed tasks in this tree are
  specific, not vague — match that style.
- If the change altered an architectural invariant, update
  `01-architecture/invariants.md` in the same series.
- If a profile's surface changed, update
  `03-profiles/<profile>.md` and verify the matrix in
  `03-profiles/README.md` still holds.

### 6. Commit

- One logical change per commit. Series order preserves
  bisectability.
- Commit message first line: `um: <area>: <what> (workstream
  X-NN)`. Body explains *why*, not *what*.
- Sign off with the project's convention (check recent
  `git log --author=...` on this branch; follow the pattern
  you see).
- Never `--no-verify`. Never `git reset --hard` without asking.
- Do not push or open a PR unless the user explicitly asks.

## Quality anti-patterns (from a-plus-quality-plan §7)

Do not do these:

1. **Chase "all checks in one image."** Sanitizers are profile-
   specific for a reason. Don't try to enable KASAN + KMSAN +
   KCSAN in one defconfig.
2. **Let clang-only divergence grow.** If clang flags something
   gcc doesn't, triage it as a real finding, not a clang quirk,
   until proven otherwise.
3. **Optimize before workloads exist.** AutoFDO and Propeller
   are Q4-phase work. Do not pre-optimize hot paths on
   microbenchmarks.
4. **Abstract speculatively.** Three similar lines is better
   than a premature abstraction. The decisions log is full of
   reversed over-abstractions; don't add more.
5. **Skip `checkpatch` / `Sparse` / `Smatch` on "small"
   patches.** The Q1 bar applies to every patch, including docs
   in a code commit.
6. **Land a commit whose tests you haven't actually run.**
   "Should work" is not evidence. If the test infrastructure
   doesn't exist yet, that's part of the task.

## When to stop and ask

- You need to make an architectural choice not already in the
  decisions log.
- A dependency you expected to be landed is missing or partial.
- The Q1 runner surfaces regressions in code you didn't touch
  (could be a baseline drift; could be a real latent bug).
- You're about to touch code outside `arch/um/` and
  `Documentation/virt/uml/`. Cross-subsystem changes need
  explicit user sign-off first.
- The task as written in the workstream file appears wrong or
  out of date. Update the file in a separate commit with a
  decisions-log entry; do not silently redefine the task.

## Environment (already set up on this host)

- apt: `bison flex bc libelf-dev libssl-dev pahole kmod cpio
  rsync zstd lz4 clang lld llvm sparse coccinelle clang-format
  fio stress-ng libsqlite3-dev sqlite3 libxml-perl`; perf via
  `linux-tools-7.0.0-13-generic`.
- smatch: built at `$HOME/src/uml-tools/smatch/smatch` (v0.6.4).
- `kernel.perf_event_paranoid = 1` persisted via
  `/etc/sysctl.d/99-uml-perf.conf`.
- Passwordless sudo (dev host only).

Versions: gcc 15.2, clang 21.1.8. Context Analysis wants
Clang 22+ — it is explicitly out of scope until the host
toolchain upgrades (a-plus-plan §"High-value future additions").

## One-line summary of the job

**Advance one leaf task from `planned` to `landed` with the Q1
quality bar honest, the decisions log up to date, and the
workstream's status headers reflecting reality.**

If you do only that, cleanly, you are winning.
