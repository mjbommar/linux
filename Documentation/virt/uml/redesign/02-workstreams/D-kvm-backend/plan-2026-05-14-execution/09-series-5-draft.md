# Series 5 (static-key-hot-paths-series) — DRAFTED (2026-05-14)

## What

Landed cover letter + SUBMISSION-NOTES + reader's-guide README
under `Documentation/virt/uml/redesign/upstream-patches/
static-key-hot-paths-series/`:

  - `0000-cover-letter.patch.md` — 935 lines, draft cover
    letter heading the RFC series (target: `[PATCH RFC 0/6]
    um: static_key gates on arch/um hot paths`). Walks the
    six gates declared in `<asm/um-hooks.h>` and the six hook
    helpers across seven call sites (`handle_syscall`,
    `segv`, `__switch_to`, three IRQ-delivery sites per D20,
    `timer_read`), shows a concrete pre-Series-5
    `handle_syscall` example with five ad-hoc "is feature
    enabled?" conditionals, proposes the 6-patch landing
    sequence with each intermediate state bisect-clean per
    D23.

  - `SUBMISSION-NOTES.md` — 476 lines, operational notes.
    Carries the on-branch commit map (which on-branch
    commit feeds which 0001-* slot — `570b0c386e0c` mega
    + D20/D21/D22 fixups), squash audit checklist,
    pre-submission cleanups (drop `Co-authored-by:`,
    michael.bommarito@gmail.com identity, no UML-specific
    log references in commit messages), maintainer routing
    (linux-um@ primary; linux-trace-kernel@ for the
    `trace_syscalls` gate's downstream consumer signal;
    Richard / Johannes / Anton / Benjamin / Tiwei + Steven
    / Masami as Cc), checkpatch expectations per patch, and
    post-landing cleanup checklist (move to `landed/`,
    supersede D19, unblock Series 6 sub-series).

  - `README.md` — 78 lines, brief reader's guide describing
    the directory contents and the relationship to the
    B-workstream memos under
    `02-workstreams/B-static-key-hot-paths/`.

All three files pass `scripts/checkpatch.pl --no-tree -f`
at 0 errors / 0 warnings.

## Why now

PLAN-2026-05-14 §6.2 row 5 had Series 5 as **to write**.
The B-workstream substrate has been validated since
2026-04-18 (six tasks closed: hot-path audit, gate design,
debugfs surface, section split, microbench harness with
checked-in baseline, B-06 end-to-end flip demo at
`tools/testing/selftests/um/hooks-flip/` PASSing with
baseline=0, on_delta=18340, settle_delta=0). External
review on 2026-04-23 surfaced three findings
(D20 / D21 / D22) folded back into the on-branch substrate.

Drafting now lets the cover letter and SUBMISSION-NOTES go
through a review pass in parallel with Series 4 drafting;
the actual `git format-patch` emission still waits on
Series 4 landing upstream (Series 5's hook insertion sites
are immediately downstream of Series 4's HOT-op dispatch
surface; sending Series 5 ahead would force reviewers to
evaluate gates against a moving target).

D63 lists Series 5 as the second of the arch/um RFC pair
(Series 4 = ops table, Series 5 = static-key substrate).
PLAN-2026-05-14 §6.2 marks Series 2-5 as parallelisable
opportunistic sends. The drafting work now reduces the
lead time once Series 4 reaches a maintainer-ack.

## What's still missing before send

  1. **Series 4 lands upstream first.** Operational
     hygiene per D63: Series 5's hook insertions sit on
     code Series 4 reshapes. Strictly speaking
     compile-independent (the C-fallback jump-label form
     is arch-generic and the hook call sites exist in
     today's tree), but reviewer workflow benefits from
     dependency ordering. If Series 4 stalls past Series 5's
     emission window, Series 5 may emit ahead with a
     cover-letter note.

  2. **Squash audit pass on the on-branch substrate.** The
     B-workstream lives as one ~1500 LoC megacommit
     (`570b0c386e0c` "um: introduce Layer 2 static-key
     hot-path gates") plus three follow-on fix commits
     (D20 three-site IRQ extension, D21 sanitize_paranoid
     removal, D22 atomic-to-per-CPU stats rewrite). Per D23
     the upstream emission splits by compile boundary into
     six patches with each intermediate state bisect-clean.
     The D20/D21/D22 fixups fold into the patches that
     introduce the relevant surface (not as separate
     after-the-fact fix patches). See SUBMISSION-NOTES.md
     §"On-branch commit map" for the per-slot input.

  3. **Cherry-pick onto a submission branch.**
     `static-key-hot-paths-rfc-v1-submit` off
     post-Series-4 mainline `master` (NOT `umlctl-deploy`,
     NOT pre-Series-4 master), with `Co-authored-by:`
     trailers dropped and UML-specific log references
     removed from commit messages.

  4. **Build verification per intermediate state.** Each
     of the 6 patches must:
       (a) build clean on `make ARCH=um O=/tmp/uml-N
           defconfig` with `CONFIG_UM_HOOKS={y,n}`
       (b) boot under `./linux mem=512M init=/bin/sh`
       (c) pass cpython-parity 21/21 with all gates off
           (bit-identical to `CONFIG_UM_HOOKS=n`)
     Failing intermediate states must be fixed in the
     squash, not papered over; per D23 + kernel
     commit-bisect discipline every commit must work.

  5. **`scripts/get_maintainer.pl` rerun** right before
     send to pick up rotation since 2026-05.

## The 3 hardest open questions flagged in the cover letter

(For the diary record + future-session catchup.)

  1. **Six gates vs three vs one.** Cover letter
     §"Anticipated review questions" (a). The gate-per-
     concern rule (B-02 Q2) was deliberate — independent
     toggles, per-gate cost accounting in
     `/sys/kernel/debug/um/stats`. The alternative — one
     umbrella `um_hook_observability` that fans out to N
     slow paths — was rejected because consumers share a
     fate (turn one on, turn them all on). Reviewers may
     prefer a smaller gate set (fuse `kcov_enabled` +
     `kfence_sample` into `sanitizer_active`); the change
     is mechanical. The B-02 Q1 design memo walks the
     "tracepoint vs gate" trade for completeness, but the
     gate-set granularity is the load-bearing question.

  2. **`HAVE_ARCH_JUMP_LABEL` depends on the section
     split.** Cover letter §"Anticipated review
     questions" (b). Patch 4 adds `select
     HAVE_ARCH_JUMP_LABEL` only after adding the
     `.um_patch_text` section split + mprotect helpers
     (D19 alternative 1: implement `text_poke` without the
     section split was rejected because it makes all of
     `.text` writable for the duration of the patch — a
     security regression relative to today's host-enforced
     RO text). Reviewers may want to see the section split
     as its own preceding 1-2-patch series, narrowing
     Series 5 to 4 patches. Mechanical to split; v2
     accommodation is straightforward. The
     `04-section-split.md` design memo carries the
     constraint walk.

  3. **`regs=NULL` contract on time-travel synthetic IRQ
     paths.** Cover letter §"Anticipated review
     questions" (d). The `um_on_irq_entry` helper fires
     from three sites per D20 (`do_IRQ`,
     `irq_event_handler`, `irq_do_pending_events`); the
     two synthetic paths don't have a trap `struct
     uml_pt_regs` available, so the hook is called with
     `regs=NULL`. Slow paths today swallow `regs` so this
     is harmless. Reviewers may prefer explicit `_trap` vs
     `_synth` wrapper helpers (eliminating the NULL
     contract at the cost of two helpers per IRQ-delivery
     shape). The simpler one-helper-three-sites design
     with a NULL contract is on the branch; the wrapper
     alternative is accommodatable but adds visible
     complexity to the helper API. The B-01 audit's "no
     hot-path escape" invariant is the load-bearing
     argument for covering all three sites; the helper
     shape (one with NULL contract vs two wrappers) is
     where reviewer preference can override.

A fourth open question — the section name `.um_patch_text`
vs the generic `.text.patchable` (cover letter (e)) — is
also flagged but is a search-and-replace, not a design
debate. The fifth (per-CPU stats file exposure, (c)) is
similarly mechanical.

## On-branch commit → 0001-000X slot map (the squash audit input)

| Slot     | Subject (target)                                          | On-branch commits                                                                                          |
|----------|-----------------------------------------------------------|------------------------------------------------------------------------------------------------------------|
| Patch 1  | um: hooks: gate infrastructure + first live hook site     | `570b0c386e0c` (header + `kernel/hooks.c` + first `um_on_syscall_entry` hunks) + D21 fold + D22 fold       |
| Patch 2  | um: hooks: remaining hot-path insertions                  | `570b0c386e0c` (rest of `kernel/skas/syscall.c` tail + `kernel/trap.c` + `kernel/process.c` + `kernel/irq.c` + `kernel/time.c` hunks) + D20 fold |
| Patch 3  | um: hooks: debugfs control surface                        | `570b0c386e0c` (`kernel/um_debugfs.c` + `Documentation/virt/uml/debugfs.rst` hunks) + D22 fold              |
| Patch 4  | um: hooks: `.um_patch_text` section split + JIT patching  | `570b0c386e0c` (`{uml,dyn}.lds.S` + `kernel/jump_label.c` + `arch/um/Kconfig` `select HAVE_ARCH_JUMP_LABEL`) |
| Patch 5  | um: hooks: benchmark harness + checked-in baseline        | `570b0c386e0c` (`kernel/hooks_bench.c` + `scripts/uml-gate-bench{,-compare}.sh` + `notes/bench-baseline.json`) |
| Patch 6  | um: hooks: documentation + selftest                       | `570b0c386e0c` (`Documentation/virt/uml/hooks.rst` + `tools/testing/selftests/um/hooks-flip/` + MAINTAINERS) |

The D20/D21/D22 fixup commits on the branch are NOT
shipped as their own patches; they fold into the patches
above per D23's compile-boundary rule (and per kernel
commit-bisect discipline — shipping the original B-02
form followed by an after-the-fact fix would create a
known-bad intermediate).

## B-workstream memo → 0001-000X slot driver map

For the diary record (which design memo drove which
upstream patch slot):

  - **B-01 audit** + **`notes/hot-paths.md`** → **Patch 1
    + Patch 2.** The hot-path inventory tells Patches 1-2
    where each of the six hook helpers go and what gates
    fire from each site. Patch 1's "one inserted site"
    chooses `um_on_syscall_entry` per D23 step 1; Patch 2
    covers the remaining six call sites (the IRQ helper's
    three sites per D20 are all in Patch 2).
  - **B-02 gate design** → **Patch 1.** The macro design
    (`<asm/um-hooks.h>` header, `um_on_*()`
    `__always_inline` helpers, slow-path stub prototypes
    in `kernel/hooks.c`) lands as Patch 1's contract
    surface. D21's sanitize_paranoid drop folds into
    Patch 1 (six gates land directly, no
    intermediate-then-removed gate). D22's per-CPU stats
    rewrite folds into Patch 1 (per-CPU counter shape
    lands directly, no global-atomic intermediate).
  - **B-03 debugfs controls** → **Patch 3.** The
    `/sys/kernel/debug/um/{hooks,stats,backend}` surface
    and `Documentation/virt/uml/debugfs.rst`.
  - **B-04 section split** → **Patch 4.** The
    `.um_patch_text` section in `{uml,dyn}.lds.S`, the
    mprotect helpers, `arch/um/kernel/jump_label.c`, and
    `select HAVE_ARCH_JUMP_LABEL` in `arch/um/Kconfig`.
    This patch closes D19 (Layer 2 gates JIT-patched to
    literal NOPs rather than C-fallback form).
    Cross-cutting: the section split is also the substrate
    that Series 6b (ftrace mcount), Series 6a (kprobes),
    and future BPF JIT consume.
  - **B-05 benchmark targets** → **Patch 5.** The kernel-
    side bench at `kernel/hooks_bench.c`, the host scripts
    `Documentation/virt/uml/redesign/scripts/uml-gate-bench
    {,-compare}.sh`, and the checked-in baseline at
    `02-workstreams/B-static-key-hot-paths/notes/
    bench-baseline.json`. Methodology is first-pass per
    B-05; per-gate isolation (toggle-sweep with only one
    gate on at a time) is deferred to B-05-precise
    follow-up. Patch 5 includes the 15% per-site CI
    ceiling that enforces invariant I3.
  - **B-06 first-flip demo** → **Patch 6.** The end-to-
    end selftest at
    `tools/testing/selftests/um/hooks-flip/` plus
    `Documentation/virt/uml/hooks.rst` (the architecture
    spec — what each gate observes, how to add a new gate)
    and the MAINTAINERS path-list update for the new
    arch/um/ files.

The 1:1 (well, 1:2 for B-01, 2:1 for B-04+B-05's
section-split + bench pairing) mapping reflects the
compile-boundary split D23 chose. The B-NN task boundary
is too fine-grained for LKML review (B-NN steps often touch
files that only make sense in context of the next B-NN
step); the compile-boundary split keeps each upstream
patch self-contained.

## Total LoC (this diary entry + new directory)

```
README.md                                78 lines
SUBMISSION-NOTES.md                     476 lines
0000-cover-letter.patch.md              935 lines
09-series-5-draft.md (this)            ~260 lines
SUBMISSION-QUEUE.md                      +0 (in-place row 5 edit)
                                       ─────
                                       ~1749 lines new content
```

(SUBMISSION-QUEUE.md change is a single row's status flip
from `**to write**` to `**DRAFTED 2026-05-14**`; counts as
0 LoC delta beyond the inline edit.)

## Diary cross-references

This entry is workstream-D's logbook for plan-2026-05-14
execution. Series 5 is B-workstream content; the execution
diary lives here because the entire PLAN-2026-05-14
execution trail is workstream-D's. The substantive Series 5
substrate is under
`02-workstreams/B-static-key-hot-paths/`.

Companion entries in this diary:

  - `00-kickoff.md` — execution kickoff.
  - `06-series7-cover-letter-refresh.md` — Series 7 cover
    letter refresh.
  - `07-series-4-draft.md` — Series 4 drafting pass.
  - `08-t55-up-hop-bisect.md` — T55 UP-hop bisect (Polish
    track, parallel to this).
  - `09-series-5-draft.md` (this entry) — Series 5
    drafting pass.

The next diary entry will record either the squash audit
pass when Series 4 lands and Series 5's squash becomes
unblocked, or the Series 6 sub-series drafting if that
work starts before Series 4 reaches LKML.
