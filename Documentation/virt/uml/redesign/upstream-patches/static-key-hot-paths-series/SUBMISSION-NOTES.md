# static-key-hot-paths-series — submission-readiness notes

## Status (2026-05-14)

**DRAFTED.** Cover letter + this notes file in-tree;
`0001-*.patch` … `0006-*.patch` **not yet emitted**. The
on-branch substrate exists as one ~1500 LoC megacommit
(`570b0c386e0c` "um: introduce Layer 2 static-key hot-path
gates") plus three follow-on fix commits (D20 IRQ gate
extension, D21 `sanitize_paranoid` removal, D22 per-CPU
stats rewrite). A structured squash + rebase pass against
post-Series-4 mainline is the gating prerequisite before
`git format-patch` can produce the 6-patch reviewable
sequence the cover letter advertises.

Transition: **TO WRITE → DRAFTED 2026-05-14 → (after Series
4 lands + squash audit) READY → SENT → LANDED.**

This is Series 5 of the seven-series upstream queue per
`upstream-patches/SUBMISSION-QUEUE.md` (D63). Series 4 (the
backend-ops-abstraction RFC) must land first because the
hook insertion sites in this series sit immediately
downstream of Series 4's HOT-op dispatch surface. Series 6
+ Series 7 cite this series for the slow-path-replacement
contract their consumers plug into.

## Queue position

5th, after Series 1 (bpf-hygiene-v1, READY), Series 2
(kmsan-arch-callback-rfc, READY), Series 3
(ftrace-notrace-generic-v1, READY), and Series 4
(backend-ops-abstraction-rfc, DRAFTED 2026-05-14). Per D63
the queue is **dependency-ordered, not strictly time-
ordered** — Series 1+2+3 can be sent in any order on their
own merits, Series 4 follows Series 3 for `notrace`
cleanliness, and Series 5 follows Series 4 because the
gate call sites land on code Series 4 is reshaping.

Strictly speaking Series 5 compiles against pre-Series-4
mainline (the C-fallback jump-label form is arch-generic,
and the hook call sites `handle_syscall` / `__switch_to` /
`do_IRQ` / `timer_read` exist in today's tree). The
dependency on Series 4 is operational: sending Series 5
ahead would force reviewers to evaluate the gate surface
against a moving target. If Series 4 stalls in review past
Series 5's emission window, Series 5 may emit ahead with a
cover-letter note that Series 4 is recommended-but-not-
required, but per D63 the queue should not normally jump
this dependency.

## Motivating workstream

Workstream B (static-key hot-path gates) of the UML
redesign plan:

  `Documentation/virt/uml/redesign/02-workstreams/B-static-key-hot-paths/`

The workstream produced the Layer 2 substrate: six static-
key gates, six hook helpers across seven call sites, a
debugfs control surface, a section-split linker script that
unlocks JIT patching, a microbenchmark harness with
checked-in baseline, and a first end-to-end demo (flip
`trace_syscalls` mid-run; observe counter delta). All six
on-branch tasks (B-01..B-06) completed 2026-04-18 and were
validated by the 2026-04-23 external review pass which
surfaced three findings (D20/D21/D22) folded back into the
substrate.

Workstream B's README:

  `02-workstreams/B-static-key-hot-paths/README.md`

Per-task memos under the same directory:

  - `01-audit-entry-points.md` — what hot paths exist; the
    six-helper / seven-site inventory.
  - `02-gate-design.md` — the macro design + helper API.
  - `03-debugfs-controls.md` — runtime toggle surface.
  - `04-section-split.md` — code-patching considerations,
    `.um_patch_text` design.
  - `05-benchmark-targets.md` — perf measurement
    methodology + B-05-precise follow-up scope.
  - `06-first-flip-demo.md` — the B-06 end-to-end
    `trace_syscalls`-flip selftest.

In particular `notes/hot-paths.md` is the authoritative
inventory of every gate insertion site; the upstream
patches must reproduce that inventory exactly.

## Pre-submission cleanups — what needs to happen before format-patch

The on-branch state today is one ~1500 LoC megacommit
(`570b0c386e0c`) plus three follow-on fixup commits. None
of those are in a shape we can `git format-patch` and send
to LKML. The squash audit produces the 6-patch sequence the
cover letter advertises, per D23.

### On-branch commit map (squash audit input)

The following on-branch commits feed Series 5 patches:

| Slot     | Patch subject (target)                                    | On-branch commit(s)                                                         | Notes                                                                                                                          |
|----------|-----------------------------------------------------------|-----------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------|
| Patch 1  | um: hooks: gate infrastructure + first live hook site     | `570b0c386e0c` (hunks for `<asm/um-hooks.h>` + `kernel/hooks.c` + first `um_on_syscall_entry` insertion) | Apply D21 (drop `sanitize_paranoid`) + D22 (per-CPU stats) inline; do not ship the atomic-then-per-CPU intermediate.            |
| Patch 2  | um: hooks: remaining hot-path insertions                   | `570b0c386e0c` (hunks for `kernel/skas/syscall.c` tail + `kernel/trap.c` + `kernel/process.c` + `kernel/irq.c` + `kernel/time.c`) | Apply D20 (three IRQ-delivery sites) inline; not an after-the-fact fix patch.                                                  |
| Patch 3  | um: hooks: debugfs control surface                         | `570b0c386e0c` (hunks for `kernel/um_debugfs.c` + `Documentation/virt/uml/debugfs.rst`) | Apply D22's per-CPU stats reader; do not ship the global-atomic reader.                                                        |
| Patch 4  | um: hooks: `.um_patch_text` section split + JIT patching   | `570b0c386e0c` (hunks for `kernel/uml.lds.S` + `kernel/dyn.lds.S` + `kernel/jump_label.c` + `arch/um/Kconfig` `select HAVE_ARCH_JUMP_LABEL`) | The mprotect helpers (`um_patch_text_rw` / `um_patch_text_ro`) land here, used by Patch 4 today and by ftrace mcount / kprobes / BPF JIT in later workstreams. |
| Patch 5  | um: hooks: benchmark harness + checked-in baseline         | `570b0c386e0c` (hunks for `kernel/hooks_bench.c` + `scripts/uml-gate-bench{,-compare}.sh` + `notes/bench-baseline.json`) | First-pass methodology only; per-gate isolation deferred to B-05-precise. The 15 % per-site ceiling is the CI gate.            |
| Patch 6  | um: hooks: documentation + selftest                        | `570b0c386e0c` (hunks for `Documentation/virt/uml/hooks.rst` + `tools/testing/selftests/um/hooks-flip/`) + MAINTAINERS hunks | The B-06 selftest is the demo vehicle; reviewers can `make TARGETS=um kselftest` to reproduce the flip.                        |

Three on-branch commits do NOT feed Series 5 as their own
patches:

  - **D20 IRQ-extension fixup commit** — folded into Patch 2.
    The three-site insertion was a fix for the original B-01
    audit's "one site covers everything" claim; upstream
    sees the three-site form directly. Per D23 each Patch
    must be bisect-clean; shipping the one-site form first
    and the three-site fix second would create a known-bad
    intermediate.

  - **D21 sanitize_paranoid removal commit** — folded into
    Patch 1. The named-but-dead gate was a B-02 artifact;
    upstream sees six gates directly, with the policy
    comment in the header. No "remove it after introducing
    it" patch.

  - **D22 atomic-to-per-CPU rewrite commit** — folded into
    Patch 1 + Patch 3. The atomic intermediate distorted
    benchmark numbers and was a B-02 first-cut artifact;
    upstream sees the per-CPU shape directly.

This shape produces a 6-patch sequence where the commit
messages read as upstream-shape patches, not as redesign-
walkthrough commits. Each intermediate state MUST build,
boot, and pass cpython-parity 21/21 on
`tools/testing/selftests/um/cpython-parity/cpython-parity.sh`.
Patches that break the build are merge-conflict bugs in the
emission, not legitimate intermediate states.

### Sequencing checklist before format-patch

Per `backend-ops-abstraction-rfc/SUBMISSION-NOTES.md` and
`ftrace-notrace-generic-v1/SUBMISSION-NOTES.md` precedent:

  1. **Submission branch:** `static-key-hot-paths-rfc-v1-
     submit` off post-Series-4 `master` (NOT
     `umlctl-deploy`, NOT pre-Series-4 master). Cherry-pick
     the 6 squash-audit outputs in order.
  2. **Author + signoff:** `Michael Bommarito
     <michael.bommarito@gmail.com>` (never
     bommaritollc.com on kernel work). `Co-authored-by:
     Claude Opus 4.7 …` trailers **dropped** for upstream
     per ftrace-notrace precedent;
     `Documentation/process/coding-assistants.rst` covers
     AI-assistance disclosure.
  3. **No redesign-log references in commit messages.** D##
     entries stay in this file and the cover letter, never
     in per-patch bodies (per
     `upstream-patches/README.md`).
  4. **Checkpatch:** 0/0 target per `.patch`. Code-snippet
     line-length warnings in commit messages are acceptable
     false positives.
  5. **Build verification per intermediate state:** each
     of 6 patches must build under
     `make ARCH=um O=/tmp/uml-N defconfig` with both
     `CONFIG_UM_HOOKS={y,n}`, boot under
     `./linux mem=512M init=/bin/sh`, and pass
     cpython-parity 21/21 with all gates off (bit-identical
     to `CONFIG_UM_HOOKS=n`). Failing intermediates fix in
     the squash, never papered over.
  6. **`scripts/get_maintainer.pl`** rerun on each patch +
     cover letter right before `git send-email`.
  7. **Format-patch:**

        git format-patch -6 --cover-letter \
            --subject-prefix='PATCH RFC' --base=master \
            -o /tmp/static-key-hot-paths-rfc-emit/

     Cover subject:
     `[PATCH RFC 0/6] um: static_key gates on arch/um hot paths`.

## Verification author performed

(Captured 2026-05-14 against the on-branch state; needs
re-run on the cherry-picked submission branch after Series
4 lands and before send.)

  - **Build-tested on ARCH=um defconfig** with
    `CONFIG_UM_HOOKS={y,n}`; cpython-parity 21/21 under
    both, bit-identical when all gates are off.
  - **Build-tested on ARCH=x86_64 defconfig** —
    `<asm/um-hooks.h>` is kernel-only-path, no leakage to
    other arches.
  - **Boot-tested** under
    `./linux mem=512M init=/bin/sh` for five-minute uptime
    on both flavours; no panics, no `WARN_ON_ONCE`.
  - **B-05 microbench** vs checked-in baseline at
    `02-workstreams/B-static-key-hot-paths/notes/
    bench-baseline.json` via
    `Documentation/virt/uml/redesign/scripts/uml-gate-bench
    {,-compare}.sh`: per-site Δ ≤ 12 % under the 15 % CI
    ceiling; all measured sites (syscall_entry / exit,
    context_switch, clock_read) under I3's 2 ns / gate
    ceiling.
  - **B-06 end-to-end selftest** at
    `tools/testing/selftests/um/hooks-flip/`:
    baseline=0, on_delta=18340, settle_delta=0.
  - **D20 cross-site IRQ test** under time-travel profile
    exercises all three sites (`do_IRQ`,
    `irq_event_handler`, `irq_do_pending_events`);
    `regs=NULL` synthetic paths exercised, no slow-path
    NULL deref.
  - **objdump verification (post-Patch-4)** confirms
    literal 5-byte NOPs at gate sites in `handle_syscall`,
    `segv`, `__switch_to`, `do_IRQ`, `timer_read` with
    `HAVE_ARCH_JUMP_LABEL=y`. C-fallback `cmpb;jle` sequence
    with `HAVE_ARCH_JUMP_LABEL=n`. Artifacts saved at
    `02-workstreams/B-static-key-hot-paths/objdump/`.
  - **`git apply --check` simulation** against a 2026-05
    `linux-next` snapshot: no conflicts. Patch 4's
    `arch/um/Kconfig` `select HAVE_ARCH_JUMP_LABEL` and
    `{uml,dyn}.lds.S` `.um_patch_text` section need
    re-validation against post-Series-4 state.

Re-run on the cherry-picked submission branch (against
post-Series-4 `master`) before `git send-email`.

## Maintainer routing

Run `scripts/get_maintainer.pl` right before send; the
2026-05 view:

```text
$ scripts/get_maintainer.pl 0001-*.patch
Richard Weinberger <richard@nod.at>           (MAINTAINER: USER MODE LINUX)
Anton Ivanov <anton.ivanov@cambridgegreys.com>   (MAINTAINER: USER MODE LINUX)
Johannes Berg <johannes@sipsolutions.net>     (MAINTAINER: USER MODE LINUX)
linux-um@lists.infradead.org                   (open list: USER MODE LINUX)
linux-kernel@vger.kernel.org                   (open list)
```

The MAINTAINERS file's USER MODE LINUX section covers
`arch/um/` already; Patch 6 adds
`arch/um/include/asm/um-hooks.h`,
`arch/um/kernel/hooks.c`, `arch/um/kernel/hooks_bench.c`,
`arch/um/kernel/um_debugfs.c`,
`arch/um/kernel/jump_label.c`,
`Documentation/virt/uml/debugfs.rst`, and
`Documentation/virt/uml/hooks.rst` to the path list so
future runs of `get_maintainer.pl` on those paths route
correctly.

Manual additions for cover-letter Cc:

  - **Benjamin Berg `<benjamin@sipsolutions.net>`** —
    author of the 6.16-merged seccomp work that Series 4
    wraps; same review-signal logic applies here since
    Series 5's hook insertions sit on the seccomp-shaped
    syscall path Series 4 abstracts.

  - **Tiwei Bie `<tiwei.btw@antgroup.com>`** — UML
    contributor via the Ant Group port; reviews most
    cross-cutting UML series.

  - **Steven Rostedt `<rostedt@goodmis.org>`** + **Masami
    Hiramatsu `<mhiramat@kernel.org>`** — tracing infra
    maintainers. The `um_hook_trace_syscalls` gate's
    eventual slow-path consumer (Series 6b) delegates to
    `include/trace/events/syscalls.h`; signalling here so
    they see the framework before its tracing consumer
    arrives. Not asking for ack on Series 5 itself, which
    is the framework.

  - **`linux-trace-kernel@vger.kernel.org`** — open list
    for the same reason.

  - **`linux-kernel@vger.kernel.org`** — open list per
    kernel convention.

Total Cc list (target): 5 maintainers + 3 lists. Well
under the "20 recipients max for a single mail" LKML
heuristic.

### Send recipe (when authorized)

```text
cd /tmp/static-key-hot-paths-rfc-emit
git send-email \
    --to=linux-um@lists.infradead.org \
    --cc=linux-kernel@vger.kernel.org \
    --cc=linux-trace-kernel@vger.kernel.org \
    --cc=richard@nod.at \
    --cc=johannes@sipsolutions.net \
    --cc=anton.ivanov@cambridgegreys.com \
    --cc=benjamin@sipsolutions.net \
    --cc=tiwei.btw@antgroup.com \
    --cc=rostedt@goodmis.org \
    --cc=mhiramat@kernel.org \
    *.patch
```

(Rerun `scripts/get_maintainer.pl --file
arch/um/include/asm/um-hooks.h` right before sending to
pick up any maintainer rotation since 2026-05.)

## checkpatch findings (anticipated)

Projected from the on-branch state until the squash audit
produces the actual `.patch` files:

  - **Patch 1:** possible `MACRO_ARG_REUSE` on the
    `um_on_*()` `__always_inline` helpers (today's helpers
    don't trigger it; flagged for v2 review feedback).
  - **Patches 2-3:** zero new warnings expected.
  - **Patch 4:** linker-script changes to `{uml,dyn}.lds.S`
    where checkpatch's handling is limited; structural
    warnings here are likely false positives.
  - **Patch 5:** `LONG_LINE` on `hooks_bench.c` `pr_info`
    formats printing `ns × 1000`. Acceptable false positive.
  - **Patch 6:** zero warnings on `.rst` files and the
    selftest shell script.

Per `ftrace-notrace-generic-v1/SUBMISSION-NOTES.md`
precedent, prose `.md` files pass checkpatch `--no-tree -f`
at 0/0 because they contain no source-code patterns; only
`.patch` files trigger real findings.

## False-positive false alarms to pre-empt

  - **"Why not a single observability tracepoint?"**
    Tracepoints pay a non-zero cost even when no consumer
    is attached (jump-label check + iteration over
    `__tracepoint_*`). A static-key gate with a function-
    pointer slow path is the cheaper equivalent;
    tracepoint-loving consumers can hang one off the slow
    path. B-02 Q1 walks this.

  - **"Why declare gates kernel-only?"** Per D18,
    USER TUs in `arch/um/os-Linux/` and `arch/um/backend/
    <kind>/` cannot link against `<linux/static_key.h>`.
    Trap loops in those USER TUs hand off to kernel TUs
    (`handle_syscall`, `segv`, ...) where the gates fire.
    Mirrors the `<asm/backend.h>` / `<shared/backend.h>`
    boundary Series 4 establishes.

  - **"`select HAVE_ARCH_JUMP_LABEL` without
    `text_poke_bp`?"** Patch 4's mprotect-based poke
    wrapper is safe because UML is single-threaded at the
    host level during boot and per-CPU pthread serialised
    after; no cross-CPU racing reader of the patch site
    during transform. `text_poke_bp` depends on INT3
    trampoline + fixmap that UML doesn't share. D19
    catalogues alternatives.

  - **"Why not gate `mm_map`/`mm_unmap`?"** B-01's
    profile-driven check showed calls well under the
    1k/sec gate-waste threshold; the gate would pay NOP
    cost on every mmap for zero benefit. Workstream C-08
    revisit if syzkaller surfaces a gap.

## Why this series stands alone — the upstream-interest pitch

The static-key substrate is upstream-friendly for three
reasons:

  1. **It cleans up arch/um's existing runtime-feature
     checks.** Today every "is feature N enabled" check on
     the syscall / fault / IRQ / clock paths is an
     ad-hoc global or per-task field load + branch. Series
     5 lifts those into named, runtime-toggleable static-
     key gates with a uniform debugfs surface. Reviewers
     see a net negative line count on the
     conditional surface even though the new files
     (`hooks.c`, `um-hooks.h`, `um_debugfs.c`) add LoC.

  2. **It unlocks the JIT-patching substrate for the rest
     of `arch/um/`.** Patch 4's `.um_patch_text` section
     split + mprotect helpers are the
     load-bearing piece for everything that wants runtime
     code modification: static_branch JIT (this series),
     ftrace mcount patching (Series 6b), kprobe insertion
     (Series 6a), BPF JIT code allocation (Series 1
     follow-up, future). Series 5 ships the substrate;
     later series consume it.

  3. **The substrate lives entirely in arch/um.** No
     cross-subsystem impact. `kernel/`, `mm/`, `fs/`,
     `net/`, `drivers/` are all unaffected. Series 5's
     maintainer-list is `linux-um@` + `linux-trace-kernel@`
     for the tracing-substrate signal; other subsystem
     maintainers have no review surface here.

## v1 → v2 if review feedback comes in

Keep `umlctl-deploy` as-is; cherry-pick the squashed v2
patches onto a fresh submission branch per iteration:

```text
git checkout -b static-key-hot-paths-rfc-v2 master
git cherry-pick <6 squash-audit outputs from v2 pass>
git format-patch -6 --cover-letter --subject-prefix='PATCH v2'
```

Likely v2 deltas (from cover letter §"Anticipated review
questions"): rename `.um_patch_text` → `.text.patchable`
(q (e)); split `um_on_irq_entry` into `_trap` / `_synth`
wrappers (q (d)); fuse `kcov_enabled` + `kfence_sample`
into `sanitizer_active` (q (a)); expose
`stats_percpu` debugfs (q (c)); split Patch 4 off as a
precursor series (q (b), narrows this series to 4
patches). Drop `RFC` prefix once a maintainer acks the
shape.

## Cross-references — lineage back to the UML redesign

  - `00-vision.md` — three-layer architecture (Layer 2 =
    static-key gates).
  - `01-architecture/three-layers.md` — per-layer
    rationale.
  - `01-architecture/invariants.md` §I3 — "off-state cost
    ≤ 2 ns per gate"; Patch 5's bench harness is the CI
    enforcement.
  - `02-workstreams/B-static-key-hot-paths/` — B-01..B-06
    tasks + notes; `notes/hot-paths.md` is the
    authoritative inventory of insertion sites;
    `notes/jump-label-status.md` is the C-fallback-vs-JIT
    walk Patch 4 closes (D19).
  - `04-risks/decisions-log.md` — D17, D18, D19, D20, D21,
    D22, D23, D45, D63 (cover letter §"Lineage" for the
    per-D walk).
  - `06-sequencing/PLAN-2026-05-14.md` §6.2 — Series 5
    queue position + rationale.
  - `upstream-patches/SUBMISSION-QUEUE.md` row 5 — the
    per-series sequencing decision.

Lineage is NOT cited in the upstream commit messages per
the "reviewers see the patch on its own merits" rule in
`upstream-patches/README.md`. The redesign branch is
offered for inspection; nothing in it depends on Series 5
landing upstream.

## When this lands

On acceptance:

  1. Add the upstream commit SHA range to this file
     (`Status` section header note).
  2. Move this directory to
     `upstream-patches/landed/static-key-hot-paths-series/`
     per `upstream-patches/README.md`.
  3. Update `SUBMISSION-QUEUE.md` row 5 status: "landed
     (upstream <SHA range>)".
  4. Unblock Series 6 sub-series (6a/6b/6c/6d) in the
     queue — each consumer slow path replaces a Series 5
     counter-only stub.
  5. Update `02-workstreams/B-static-key-hot-paths/README.md`
     to mark the workstream as "landed upstream +
     in-fork extensions deferred to Series 6 (consumers)."
  6. Note in `04-risks/decisions-log.md` D19 follow-up
     that the C-fallback / JIT-NOP transition is now
     upstream-visible; mark D19 as superseded.

## Status

  - Cover letter: **DRAFTED 2026-05-14** (this revision).
  - SUBMISSION-NOTES.md: **DRAFTED 2026-05-14** (this file).
  - README.md: **DRAFTED 2026-05-14** (reader's guide).
  - Patches: **NOT YET EMITTED.** Awaiting Series 4 landing
    + squash audit pass on the on-branch substrate.
  - Submission branch: **not yet created.** Will be
    `static-key-hot-paths-rfc-v1-submit` off post-Series-4
    `master`.
  - Send timeline: gated on Series 4 landing; realistic
    earliest send-out is ~2-4 weeks after Series 4 reaches
    a maintainer-ack.
