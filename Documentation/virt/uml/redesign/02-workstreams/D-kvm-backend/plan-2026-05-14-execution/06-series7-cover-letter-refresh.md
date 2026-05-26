# Series 7 cover-letter + SUBMISSION-NOTES refresh (2026-05-14)

## What

Replaced two stale in-tree drafts under
`Documentation/virt/uml/redesign/upstream-patches/kvm-backend-series/`
to reflect the current v2-redesign state:

  - `0000-cover-letter.patch.md` — was 414 lines dated
    2026-04-27 describing the Stage-A per-task vCPU + shadow-PT
    architecture (commit `7f94922a356f`). Replaced in place
    with a 900-line v2-shape draft.
  - `SUBMISSION-NOTES.md` — was 315 lines describing the
    matching 15-patch Stage-A series. Replaced in place with
    a 722-line refresh.

Net delta: +1739 / -729 lines.

## Why

The 2026-04-27 drafts described an architecture that is no
longer the proposed upstream shape. v1 was archived at tag
`kvm-v1-archive-20260428` under `arch/um/backend/kvm-v1-archive/`
(gated on `BROKEN`); v2 was built from scratch under
`arch/um/backend/kvm-v2/` per memo 26. Sending those drafts
to LKML now would describe code that doesn't exist.

Since the 2026-04-27 capture, the project has also closed
14 SMP-T## fix cycles (D106..D122 in decisions-log), revived
the systrap gadget with full x86_64 ABI preservation (D113),
enabled AVX/XSAVE (D121 SMP-T57 Phase A), and run the first
2 h Phase J daemon soak with kvm-v2 200 / 200 = 100 %
(D122). The cover letter pitch and the SUBMISSION-NOTES patch
ordering both needed a full refresh to capture this.

## What's in the refresh

### Cover letter (900 lines)

  - **Pre-letter preamble** explaining that this is a fresh
    draft current as of 2026-05-14, gated on Phase J DONE.
    The preamble walks the reader through the v1 → v2
    transition + the 14 SMP-T## closures + the Phase J pilot
    + 2 h soak numbers.
  - **Subject line** updated to `[PATCH RFC 00/19] um: add
    KVM backend (v2) with TDP, per-CPU vCPU pool, and
    in-guest systrap gadget`. RFC v2 (the draft itself; the
    "v2 redesign" is distinct from "the LKML cycle number").
  - **TL;DR perf section** — keeps the gadget hot-path
    pitch (~95 cyc/getpid on Zen 4) but replaces the
    Stage-A fleet bench numbers with the current Zen 4
    7840HS measurements: bench-py 4.00× faster than seccomp,
    bench-micro getpid ~1050× faster, perf-py-startup ratio
    1.10-1.17 vs the 1.20 ceiling. mt-mini SMP T=8 N=400 =
    400/400. cpython-parity 21/21 under UP and SMP.
    threaded-fork-malloc 24 000 forks ⇒ 0 CHILD_FAIL.
    Phase J pilot 240/240 + Phase J 2 h soak 200/200.
  - **"Why a third backend"** — preserved the strategic
    pitch (ptrace / seccomp / KVM tradeoffs) and added the
    time-machine alignment argument (KVM's vCPU state model
    is snapshot-friendly; v1's shadow-PT made snapshot
    intractable, v2 makes it tractable).
  - **"The v2 architecture"** — three load-bearing
    structural choices (per-CPU vCPU pool, TDP / per-region
    memslots, kernel-half-only trampoline at PML4[448])
    plus the gadget as #4. Each gets a paragraph of
    rationale with explicit reference to memo 26 phases +
    decisions-log entries.
  - **"Security posture"** — preserved most of the original
    posture (gadget runs in ring-0 inside guest CR3, same
    trust model as any KVM guest; page-table mapping
    P-only or P|RW with no US on state pages; class-D
    classifier traps dangerous syscalls at -EPERM). Added
    the per-vCPU FPU-dirty epoch flag explanation (T55)
    and the cross-task SREGS / `WARN_ON_ONCE`
    (T33 + T47) for the SMP discussion.
  - **"Testing methodology + results"** — full results
    section with cpython-parity 21/21 + substrate gate
    25+3+3 + SMP stress + perf-py-startup + bench-py +
    bench-micro + AVX/XSAVE matrix + Phase J pilot + 2 h
    soak. All numbers cited with their decision-log entry
    or measurement-memo pointer.
  - **"Known limitations"** — AVX-512 deferred to T57
    Phase B; SMP-T55 stretch (+50% UP-hop bisect) deferred;
    per-mm host worker model deferred; x86_64 only; KSM /
    KSAN guest-side deferred; snapshot/record-replay TUs
    stubs at v2.
  - **"Closed-since-2026-04-27 fix-cycle catalogue"** —
    D106..D122 table. Reviewer-credibility list of issues
    we fixed since the prior draft.
  - **"MAINTAINERS routing"** — updated to v2-shape paths
    (`arch/um/backend/kvm-v2/`).
  - **"Dependencies on Series 1-6"** — explicit statement
    of what blocks Series 7 (Series 4 landed upstream) and
    what doesn't (Series 1, 2, 3, 5). Series 6 gates the
    other way.
  - **"Outstanding review questions"** — refreshed list:
    (a) per-CPU vs per-task vCPU, (b) -EPERM vs -ENOSYS
    for class-D, (c) per-mm host worker deferral, (d) gadget
    Kconfig default-y vs n. Each with the decision-log
    pointer the operator can cite.
  - **"Reproducing the numbers"** — build recipe with the
    Phase J daemon driver invocation. Host prerequisites
    (KVM_CAP_SYNC_REGS, KVM_CAP_SET_GUEST_DEBUG, x86-64,
    fleet bench host coverage).
  - **"Changelog"** — RFC v2 (this draft, full rewrite),
    v1 (2026-04-27 initial Stage-A draft).
  - **Diffstat placeholder** — 19 files in `arch/um/backend/
    kvm-v2/` + `Documentation/virt/uml/backends.rst` +
    `MAINTAINERS`. Real numbers will fill in post-squash.

### SUBMISSION-NOTES (722 lines)

  - **Status** — SCOPED but GATED on Phase J DONE. Cover
    letter draft is refreshed; patches not yet rebased.
    Dependent on Series 4.
  - **Authoring branch** — `umlctl-deploy`. ~846 total
    commits since master; ~105 commits touching
    `arch/um/backend/kvm-v2/`; ~70 more in adjacent paths.
    Squash audit produces 17-19 upstream-shape patches.
  - **Companion docs** — refreshed from Stage-A list
    (memos 09/10/11, decisions D66..D86) to v2 list (memos
    22/23/24/25/26 + state-audit subdirectory + measurements +
    Phase J pilot + 2 h soak diary + decisions D106..D122).
  - **Planned patch ordering (17-19 patches)** — refreshed
    from the Stage-A 15-patch list to the v2 list. Compact
    bullets describing each patch's scope + the memo 26
    phase + the origin commit. Ordered as:
    1-3 Kconfig + lifecycle + CPUID seed (Phase A).
    4-6 per-CPU vCPU pool + TDP memslots + dispatch loop
        with migrate_disable + cross-task SREGS (Phases B+C
        + SMP-T13/T33/T47 folded).
    7-9 lazy CPUID + LSTAR trampoline + IO-port trap +
        kernel-half PML4[448] + physmem identity memslot
        (Phase D).
    10-12 IDT/IST/TSS + exception handlers + signal +
        EINTR + PF-stub user-RAX recovery (Phases E+F +
        SMP-T41).
    13-14 SMP per-CPU IDT/IST/TSS + cross-vCPU TLB kick
        (Phase G + SMP-T36/T37).
    15  per-vCPU FPU-dirty epoch flag (Phase H + SMP-T55).
    16-17 AVX/XSAVE enable + lazy first-dispatch arming
        (SMP-T57 Phase A).
    18  systrap gadget revival (D113 + D114).
    19  Documentation + MAINTAINERS (Phase I.3).
  - **Routing** — refreshed cc-list.
  - **Hard prerequisites** — 5 items including Series 4
    landed + Phase J DONE + squash audit + fleet bench
    re-capture + upstream audit on a fresh branch.
  - **Pre-submission cleanups** — same shape as Series 1's
    SUBMISSION-NOTES: trailer hygiene (drop
    `Co-authored-by:`), format-patch output naming,
    build verification recipe (am the patches, build each
    commit, run cpython-parity, run mt-mini soak gate at
    tip), checkpatch discipline.
  - **Anticipated review questions** — 8 questions with
    decisions-log pointers. Q1 = per-CPU vs per-task,
    Q2 = -EPERM vs -ENOSYS for class-D, Q3 = per-mm worker
    deferral, Q4 = CPUID curation rationale, Q5 = why
    PML4[448], Q6 = SMP-T55 stretch deferral, Q7 = AVX-512
    deferral (T57 Phase B), Q8 = why a gadget. Plus D63
    queue-ordering rationale and D70 gadget go/no-go.
  - **Risk register** — 5 entries:
    - review iteration timeline (2-6 months from RFC v2
      to merge for a series this size);
    - linux-um pushback on per-CPU vCPU pool, with
      fallback plan if pushback is hard
      (`CONFIG_UM_BACKEND_KVM_V2_VCPU_MODEL` Kconfig);
    - KVM maintainer pushback on CPUID curation as
      overly restrictive;
    - per-vCPU FPU-dirty epoch flag pattern is
      unfamiliar to most reviewers;
    - in-guest gadget reviewed as a security surface
      (state pages, swapgs-toggled %gs:disp32).
  - **Post-send resolution path** — `v<N>/` sibling dirs,
    landed → `upstream-patches/landed/`.
  - **Session log pointer** — 2026-03 → 2026-05-16
    timeline with decision-log refs.

## What changed (delta against the 2026-04-27 drafts)

### Cover letter

| Section | 2026-04-27 (Stage A)        | 2026-05-14 (v2 redesign)    |
|---------|------------------------------|------------------------------|
| Subject | `[PATCH RFC 00/15] um: add KVM backend with in-guest systrap gadget` | `[PATCH RFC 00/19] um: add KVM backend (v2) with TDP, per-CPU vCPU pool, and in-guest systrap gadget` |
| Architecture | per-task vCPU + shadow PT     | per-CPU vCPU pool + TDP + kernel-half PML4[448] |
| Patch count | 15                          | 19 (target; may compress to 17-18 post-squash) |
| Perf headline | Xeon W-2123 (Skylake-SP) post-round-6-audit numbers (53k/43k/163k/97 cyc) | Zen 4 7840HS post-Phase-A (109k cyc seccomp vs 95 cyc gadget, ~1050× speedup) |
| Fleet bench | s0-s7 spanning Skylake / Kaby / Alder Lake P+E / Zen 4 | Same fleet, but cited as "to be re-captured on a 24-h-validated host post-Phase-J-DONE" |
| Known limitations | Bootstrap page RW; eager kvm_touch_all_user_vmas; shadow-PT TLB flush; x86_64 only | AVX-512 (T57 Phase B); SMP-T55 stretch +50% UP-hop; per-mm worker deferral; x86_64 only; snapshot stubs |
| Audit-closed list | A1/A2/A4/F1..F10/G1..G8 (D70..D95) | D106..D122 (May 2026 SMP-T## catalogue + gadget revival + T57 Phase A) |
| Outstanding review questions | (a) MSR_KERNEL_GS_BASE as channel; (b) shadow-PT invalidation mechanism; (c) -EPERM vs -ENOSYS | (a) per-CPU vs per-task; (b) -EPERM vs -ENOSYS; (c) per-mm worker deferral; (d) gadget Kconfig default |

### SUBMISSION-NOTES

| Section | 2026-04-27 | 2026-05-14 |
|---------|-----------|-----------|
| Branch | `uml-redesign-plan`, 69 commits | `umlctl-deploy`, ~846 commits total / ~105 kvm-v2 |
| Status | "SCOPED, dep on Series 4" | "SCOPED, dep on Series 4 + GATED on Phase J DONE" |
| Patch ordering | 15 patches around shadow PT + bootstrap page + LSTAR gadget | 19 patches around per-CPU vCPU pool + TDP + kernel-half PML4[448] + revived gadget |
| Companion docs | memos 06, 09, 10, 11 + D66..D86 | memos 22, 23, 24, 25, 26 + state-audit/ + measurements.md + phase-J memos + D106..D122 |
| Hard prerequisites | Series 4 + squash pass + G8 fleet numbers + audit round | Same 4 + Phase J DONE certificate |
| Risk register | (implicit; mostly in companion sections) | 5 explicit risk entries with mitigations |
| Anticipated review Qs | (not in 2026-04-27 draft) | 8 questions + decisions-log pointers |

## Validation

  - `./scripts/checkpatch.pl --no-tree -f
    Documentation/virt/uml/redesign/upstream-patches/
    kvm-backend-series/0000-cover-letter.patch.md`:
    `0 errors, 0 warnings, 900 lines checked`.
  - `./scripts/checkpatch.pl --no-tree -f
    Documentation/virt/uml/redesign/upstream-patches/
    kvm-backend-series/SUBMISSION-NOTES.md`:
    `0 errors, 0 warnings, 722 lines checked`.

## What's still operator-time work

This refresh updates the **drafts** in-tree. The actual
patch-emission work remains gated on Phase J DONE:

  1. Phase J 24 h continuous soak (PLAN-2026-05-14 §3
     definition of DONE).
  2. Squash audit on a clean topic branch off `master`.
  3. Re-capture the fleet bench numbers on a
     24-h-validated host.
  4. Run `scripts/get_maintainer.pl` against the rebased
     series.
  5. Strip `Co-authored-by:` trailers + verify Author /
     Signed-off-by both read michael.bommarito@gmail.com.
  6. Generate the format-patch output at `/tmp/kvm-v2-
     series-emit/` and run checkpatch on each.
  7. Upstream audit round (external prompt review) before
     `git send-email`.

None of those steps were attempted in this commit per the
"DO NOT modify the patch series itself" constraint.

## References

  - `Documentation/virt/uml/redesign/upstream-patches/
    kvm-backend-series/0000-cover-letter.patch.md`
  - `Documentation/virt/uml/redesign/upstream-patches/
    kvm-backend-series/SUBMISSION-NOTES.md`
  - `Documentation/virt/uml/redesign/02-workstreams/
    D-kvm-backend/26-v2-implementation-plan.md`
  - `Documentation/virt/uml/redesign/06-sequencing/
    PLAN-2026-05-14.md`
  - `Documentation/virt/uml/redesign/04-risks/
    decisions-log.md` §D106..D122
  - `Documentation/virt/uml/redesign/STATUS.md`
