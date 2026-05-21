# Upstream-bound patch series — submission queue + sequencing

**Status:** Phase VI Lift #7 deliverable (2026-04-23).
**Companion to:** `README.md` (this directory's index),
`04-risks/decisions-log.md` D63 (the sequencing decision),
`06-sequencing/post-q1-push.md` §"Phase VI — Lift #7",
each series' own `SUBMISSION-NOTES.md`.

This file lists every upstream-bound patch series the UML
redesign generated, in the **order they should be sent** to
LKML / subsystem lists, with dependencies + maintainer routing
spelled out. Per D45 the fork runs ahead of upstream; this
queue orders the eventual submission cadence without blocking
in-tree progress.

## The seven series

Ordered from "smallest, send first" to "largest, send last."
Earlier series land quickly on their own merits; later series
cite the earlier ones for context.

| # | Series | Size | Subsystem | Dependencies | Status |
|---|--------|------|-----------|--------------|--------|
| 1 | `bpf-hygiene-v1/` | 2 patches | BPF | none | **ready** — patches staged, cover letter written |
| 2 | `kmsan-arch-callback-rfc/` | 1 RFC patch | mm/kmsan | none (upstream-independent; see below) | **ready** (unblocked 2026-04-23 by D62; see §"Post-D62 update") |
| 3 | `ftrace-notrace-generic-v1/` | 1 patch | tracing/ftrace | none (generic kthread notrace) | **READY (regenerated 2026-05-14, commit `cdea7ace1893`)** — patch regen'd with `--unified=3` after the 2026-04-24 staged version tripped `git am --3way` "corrupt patch at line 63"; `git apply --check` and `git am --3way` both clean against `origin/master`. Operator action: `git send-email`. |
| 4 | `backend-ops-abstraction-rfc/` | ~12 patches | arch/um | series 3 landed (ftrace clean) | **DRAFTED 2026-05-14** — cover letter + SUBMISSION-NOTES in-tree; awaiting on-branch series rebase before patches emit. |
| 5 | `static-key-hot-paths-series/` | ~6 patches | arch/um | series 4 landed | **DRAFTED 2026-05-14** — cover letter + SUBMISSION-NOTES in-tree; awaiting Series 4 landing + on-branch B-workstream commits. |
| 6 | `kprobes-ftrace-kfence-kcsan-profiles-series/` | ~20 patches across 4 sub-series | arch/um | series 4 + 5 landed | **DRAFTED 2026-05-14** — cover letter + SUBMISSION-NOTES in-tree; awaiting Series 4 + 5 landing + on-branch C-workstream commits per sub-series. |
| 7 | `kvm-backend-series/` | ~19 patches | arch/um | series 4 + 5 landed; Phase J DONE certificate | **SQUASH AUDIT LANDED 2026-05-21 (commit `52985fb22e77`)** — `SQUASH-AUDIT-PLAN.md` lists 19 named patches with LoC budget + source commits + bisect risk + pre-flight gates per patch.  Cover letter rewritten 2026-05-14 (`aaaa3ce70027`).  Squash execution itself unblocked once the 24h soak completes naturally on the post-memo-04 kernel (HEAD `94e8d71b8f6d` or later); fallback fusion to 17 patches explicitly enumerated in the plan if reviewers push back on density.  See `kvm-backend-series/{SQUASH-AUDIT-PLAN.md, SUBMISSION-NOTES.md, 0000-cover-letter.patch.md}`. |

## Submission-order rationale

### Series 1 + 2 first: upstream-independent hygiene

`bpf-hygiene-v1/` and `kmsan-arch-callback-rfc/` are both
arch-agnostic. Neither mentions UML in the patch bodies;
both land on their own upstream merit. Sending these first
does three things:

1. Validates the `upstream-patches/` workflow (SUBMISSION-
   NOTES, checkpatch baseline, patch format). Mistakes
   here are recoverable — small series, low reviewer risk.
2. Builds author credibility with the target subsystems
   (BPF, mm/kmsan) before we ship bigger architectural
   work that asks more from them.
3. Unblocks the UML-side consumers on the fork — D62's
   KMSAN redesign already works without series 2 upstream
   (the arch callback was renamed to a shape UML can
   consume independently), but series 2 cleans up the
   upstream-side for everyone else.

### Series 3 before series 4: generic cleanup before arch RFC

`ftrace-notrace-generic-v1/` covers the `notrace` annotations
on generic `kthread()` / `smpboot_thread_fn()` that C-04
commit 3a added to arch/um locally. These are 1-line
annotations in `kernel/` that help any arch where
`-fpatchable-function-entry` instruments signal-adjacent
functions — not UML-specific. Sending these as a standalone
series gives tracing maintainers a clean upstream-only
diff before the A-workstream RFC asks them to accept the
full arch/um re-shape.

### Series 4 is the tentpole: `struct um_backend_ops` RFC

The A-workstream abstraction is the foundational upstream
change. Everything downstream (B, C, D) cites it for shape.
Cover-letter pitch: "UML backend diversity (ptrace, seccomp,
future-KVM) is an ABI surface that deserves a typed ops
table; here's the table and the ptrace + seccomp
implementations."

Size budget: ~12 patches (ops-table header + 10 ptrace/seccomp
per-op impls + Kconfig gating). 5-10 round-trip review
cycles is realistic; plan for 6-9 months wall-clock.

### Series 5 builds on series 4's ops table

B-workstream static-key gates depend on having the ops
table landed so the gate macros can be typed. Send after
series 4 to avoid a merge-conflict pile.

### Series 6 bundled by profile

C-series kprobes, ftrace, KFENCE, KCSAN each have their own
maintainer audience; bundling them as 4 sub-series with
shared cover-letter framing ("UML profile-driven feature
selection") gives reviewers the context without forcing
one person to carry all 4 maintainer threads.

### Series 7 last: KVM backend

D-workstream needs both series 4 (ops table) and the real
`run_userspace` integration (task #162, post-Phase III).
Realistic timing: 12-18 months after series 4.

## Per-series maintainer routing

Run `scripts/get_maintainer.pl` against each series to get
the current list; the notes below are from the 2026-04
tree and will drift.

### Series 1 — bpf-hygiene-v1

- **Primary list:** `bpf@vger.kernel.org`
- **Cc:** Alexei Starovoitov, Daniel Borkmann, Andrii
  Nakryiko (`MAINTAINERS` "BPF JIT for x86-64")
- **Secondary:** `netdev@vger.kernel.org` (for any x86-
  specific JIT hunks that touch `arch/x86/net/bpf_jit_*`)
- Notes: `upstream-patches/bpf-hygiene-v1/SUBMISSION-NOTES.md`

### Series 2 — kmsan-arch-callback-rfc

- **Primary list:** `linux-mm@kvack.org`
- **Cc:** Alexander Potapenko (KMSAN maintainer), Marco
  Elver (KCSAN maintainer, related patterns), Dmitry
  Vyukov (KMSAN author)
- Framing: arch callback hook only; no UML reference in
  the patch body; UML consumer stays on the fork until
  the hook lands upstream.
- Notes: `upstream-patches/kmsan-arch-callback-rfc/SUBMISSION-NOTES.md`

### Series 3 — ftrace-notrace-generic-v1 (to write)

- **Primary list:** `linux-trace-kernel@vger.kernel.org`
- **Cc:** Steven Rostedt, Masami Hiramatsu
- Framing: "`-fpatchable-function-entry` emits ftrace
  trampoline calls at signal-adjacent kernel generic
  helpers; the existing `notrace` annotation vocabulary
  already covers the right places — this series adds the
  missing 3 annotations that uncovered under full
  instrumentation coverage."

### Series 4 — backend-ops-abstraction-rfc (to write)

- **Primary list:** `linux-um@lists.infradead.org`
- **Cc:** Richard Weinberger (UML maintainer nominal),
  Johannes Berg, Benjamin Berg, Anton Ivanov, Tiwei Bie
- **Copy:** `linux-arch@vger.kernel.org` for the ops-table
  pattern feedback (other arches might adopt similar
  shapes).
- Framing: "UML has had three backend flavors coexisting
  informally; a typed ops table makes the backend surface
  explicit, lets profiles declare their dispatch model at
  compile time, and unblocks KVM as a third backend
  without forking arch/um."

### Series 5 — static-key-hot-paths (to write)

- **Primary list:** `linux-um@lists.infradead.org`
- **Cc:** same as series 4 + maintainers of any specific
  gated subsystem (trace, seccomp, KCOV)

### Series 6 — per-profile C-series (to write)

Split by profile, four sub-series:

- **6a** kprobes + rethook (C-04)
- **6b** ftrace + dynamic-ftrace + function-graph (C-05 + C-04 3a/3b)
- **6c** KFENCE + KCSAN (C-02 + C-03)
- **6d** KMSAN (C-07, after series 2 lands upstream)

Each routes to its subsystem maintainer with a
UML-specific rider cc'ing `linux-um@lists.infradead.org`.

### Series 7 — kvm-backend-series (to write)

- **Primary list:** `linux-um@lists.infradead.org` + `kvm@vger.kernel.org`
- **Cc:** Paolo Bonzini, Sean Christopherson (KVM x86),
  plus series 4 cc-list.
- Prereq: task #162 real `run_userspace` integration
  shipped + measured against seccomp baseline on at least
  two silicon classes.

## Post-D62 update (Phase V closing note)

Series 2 (`kmsan-arch-callback-rfc`) was previously blocked
on "UML-side callback consumer doesn't work yet because the
dedicated-slab scheme is broken" (D58). D62 (2026-04-23)
replaced the dedicated-slab scheme with VMALLOC-quarter-
split, which works without needing the new arch callback
hook. Series 2's upstream submission is therefore
independent of UML's forward progress — it's pure
upstream hygiene now, and UML can ship with or without it.
Keep submitting; removing the gate is a fork-progress win.

## When a series lands

Add a post-landing note to the series' `SUBMISSION-NOTES.md`
with the upstream commit ID + landing date, then move the
directory to `upstream-patches/landed/` (create if needed)
to keep the active queue visible.

## What this memo is NOT

- Not an implementation commitment. Series 3-7 are all
  "to write" — writing them is the actual upstream work,
  which the memo just sequences.
- Not a promise that series 1-2 land easily. LKML review
  cycles are 3-6 months; series 2 in particular may stall
  on maintainer availability. The queue is resumable;
  nothing in the fork depends on upstream landing.
- Not a replacement for per-series `SUBMISSION-NOTES.md`.
  This file is sequencing only; operational details stay
  per-series.

## Cross-references

- D45 (2026-04-21) — fork-first upstream policy.
- D63 (2026-04-23) — the sequencing decision this memo
  formalizes.
- D62 (2026-04-23) — KMSAN redesign that unblocked series 2
  from UML's side.
- `README.md` (this directory) — conventions for adding
  new series.
- `06-sequencing/post-q1-push.md` §"Phase VI" — parent-
  plan reference.
