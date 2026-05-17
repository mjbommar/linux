# Series 6 (kprobes-ftrace-kfence-kcsan-profiles-series) — DRAFTED (2026-05-14)

## What

Landed cover letter + SUBMISSION-NOTES + reader's-guide README
under `Documentation/virt/uml/redesign/upstream-patches/
kprobes-ftrace-kfence-kcsan-profiles-series/`:

  - `0000-cover-letter.patch.md` — 1354 lines, draft shared
    cover letter heading the bundle (target: `[PATCH RFC 0/20]
    um: profile-driven feature selection (kprobes, ftrace,
    KFENCE, KCSAN)`). Walks the four sub-series's pre-Series-6
    broken states (kprobes / ftrace / KFENCE silently off via
    `make olddefconfig`'s `depends on HAVE_*` short-circuit;
    function_graph half-broken at runtime via the TINY_RCU-on-UP
    / atomic-context interaction), the 4-sub-series shape
    (~20 patches total: ~5 + ~6 + ~4 + ~5 per feature), the
    migration plan (6a → 6b → 6c → 6d ordering), the
    dependency story (Series 4 + 5 first; sub-series 6b
    depends on Series 3 for function_graph leak source 3
    closure), the performance pitch (zero overhead when off
    via `#ifdef CONFIG_*=n`; Series 5 static-key gates carry
    the on-state at ~0.3 ns per gate JIT-NOP), and the 4-way
    maintainer routing (trace-kernel for 6a + 6b; mm for 6c +
    6d; linux-um cc'd on all four).

  - `SUBMISSION-NOTES.md` — 810 lines, operational notes per
    `upstream-patches/README.md` convention. Carries the
    per-sub-series on-branch commit maps (which in-fork
    commits feed which 6N/0001-* slot), per-sub-series squash
    audit checklists, pre-submission cleanups (drop
    `Co-authored-by:`, michael.bommarito@gmail.com identity,
    no UML-specific log references in commit messages), 4-way
    maintainer routing recipes (per-sub-series `git
    send-email` recipes), checkpatch expectations, false-
    positive false-alarms to pre-empt, post-landing cleanup
    checklist (move to `landed/` once ALL FOUR have landed;
    unblock Series 7's record/replay function_graph cite).

  - `README.md` — 143 lines, brief reader's guide describing
    the directory contents, the four sub-series at a glance,
    and the relationship to the C-workstream memos under
    `02-workstreams/C-profiles-and-gaps/`.

All three files pass `scripts/checkpatch.pl --no-tree -f`
at 0 errors / 0 warnings.

`upstream-patches/SUBMISSION-QUEUE.md` row 6 status flipped:
**to write → DRAFTED 2026-05-14**.

## Why now

PLAN-2026-05-14 §6.2 row 6 had Series 6 as **to write**.
The C-workstream substrate has been validated across four
sub-tracks since 2026-04-22:

  - C-02 KFENCE landed 2026-04-18 (13/27 KFENCE KUnit PASS;
    failures are stack-walker-renderer issues, not KFENCE-
    functional).
  - C-03 KCSAN landed 2026-04-18 (selftest 3/3 PASS, KUnit
    20+ races).
  - C-04 kprobes + rethook + function_graph landed
    2026-04-22 (kprobes-stress 200 + 2000 iter PASS;
    graph=on at the tail of commit 3b).
  - C-05 ftrace landed 2026-04-19 (ftrace-smoke PASS with
    trace_lines=51316; +3.3 % tracer-off boot cost under
    A-07's 5 % ceiling).

Drafting now lets the cover letter + SUBMISSION-NOTES go
through a review pass in parallel with Series 4 + 5
drafting; the actual `git format-patch` emission still
waits on Series 4 + 5 landing upstream (sub-series 6a + 6b
consume Series 5's `.um_patch_text` section split +
mprotect helpers directly; all four sub-series's hook
insertion points lie on the trap path Series 4 reshapes).

D63 lists Series 6 as the third of the arch/um RFC bundle
(Series 4 = ops table, Series 5 = static-key substrate,
Series 6 = consumer features). PLAN-2026-05-14 §6.2 marks
Series 2-5 as parallelisable; Series 6 (this drafting work)
follows the same opportunistic-send shape with one twist:
the four sub-series ship at ~1-week intervals to avoid
overwhelming `linux-um@` cc'd reviewers with four parallel
RFC threads.

D63 also notes the cover-letter framing decision: bundle
the four sub-series with shared parent context for the
`linux-um@` cross-cutting reviewers, but route each
sub-series's primary maintainer audience independently
(linux-trace-kernel@ for 6a + 6b, linux-mm@ for 6c + 6d).
This shape gives reviewers per-feature focus without
losing parent context.

## Cover letter highlights

Differences from Series 4 + 5 cover letters worth flagging:

  - **4-sub-series structure.** Series 4 + 5 were each a
    single RFC series. Series 6 ships as four parallel
    `[PATCH RFC NN/MM]` sub-series with a shared parent
    cover letter. The sub-series stand alone; the parent
    cover gives the bundle context.

  - **No new infrastructure.** Series 4 introduces the ops
    table; Series 5 introduces the static-key gate
    substrate. Series 6 just consumes both: each sub-series
    is the per-feature arch enablement (4-6 patches per
    feature) on top of the substrate landed in Series 4 + 5.

  - **Per-feature broken-state walk.** The cover letter
    §"The problem in arch/um today" gives one concrete
    failure mode per feature:
    (1) kprobes silently off via `make olddefconfig` →
        `insmod kprobe_example.ko` returns
        "Operation not supported".
    (2) ftrace silently off → `/sys/kernel/tracing/` missing
        entirely; no `current_tracer`.
    (3) KFENCE silently off → fuzz target has no
        sampling-allocator catching slab OOBs.
    (4) KCSAN silently off + KASAN mutual-exclusion catch.
    (5) function_graph half-broken at runtime → atomic-
        context shadow-stack push trips `__might_resched`
        on UML-UP's TINY_RCU.

  - **Dependency chain made explicit.** Series 6 depends on
    Series 4 (ops table) AND Series 5 (`.um_patch_text` +
    mprotect helpers) AND Series 3 (notrace on generic
    kthread/smpboot_thread_fn). The longest dependency
    chain in the queue.

  - **3 hard open questions flagged for SUBMISSION-NOTES
    inventory** — bundle-vs-independent send, sub-series
    ordering, KFENCE KUnit stack-walker mismatch
    (q-a/q-b/q-c per cover-letter convention).

## What's still missing before send

  1. **Series 4 lands upstream first.** Operational
     hygiene per D63: Series 6's hook insertion sites
     (`relay_signal()`, `segv()`, `mcount.S` calls,
     `arch/um/kernel/trap.c`) lie on code Series 4
     reshapes. Reviewer workflow benefits from dependency
     ordering. If Series 4 stalls past Series 6's
     emission window, sub-series 6a and 6c may emit
     ahead (no direct Series 4 substrate consumption;
     the trap-path hooks are arch-generic). Sub-series
     6b and 6d are tighter dependencies.

  2. **Series 5 lands upstream first.** Sub-series 6a's
     kprobes-arming and sub-series 6b's ftrace
     NOP↔call patching directly consume Series 5's
     `um_kernel_text_patch_begin/end` mprotect helpers
     and the `.um_patch_text` section split.

  3. **Series 3 lands upstream first.** Sub-series 6b's
     function_graph consumes Series 3's `notrace` on
     generic `kthread()` / `smpboot_thread_fn()` for
     leak source (3) closure.

  4. **Per-sub-series squash audits.** Four separate
     squash audits, one per sub-series:
       - 6a: 5 in-fork commits → 4-5 patches.
       - 6b: 6-9 in-fork commits → 5-6 patches.
       - 6c: 3-4 in-fork commits → 4 patches.
       - 6d: 4-5 in-fork commits → 5 patches.

  5. **Four submission branches** off post-Series-5
     `master`:
       - `kprobes-rfc-v1-submit`
       - `ftrace-graph-rfc-v1-submit`
       - `kfence-rfc-v1-submit`
       - `kcsan-rfc-v1-submit`

  6. **Re-run `scripts/get_maintainer.pl`** per
     sub-series right before each `git send-email`.

  7. **Send cadence.** ~1 week between sub-series sends
     to avoid `linux-um@` reviewer overload.

## What this DRAFTED state delivers

  - Reviewers (the user; later, the LKML maintainer
    audience) can read the cover letter + SUBMISSION-NOTES
    now to evaluate the bundle shape against the C-
    workstream memos before any patches are emitted.
  - The 4-way maintainer routing is captured per sub-
    series; future automation can extract the `git
    send-email` recipes from SUBMISSION-NOTES directly.
  - The per-sub-series on-branch commit map is the
    authoritative input for the squash audits; the
    squash auditors (post-Series-5-landing) have a clear
    target shape.
  - The dependency chain is explicit: any movement on
    Series 3 / 4 / 5 directly affects Series 6's
    emission readiness, captured in this file's
    SUBMISSION-NOTES "Blockers" section.

## What this DRAFTED state does NOT yet deliver

  - `0001-*.patch` files per sub-series (await Series 4
    + 5 landing + squash audits).
  - Per-sub-series cover letters (derived from this
    shared frame; each sub-series gets its own as part
    of the `git format-patch --cover-letter` emission).
  - Validated upstream-friendly diffstats (the cover
    letter's diffstat block has placeholder `XXX` /
    `XX` line counts; post-squash-audit fills these
    in).
  - Re-bench numbers against post-Series-5 mainline
    (the cover letter's performance numbers are the
    in-fork numbers; reviewers will want post-rebase
    numbers).

## Cross-references

  - `Documentation/virt/uml/redesign/upstream-patches/
    kprobes-ftrace-kfence-kcsan-profiles-series/
    0000-cover-letter.patch.md` (the cover letter).
  - `Documentation/virt/uml/redesign/upstream-patches/
    kprobes-ftrace-kfence-kcsan-profiles-series/
    SUBMISSION-NOTES.md` (the operational notes).
  - `Documentation/virt/uml/redesign/upstream-patches/
    kprobes-ftrace-kfence-kcsan-profiles-series/
    README.md` (the reader's guide).
  - `Documentation/virt/uml/redesign/upstream-patches/
    SUBMISSION-QUEUE.md` row 6 (row status flipped to
    DRAFTED 2026-05-14 in this session).
  - `Documentation/virt/uml/redesign/02-workstreams/
    C-profiles-and-gaps/` (the C-workstream substrate;
    each `0N-port-<feature>.md` drives one sub-series).
  - `Documentation/virt/uml/redesign/04-risks/
    decisions-log.md` D23, D26, D27, D28, D29, D30,
    D31, D32, D33, D34 (per-sub-series decisions).
  - `Documentation/virt/uml/redesign/06-sequencing/
    PLAN-2026-05-14.md` §6.2 row 6 (the parent plan).
