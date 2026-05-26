# Series 4 (backend-ops-abstraction-rfc) — DRAFTED (2026-05-14)

## What

Landed cover letter + SUBMISSION-NOTES + reader's-guide README
under `Documentation/virt/uml/redesign/upstream-patches/
backend-ops-abstraction-rfc/`:

  - `0000-cover-letter.patch.md` — 854 lines, draft cover
    letter heading the RFC series (target: `[PATCH RFC 00/12]
    um: introduce struct um_backend_ops`). Walks the existing
    `using_seccomp` ladder concretely (`os_early_checks` +
    `userspace()` excerpts), shows the `struct um_backend_ops`
    contract (17 ops + 4 capability flags in 5 categories,
    contract version 2), and proposes the 12-patch landing
    sequence with each intermediate state buildable + bootable
    to the cpython 21/21 substrate gate.

  - `SUBMISSION-NOTES.md` — 473 lines, operational notes.
    Carries the on-branch commit map (which on-branch commit
    feeds which 0001-* slot), the squash audit checklist, the
    pre-submission cleanups (drop `Co-authored-by:`,
    michael.bommarito@gmail.com identity, no UML-specific log
    references in commit messages), the maintainer routing
    (linux-um@ primary; Richard / Johannes / Anton / Benjamin
    / Tiwei as Cc; linux-arch@ as pattern-signal), the
    checkpatch expectations per patch, and the post-landing
    cleanup checklist (move to `landed/`, update D45).

  - `README.md` — 99 lines, brief reader's guide describing the
    directory contents and the relationship to the A-workstream
    memos under `02-workstreams/A-backend-abstraction/`.

All three files pass `scripts/checkpatch.pl --no-tree -f` at
0 errors / 0 warnings.

## Why now

PLAN-2026-05-14 §6.2 row 4 had Series 4 as **to write**. The
A-workstream substrate has been validated since 2026-04-17 (six
months of design, twenty KUnit conformance tests, cpython
21/21, perf-CI ≤ 1.2 % delta under DYNAMIC and ≤ 0.5 % under
SECCOMP_ONLY) but the upstream-bound emission had been deferred
behind Series 1-3 priority on the queue.

Drafting now lets the cover letter and SUBMISSION-NOTES go
through a review pass while Phase J finishes the soak gate; the
actual `git format-patch` emission still waits on the on-branch
squash audit + Series 3 landing per the queue.

D63 lists Series 4 as "the tentpole" of the upstream queue —
everything downstream (Series 5 static-keys, Series 7 KVM-v2)
cites it. PLAN-2026-05-14 §7.3 calls out sending Series 4 early
to prime the maintainer conversation before Series 7 lands. The
drafting work now reduces the lead time once Series 3 reaches
LKML.

## What's still missing before send

  1. **Squash audit pass on the on-branch substrate.** Today
     the A-workstream lives as one ~6.6 kLoC megacommit
     (`3c2d95683123` "um: backend: introduce typed ops table
     + ptrace/seccomp backends (workstream A)") plus five
     follow-on refactor commits (R2, R5, R11, D59,
     tlb_kick_others / v2 bump). The squash audit unpicks the
     megacommit into twelve reviewable slices per D10. The
     on-branch commit map in SUBMISSION-NOTES.md §"On-branch
     commit map" shows which on-branch commit feeds which
     0001-* slot. This is the actual work-remaining bottleneck.

  2. **Cherry-pick onto a submission branch.**
     `backend-ops-abstraction-rfc-v1-submit` off mainline
     `master` (not `umlctl-deploy`), with `Co-authored-by:`
     trailers dropped and UML-specific log references removed
     from commit messages.

  3. **Build verification per intermediate state.** Each of
     the 12 patches must:
       (a) build clean on `make ARCH=um O=/tmp/uml-N defconfig`
       (b) boot under qemu-x86 wrapping ARCH=um vmlinux
       (c) pass cpython-parity 21/21
     This is the actual bisection-safety verification; failing
     intermediate states must be fixed in the squash, not
     papered over.

  4. **`scripts/get_maintainer.pl` rerun** right before send to
     pick up rotation since 2026-05.

  5. **Series 3 lands first** (operational hygiene, not a hard
     build dependency).

## The 3 hardest open questions flagged in the cover letter

(For the diary record + future-session catchup.)

  1. **Op granularity — 17 ops too few or too many?** Cover
     letter §"Anticipated review questions" (a). Maintainers
     may want `mm_create`/`mm_destroy` collapsed into
     `init`/`shutdown` (rejected per D8 alternative 2 because
     mm lifecycle is per-mm and outlives the once-per-boot
     backend lifecycle); or `set_timer`'s mode tag split into
     separate enable/disable ops (rejected per
     02-categories.md because cold-op cost is noise either
     way). Reviewers also may push back on the
     `mm_region_added`/`mm_region_removed`/
     `mm_region_protected` triple as "shouldn't this be one op
     with a mode enum?" — same argument as set_timer; happy to
     collapse if reviewers prefer.

  2. **Capability flags vs ops for "what does the host-side
     differ on."** Cover letter §"Anticipated review
     questions" (b). The four flags (`uses_stub_reaper`,
     `has_syscall_stub_fd_map`, `stub_syscall_uses_futex`,
     `stub_child_runs_seccomp`) replace `using_seccomp` checks
     at specific os-Linux/ call sites. An alternative is to
     add an op per call site (no-op for flags it doesn't care
     about). Picked flags because the consuming sites are
     static and the cost of a function-pointer call there is
     noise vs the flag check; reviewers may prefer the
     ops-only shape. Mechanical to swap.

  3. **Kconfig default — `SECCOMP_ONLY` vs `DYNAMIC`.** Cover
     letter §"Anticipated review questions" (c). Today
     `SECCOMP_ONLY` is the redesign-branch default because
     it's a closer match to mainline shape (one resident
     backend, zero indirect-call cost). A `DYNAMIC` default
     would let users override at the boot prompt with
     `backend=...` at the cost of one indirect call per HOT
     op. Current default minimises surprise; `DYNAMIC` is
     opt-in via Kconfig. Open to reversal if maintainers
     prefer the runtime-flexible default.

A fourth open question — the header split between
`<asm/backend.h>` and `<shared/backend.h>` per D11 — is also
flagged in the cover letter but is less likely to surprise
reviewers (the USER-TU vs kernel-TU split is well-known UML
machinery).

## On-branch commit → 0001-00NN slot map (the squash audit input)

| Slot     | Subject (target)                                                       | On-branch commits                                                       |
|----------|------------------------------------------------------------------------|-------------------------------------------------------------------------|
| Patch 1  | um: backend: contract header + dispatch macro                          | `3c2d95683123` (header hunks) + `764eac6d79de` (R2 `mm_id *`→`mm_struct *`) |
| Patch 2  | um: backend: arbiter scaffold (init_backend stub)                      | `3c2d95683123` (backend.c + um_arch.c hunks)                            |
| Patch 3  | um: backend: arch/um/backend/seccomp/ + ops struct populated            | `3c2d95683123` (arch/um/backend/seccomp/ hunks)                         |
| Patch 4  | um: backend: route the trap loop through vcpu_run                       | `3c2d95683123` (os-Linux/skas/process.c hunks) + `764eac6d79de` (R2)    |
| Patch 5  | um: backend: route mm lifecycle + region changes through ops            | `3c2d95683123` (skas/mmu.c + tlb.c hunks) + `d3b2035f4079` (R5)         |
| Patch 6  | um: backend: route scheduling, time, debug through ops                  | `3c2d95683123` (kernel/smp.c + time.c + exec.c + process.c hunks)       |
| Patch 7  | um: backend: capability flags replace using_seccomp side channel         | `24699ccc795e` (Phase II Lift #4d+) + `d6c9618f10c5` (D59 catalog)      |
| Patch 8  | um: backend: boot-param parser                                          | `3c2d95683123` (start_up.c hunks)                                       |
| Patch 9  | um: backend: Kconfig — choice block + matrix coverage                  | `3c2d95683123` (Kconfig + Kbuild hunks)                                 |
| Patch 10 | um: backend: contract KUnit suite                                       | `3c2d95683123` (backend/contract/test_ops.c hunks)                      |
| Patch 11 | Documentation: backend-contract.rst + backends.rst                     | `3c2d95683123` (Documentation/virt/uml hunks) + `b078a9bcf13c` (Finding #6) |
| Patch 12 | MAINTAINERS: USER MODE LINUX entry update                              | `3c2d95683123` (MAINTAINERS hunk)                                       |

Two on-branch commits do NOT feed Series 4:

  - `06c88545ae2c` "um: refactor 11 — remove ptrace backend" —
    redesign-branch decision; the upstream emission keeps
    ptrace's slot in the contract (Series 4 abstracts over
    seccomp + future-backend slot; ptrace removal is the fork's
    business).
  - `77cc1821c595` "um: backend: contract v2 — add nullable
    tlb_kick_others op" — folded into Patch 1's contract header
    directly (contract version 2 lands with the surface). The
    on-branch v1→v2 bump is internal history.

## Total LoC (this diary entry + new directory)

```
README.md                          99 lines
SUBMISSION-NOTES.md               473 lines
0000-cover-letter.patch.md        854 lines
07-series-4-draft.md (this)       ~160 lines
SUBMISSION-QUEUE.md               +0 (in-place row 4 edit)
                                ─────
                                 ~1586 lines new content
```

(SUBMISSION-QUEUE.md change is a single row's status flip from
`**to write**` to `**DRAFTED 2026-05-14**`; counts as 0 LoC
delta beyond the inline edit.)

## Diary cross-references

This entry is workstream-D's logbook for plan-2026-05-14
execution. Series 4 is A-workstream content; the execution
diary lives here because the entire PLAN-2026-05-14 execution
trail is workstream-D's. The substantive Series 4 substrate is
under `02-workstreams/A-backend-abstraction/`.

Companion entries in this diary:

  - `00-kickoff.md` — execution kickoff.
  - `03-series-2-3-recheck.md` — Series 2 + 3 readiness check.
  - `06-series7-cover-letter-refresh.md` — Series 7 cover
    letter refresh.
  - `07-series-4-draft.md` (this entry) — Series 4 drafting
    pass.

The next diary entry will record the squash audit / submission
branch creation when it happens; that's the gating work for
"DRAFTED → READY" transition.
