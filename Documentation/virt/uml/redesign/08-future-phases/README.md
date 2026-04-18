# Future Phases / Parking Lot

This directory captures **useful but not yet committed** work beyond
the current A/B/C/D roadmap.

Purpose:

- Keep end-user-facing ideas from getting lost while workstreams A-D
  stay focused.
- Distinguish clearly between:
  - **planned now** — already covered by A-D,
  - **partially planned** — gestured at, but not fully scoped,
  - **missing** — not meaningfully on the roadmap yet.
- Give future planning passes a place to start from when the current
  critical path is stable.

Rules:

- Nothing in this directory is a commitment.
- Notes here should be framed from the point of view of an end user,
  operator, or contributor, not only from the point of view of kernel
  internals.
- If an item graduates into a real workstream task, move it out of the
  parking lot and into the main plan.

### Boundary with workstream C (and the rest of the A–D plan) — D24

A parking lot that quietly re-plans the next workstream is a failure
mode: ideas here accumulate, then collide with C's scope once C
starts, and the team re-debates decisions that were already made.

Policy: **items in this directory begin strictly where workstream C
ends.** That means:

- Any item that fits C's existing task list (any `C-NN.md` spec)
  is a C task, not a future phase. If a note here describes work
  that overlaps C, the scope either folds into C or the note is
  narrowed to the slice C explicitly defers.
- When scope is ambiguous, it moves *to* C first. Future phases
  inherit only what C has explicitly written off ("C-NN.md out of
  scope: …").
- "Phase E wants X" does not override "C plans X" — C wins by
  default; phase E takes over only if C's owner says so in writing.
- Adding a new item here requires a one-line check against C's
  task list; if it matches, reclassify the note as a C proposal
  and move the idea to `02-workstreams/C-profiles-and-gaps/`
  instead.

Logged as D24 in `04-risks/decisions-log.md`.

## Notes

- [01-end-user-ideal-world.md](01-end-user-ideal-world.md) — external
  expectations, ideal end-user UX, and the biggest gaps between that UX
  and the current roadmap.
