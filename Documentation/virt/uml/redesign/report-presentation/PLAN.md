# Report / Presentation Plan

## Historical Status

This directory is an archival May 2026 publishing workspace. The report,
slides, comprehensive report, generated PDFs, and CSV data remain useful as
historical material and structure references, but they are not current status
documents for UML v2.

Use the live status files for completion claims:

- `../STATUS.md`
- `../06-sequencing/2026-06-10-next-functionality-inventory.md`
- `../06-sequencing/2026-06-10-next-full-functionality-integration-plan.md`

Any future report refresh must first freeze a new `next` cutoff commit,
regenerate the data tables from the live inventory, and rebuild the report and
deck from that cutoff. Until that happens, do not treat old branch names,
commit IDs, validation counts, snapshot/record-replay claims, or upstream queue
status in this directory as current truth.

## Purpose

Create a small publishing workspace under `Documentation/virt/uml/redesign/`
for two related deliverables built from the same source material:

- a concise Beamer deck for the story and visual summary
- a longer LaTeX report for detailed evidence, tables, and appendix material

The goal is to keep `vision`, `landed status`, and `build/validation evidence`
separate, but tied together through one shared structure.

## Recommended shape

Use one shared source base and produce two outputs:

- `slides/`: short Beamer presentation, roughly 12-15 main slides
- `report/`: longer written report with appendix material

The key design choice is that both artifacts should read from the same
normalized status/evidence tables instead of duplicating data by hand.

## Source material to mine

Primary narrative sources:

- `00-vision.md`
- `README.md`
- `02-workstreams/README.md`
- `02-workstreams/C-profiles-and-gaps/README.md`
- `06-sequencing/milestones.md`
- `06-sequencing/critical-path.md`

Primary evidence sources:

- `02-workstreams/A-backend-abstraction/notes/timings.md`
- `02-workstreams/B-static-key-hot-paths/notes/timings.md`
- `02-workstreams/C-profiles-and-gaps/notes/timings.md`
- `02-workstreams/C-profiles-and-gaps/notes/profile-matrix.md`
- `scripts/Q1-README.md`
- `scripts/uml-boot-matrix.sh`
- `scripts/uml-perf.sh`
- `scripts/uml-perf-compare.sh`
- `tools/testing/selftests/um/`

## Publishing structure

This folder is scaffolded to support a shared publishing workflow:

```text
report-presentation/
├── PLAN.md
├── shared/            common TeX macros, colors, status labels
├── data/              normalized CSV/JSON extracted from docs and scripts
├── figures/           tikz/pgfplots diagrams and reusable visual assets
├── report/
│   └── sections/      report section files
└── slides/
    └── sections/      Beamer section files
```

## Narrative split

The strongest presentation/report structure is:

1. Vision
2. Architecture and profiles
3. What has landed
4. How it is validated
5. Build / test / perf patterns
6. What remains open

That keeps the front half readable for a broad audience and moves the dense
build details into the second half or appendix/backup slides.

## Report outline

Suggested report shape:

1. Executive summary
2. Vision and target outcomes
3. Architecture in one tree
4. Status by workstream
5. Profile matrix and delivered artifacts
6. Build / validation pipelines
7. Measured evidence
8. Risks, gaps, and deferred work
9. Appendix: detailed tables, script references, milestone mapping

## Deck outline

Suggested Beamer deck shape:

1. Why redesign UML now
2. Target outcomes
3. Three-layer architecture
4. Profile story
5. Workstreams and critical path
6. What has landed so far
7. Validation pipeline
8. Build-pattern evidence
9. Remaining gaps and next milestones
10. Closing summary

Keep detailed task lists, long tables, and script-specific material in backup
slides rather than the main path.

## Best visuals

Prefer generated figures over screenshots:

- three-layer architecture diagram
- workstream status heatmap
- profile matrix heatmap
- profile build-time vs binary-size chart
- validation pipeline diagram
- backend boot-matrix summary
- perf-ratio summary rather than raw absolute-cycle-only charts
- milestone / critical-path timeline

Do not paste raw shell logs into the main deck. Convert them into tables,
timelines, or charts.

## Data normalization plan

Before writing the deck/report proper:

1. Freeze a reporting cutoff commit.
2. Extract status tables and timing tables into `data/`.
3. Normalize recurring labels such as `landed`, `partial`, `planned`,
   `deferred`, `blocked`.
4. Generate charts/tables from those normalized files in both report and deck.

This avoids drift between the written report and the slide deck.

## Reporting rules

- Every high-level claim should map to one of: landed, planned, or deferred.
- Vision claims should not be phrased as already-delivered behavior unless
  there is matching evidence in tree.
- Use actual build/test/perf automation as the evidence backbone:
  profile checks, boot matrix, Q1, selftests, perf capture/compare.
- Favor backend ratios and matrix results over isolated one-off anecdotes.

## Execution sequence

Suggested order of work:

1. Freeze the commit/date to report against.
2. Build the normalized `data/` tables.
3. Draft the long report first.
4. Compress the report into the Beamer deck.
5. Move dense tables and script details into appendices / backup slides.
6. Review for consistency between vision, landed state, and evidence.

## Immediate next step

The next concrete task after this scaffold is to populate `data/` with a first
pass of:

- workstream status
- profile matrix
- timing/build tables
- validation lanes
- perf summary tables

Once those are stable, `shared/`, `figures/`, `report/`, and `slides/` can be
filled in without duplicating logic or numbers.
