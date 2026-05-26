# Upstream-bound patch series derived from the UML redesign

This directory holds patch series that came out of the UML
redesign but which belong upstream **independent of UML** — they
are standalone hygiene / abstraction fixes that the broader
kernel benefits from regardless of whether UML ever consumes
them.

Each sub-directory is one series, in `git format-patch` form,
with a `SUBMISSION-NOTES.md` describing:

- pre-submission cleanups (trailers, author line),
- checkpatch findings that are false positives,
- build verification the author performed,
- suggested LKML routing (`get_maintainer.pl` output or manual
  cc list),
- series lineage back to the redesign decision log.

The commit messages in the `.patch` files deliberately do NOT
mention UML — reviewers see the upstream-interest rationale on
its own merits. Traceability back to the UML workstream that
motivated each series lives in `SUBMISSION-NOTES.md`, not in
the patch bodies.

## Current series

| Series | Status | Motivating workstream | Decisions log |
|---|---|---|---|
| `bpf-hygiene-v1/` | prepared, not yet sent | C-06 (BPF JIT UML port) | D43 addendum |
| `kmsan-arch-callback-rfc/` | staged (notes only; patch not written) — framing in D51 is the "no visible impact, just benefits" pitch: new weak `kmsan_arch_init_early_shadow()` hook + un-static `kmsan_record_future_shadow_range()`, byte-identical behavior for x86/s390. Upstream-side UML consumer no longer blocks on it (D62 landed the VMALLOC-quarter-split redesign which works independently). | C-07 (KMSAN UML port) | D44 resolution + D51 + D62 |
| `ftrace-notrace-generic-v1/` | prepared 2026-04-24, not yet sent — cover letter + 1 patch (`kthread()` + `smpboot_thread_fn()` notrace) + SUBMISSION-NOTES in-tree | C-04 (kprobes + function_graph strip infrastructure) | D34 three-leak-source analysis + D34 addendum-3 + D45 in-fork scope policy |
| `kvm-backend-series/` | scoped 2026-04-24 post-G8/D70=GO — SUBMISSION-NOTES (planned 15-patch ordering, squash plan, prerequisites) + cover-letter draft (`0000-cover-letter.patch.md`) in-tree; actual patches await Series 4 landing | D workstream (entire KVM backend + systrap gadget) | D70 / D79 (G8 GO) + D66-D86 (per-step decisions) |

**Full submission queue + sequencing** lives in
`SUBMISSION-QUEUE.md` in this directory (Phase VI Lift #7
deliverable, 2026-04-23). That doc orders all seven
upstream-bound series (the two above plus five "to write")
and records the per-series maintainer routing + framing
pitches. Decisions-log D63 summarizes the sequencing call.

## Conventions

- Directory name: `<subsystem>-<what>-v<N>/` (e.g. `bpf-hygiene-v1`,
  `ftrace-patchable-abstraction-v1`). Bump `v<N>` on each round
  of review feedback; keep prior versions for history.
- `0000-cover-letter.patch` with a filled-in Subject + blurb.
- Per-patch files with upstream-style commit messages (no UML
  references in the commit text unless the patch is literally
  UML-only).
- `SUBMISSION-NOTES.md` documenting everything a future picker
  needs to `git send-email` the series.

## Why this layout

The UML redesign runs in a personal branch (`uml-redesign-plan`)
that carries many UML-specific commits. Upstream patches that
came out of redesign investigation need to cherry-pick cleanly
onto `bpf-next` / `linux-next` / etc. without dragging the UML
context. This directory lets us track them in-tree alongside
their motivating decisions-log entries while keeping them
submission-ready.

When a series lands upstream, add a note to the SUBMISSION-NOTES
with the upstream commit ID and move the directory to
`upstream-patches/landed/` to keep the queue visible.
