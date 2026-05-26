# backend-ops-abstraction-rfc — reader's guide

Series 4 of the upstream-bound queue per
`upstream-patches/SUBMISSION-QUEUE.md` (D63).

## What's in this directory

  - `0000-cover-letter.patch.md` — draft cover letter that will
    head the `git format-patch` series once the on-branch
    A-workstream commits have been squashed into a clean
    12-patch sequence. Currently a `.md` (not `.patch`) because
    the source-side patches don't exist yet — the on-branch
    state is one ~6.6 kLoC megacommit plus follow-on refactor
    commits.
  - `SUBMISSION-NOTES.md` — operational notes per the
    `upstream-patches/README.md` directory convention. Walks
    pre-submission cleanups, the on-branch commit map (which
    on-branch commit feeds which 0001-* patch slot), maintainer
    routing, checkpatch expectations, and the post-landing
    cleanup checklist.
  - `README.md` (this file) — brief reader's guide.

`0001-*.patch` … `0012-*.patch` will appear here after the
squash audit pass described in SUBMISSION-NOTES.md.

## What Series 4 is

The upstream emission of workstream A (backend abstraction) of
the UML redesign plan. The series introduces
`struct um_backend_ops` — a typed function-pointer contract in
`arch/um/include/shared/backend.h` — and migrates the existing
`using_seccomp`-keyed branches in
`arch/um/{kernel,os-Linux}/skas/` to dispatch through the ops
table.

  - 17 ops + 4 capability flags in 5 categories (lifecycle,
    memory, scheduling, time, debug).
  - 5 HOT ops marked for inline expansion in single-backend
    builds (`vcpu_run`, `mm_region_added`, `mm_region_removed`,
    `context_switch`, `read_clock_ns`).
  - Two dispatch modes: `CONFIG_UM_BACKEND_SECCOMP_ONLY`
    (inlined, zero indirect-call cost) and
    `CONFIG_UM_BACKEND_DYNAMIC` (function-pointer dispatch).
  - One arbiter at `arch/um/kernel/backend.c::init_backend()`
    driven by the `backend=auto|seccomp|kvm|force=*` boot param.
  - Seccomp is the sole landing implementer; a KVM-v2 slot
    registers under EXPERT (real KVM-v2 lands as Series 7).
  - Contract versioned (`UM_BACKEND_CONTRACT_VERSION = 2`); new
    ops can be appended without breaking existing backends.

The series stands alone on its own merits as a cleanup of
arch/um's existing fork-and-syscall-trap dispatch path, even
without the KVM-v2 follow-on. The KVM-v2 backend is a downstream
consumer (Series 7), not a justification this series carries.

## Relationship to A-workstream memos

The A-workstream substrate (six months of sketches, design
notes, per-backend feasibility studies) lives at:

  `Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/`

In particular:

  - `notes/10-lkml-memo.md` — the precursor LKML memo from
    2026-04-17; this Series 4 cover letter is the upstream
    emission of that memo.
  - `notes/02-categories.md` — op-table granularity rationale
    (the "why 17 ops not 14 not 30" walk).
  - `notes/04-ptrace-sketch.md`, `notes/05-seccomp-sketch.md`,
    `notes/06-kvm-sketch.md` — per-backend feasibility against
    the contract.
  - `notes/07-coverage.md` — every backend-relevant call site
    in arch/um/ mapped to an ops-table op.
  - `notes/recon-audit.md` — the three deviations between the
    A-01 design and the landed A-02..A-07 shape (op renames,
    folding `page_fault` into `vcpu_run`, `mm_id *` →
    `mm_struct *`).

The decisions log at
`Documentation/virt/uml/redesign/04-risks/decisions-log.md`
records the per-decision rationale (D2, D8, D9, D10, D11, D12,
D13, D14, D15, D59 — see the cover letter's "Lineage" section).

The cover letter and SUBMISSION-NOTES in this directory are the
upstream-bound emission of that substrate; the A-workstream
memos are the internal record of how the contract was arrived
at.

## When this directory changes

  - When the squash audit produces the 12-patch sequence: add
    `0001-*.patch` … `0012-*.patch` here; convert
    `0000-cover-letter.patch.md` → `0000-cover-letter.patch`
    with filled-in diffstat.
  - When the series sends: SUBMISSION-NOTES.md status flips
    from DRAFTED → SENT (with the `git send-email` Message-Id).
  - When the series lands: move this entire directory to
    `upstream-patches/landed/backend-ops-abstraction-rfc/`.
