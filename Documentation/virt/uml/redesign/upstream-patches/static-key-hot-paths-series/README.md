# static-key-hot-paths-series — reader's guide

Series 5 of the upstream-bound queue per
`upstream-patches/SUBMISSION-QUEUE.md` (D63).

## What's in this directory

  - `0000-cover-letter.patch.md` — draft cover letter that
    will head the `git format-patch` series once Series 4
    has landed AND the on-branch B-workstream commits have
    been squashed into the 6-patch sequence D23 specifies.
    `.md` because the source-side patches don't yet exist
    — the on-branch state is one ~1500 LoC megacommit plus
    three D20/D21/D22 fix commits.
  - `SUBMISSION-NOTES.md` — operational notes per the
    `upstream-patches/README.md` directory convention.
    Pre-submission cleanups, on-branch commit map,
    maintainer routing, post-landing cleanup checklist.
  - `README.md` (this file).

`0001-*.patch` … `0006-*.patch` appear after the squash
audit pass.

## What Series 5 is

Upstream emission of workstream B (static-key hot-path
gates). Introduces 6 gates in
`arch/um/include/asm/um-hooks.h`
(`um_hook_trace_syscalls`, `_kcov_enabled`,
`_time_travel_active`, `_kfence_sample`,
`_record_replay`, `_perf_dispatch`) dispatched from 6 hook
helpers across 7 call sites (`handle_syscall`,
`segv`, `__switch_to`, three IRQ-delivery sites per D20,
`timer_read`); a `/sys/kernel/debug/um/{hooks,stats,backend}`
debugfs surface; a `.um_patch_text` section split +
mprotect helpers that unlock JIT patching to literal NOPs
via `select HAVE_ARCH_JUMP_LABEL`; a microbenchmark
harness with checked-in baseline that CI-gates invariant
I3; and the B-06 end-to-end `trace_syscalls`-flip selftest.

The series stands alone as a cleanup of arch/um's existing
runtime-feature checks and as the load-bearing piece for
arch/um runtime code modification (ftrace mcount, kprobe
insertion, BPF JIT all consume Patch 4's
`.um_patch_text`). It does NOT enable any new functionality
— slow paths are counter stubs that Series 6 / Series 7
consumers replace.

## Relationship to B-workstream memos

The B-workstream substrate lives at
`Documentation/virt/uml/redesign/02-workstreams/B-static-key-hot-paths/`.
Key inputs:

  - `01-audit-entry-points.md` + `notes/hot-paths.md` —
    inventory of insertion sites (drives Patches 1-2).
  - `02-gate-design.md` — macro design + helper API
    (drives Patch 1).
  - `03-debugfs-controls.md` — drives Patch 3.
  - `04-section-split.md` + `notes/jump-label-status.md` —
    drive Patch 4; close D19.
  - `05-benchmark-targets.md` — drives Patch 5.
  - `06-first-flip-demo.md` — drives Patch 6.

Decisions D17, D18, D19, D20, D21, D22, D23 in
`04-risks/decisions-log.md` record the per-decision
rationale (see cover letter §"Lineage").

## When this directory changes

  - Squash audit produces the 6-patch sequence: add
    `0001-*.patch` … `0006-*.patch`; convert
    `0000-cover-letter.patch.md` → `0000-cover-letter.patch`
    with filled-in diffstat.
  - On send: SUBMISSION-NOTES flips from DRAFTED → SENT
    (with `Message-Id`).
  - On land: move directory to
    `upstream-patches/landed/static-key-hot-paths-series/`.
