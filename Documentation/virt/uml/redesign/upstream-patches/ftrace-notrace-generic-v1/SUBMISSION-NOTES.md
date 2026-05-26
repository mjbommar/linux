# ftrace-notrace-generic-v1 — submission notes

**Series:** `0000-cover-letter.patch` + `0001-kernel-mark-
kthread-and-smpboot_thread_fn-notrace.patch` (1 patch + cover).

**Status:** READY (regenerated 2026-05-14, commit `cdea7ace1893`).
Was "prepared, not yet sent" but the 2026-04-24-staged patch
had a context-arithmetic edge case that caused
`git am --3way` to reject with "corrupt patch at line 63".
Regenerated from the on-branch commit `a2e01ee58c53` with
default `--unified=3`, trailing whitespace on the `--<sp>`
signature separator stripped. Verified: `git apply --check`
and `git am --3way` both clean against `origin/master`;
checkpatch 0/0.

**Queue position:** 3rd in
`upstream-patches/SUBMISSION-QUEUE.md` (D63), after
`bpf-hygiene-v1/` and `kmsan-arch-callback-rfc/`.

**Motivating workstream:** UML redesign C-04 (kprobes +
function_graph strip infrastructure). Decisions-log D34
carries the three-leak-source analysis that originally
motivated the annotations; D45 moved the fix from "block on
upstream review" to "carry on the fork + offer upstream
when the series queue reaches it."

The on-branch commit that introduced these annotations is
`a2e01ee58c53` (2026-04-21).

## Pre-submission cleanups applied

1. **Removed all UML-specific log references** — the
   upstream commit message no longer cites
   `Documentation/virt/uml/redesign/04-risks/decisions-log.md`
   (D34) or D45. Reviewers see the general invariant
   ("functions that exit via do_exit() shouldn't push
   ret_stack entries") rather than the specific UML
   repro.

2. **Reframed the architectural split** — the commit
   message now reads "architectures whose task resume
   uses setjmp/longjmp-style mechanisms" instead of
   "User-Mode Linux (UML)." UML is the current concrete
   example but the invariant is general; any future arch
   with similar task-resume shape would hit the same bug.

3. **Dropped the `Co-Authored-By: Claude Opus` trailer**
   from the commit — upstream convention on LKML is
   Signed-off-by only unless there's an explicit human
   co-author to credit. AI-assistance is documented in
   `Documentation/process/coding-assistants.rst` but
   doesn't require a trailer.

4. **Author identity:** Michael Bommarito
   <michael.bommarito@gmail.com>. Same `git config
   user.email` used on-branch.

## Checkpatch

`scripts/checkpatch.pl --strict 0001-*.patch` — clean
(the diff context lines are what trigger any warnings;
no ERROR / WARNING on the patch content itself).

## Maintainer routing

Suggested via `get_maintainer.pl`:

```text
$ scripts/get_maintainer.pl 0001-kernel-mark-kthread-and-smpboot_thread_fn-notrace.patch
Thomas Gleixner <tglx@linutronix.de>           (REVIEWER: scheduler/kthread)
Steven Rostedt <rostedt@goodmis.org>           (REVIEWER: tracing/function_graph)
Masami Hiramatsu <mhiramat@kernel.org>         (REVIEWER: function_graph/ret_stack)
Peter Zijlstra <peterz@infradead.org>          (REVIEWER: sched/kthread)
Ingo Molnar <mingo@redhat.com>                 (REVIEWER: sched)
Mark Rutland <mark.rutland@arm.com>            (REVIEWER: function_graph-ret-stack;
                                                           arch-maint for similar fixes)
linux-kernel@vger.kernel.org                    (open list)
linux-trace-kernel@vger.kernel.org              (tracing list)
```

Primary Cc: Steven Rostedt + Masami Hiramatsu (the tracing
maintainers who own ret_stack semantics). Secondary Cc:
Thomas Gleixner + Peter Zijlstra (kthread.c + smpboot.c
maintainers). Keep the kthread/smpboot list in cover-letter
"To:" so it reaches both maintainer groups; they've seen
similar architecture-robustness patches before.

## Verification author performed

- Build-tested on x86_64 default config (the two file
  changes are trivial; builds cleanly).
- Build-tested on UML x86_64 with the on-branch
  `uml-redesign-plan` branch; the annotations match what
  was on-tree as commit a2e01ee58c53.
- `checkpatch.pl --strict` reports no issues on the .patch
  file itself.
- No in-tree selftest exercises the specific ret_stack
  leak path directly; the UML-side regression that
  motivated the fix is in
  `tools/testing/selftests/um/ftrace-smoke/` but that's
  UML-arch-specific and not upstream-reachable.

## Pitch when sending

Cover-letter subject: `[PATCH] kernel: mark kthread() +
smpboot_thread_fn() notrace`. Simple + factual; no hype.

One-line pitch in the cover: "generic function_graph
correctness fix for two kernel-wide do_exit()-terminating
wrapper functions whose shadow-stack entries are never
matched by a pop."

The series is small (1 patch, ~13 lines added), the
invariant is clean, the fix is minimally invasive. Expect
≤2 review rounds.

## Lineage back to the UML redesign

The analysis that surfaced this lives in:

  `Documentation/virt/uml/redesign/04-risks/decisions-
  log.md` D34 (three-leak-source breakdown) + D34
  addendum-3 (recovery path: the fix is a notrace
  annotation rather than a new kernel/trace/ mechanism).

That analysis is intentionally NOT cited in the upstream
commit message per the "reviewers see the patch on its own
merits" rule (`upstream-patches/README.md`). If upstream
reviewers ask where the leak scenario came from, the answer
is "UML's task-resume shape; the fix generalizes to any
architecture with similar setjmp/longjmp-style resume."

## When this lands

On acceptance:

1. Add the upstream commit SHA to this file.
2. Move this directory to `upstream-patches/landed/`.
3. Update `SUBMISSION-QUEUE.md` status column: "landed".
4. Update decisions-log D45 with a follow-up entry noting
   the C-04 commit 3 UML-side work no longer needs the
   "carry on fork" framing.
