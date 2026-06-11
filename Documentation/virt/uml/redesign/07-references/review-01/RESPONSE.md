# Review 01: Response

Archive note: this is historical redesign review material. It is preserved as
reference context and is not current UML user documentation.

Per-item response to `README.md`, written after the reconciliation
pass. Numbering follows the review's "Prioritized recommendations"
section so a reviewer can diff directly against it.

Triage framework: each recommendation is sorted into
**real deviation** (the plan said X, implementation does Y, Y is still
on HEAD — fix) or **mid-plan artifact** (the plan *scheduled* this to
a later workstream, so "it isn't done" is not a deviation). This
framework is recorded as decision D16 in
`../../04-risks/decisions-log.md`.

## 1. Reconcile the sources of truth

**Verdict:** Real deviation. **Accepted.**

Done in this pass:

- `../../01-architecture/three-layers.md` — five blocks edited
  (args struct, ops struct, selection text, contract bullets, hook
  table) so the names, arg fields, and selection rules match the
  delivered contract (`run_userspace` / `mm_map` / `mm_unmap`,
  `(requested, force, runtime_opts)`, `*_ONLY` Kconfig-pinned vs
  `DYNAMIC` arbiter-chosen).
- `../../01-architecture/README.md` — Layer 1 diagram box
  updated.
- `../../01-architecture/data-flow.md` — five `syscall_dispatch`
  references swept to `run_userspace` across four profile walkthroughs.
- `../../02-workstreams/README.md` — one prose reference swept.
- `../../02-workstreams/A-backend-abstraction/01-ops-table.md` —
  status flipped from "design draft complete" to "landed and validated
  by A-02..A-07"; header location corrected to
  `arch/um/include/shared/backend.h` per D11.
- `arch/um/include/shared/backend.h` — `um_backend_args` comment flipped
  from "A-04 lands the boot-param parser" to a present-tense pointer at
  `uml_backend_config()` in `start_up.c`.
- `arch/um/kernel/um_arch.c` — init_backend comment flipped from "A-04
  will populate" to a description of the parsed globals the arbiter
  reads.
- `arch/um/backend/{ptrace,seccomp}/lifecycle.c` — lifecycle comments
  reclassified as "future cleanup, no workstream currently owns it"
  instead of "deferred to A-04".
- `arch/um/backend/ptrace/ptrace_backend.c` — banner collapsed; the
  "Op fill-in plan" schedule is now archaeology because all 18 ops
  are populated.

Deliberately **not** edited (historical artifacts that would
rewrite-history if touched):

- `02-ptrace-refactor.md` "Approach" numbered list — task spec;
  workflow executed against it.
- `05-validation/benchmarks.md` single `syscall_dispatch` reference —
  future-looking spec; shorthand.
- Per-slice annotations in `ptrace_backend.c` ops table (`A-02.HOT-1`
  etc.) — archaeological markers for reviewers.

Rationale for each stay/leave verdict is recorded in
`../../02-workstreams/A-backend-abstraction/notes/recon-audit.md`.

## 2. Send an RFC on workstream A now

**Verdict:** Policy/cadence call. **Accepted as direction.**

Sequencing unchanged from `notes/10-lkml-memo.md`: reconcile docs
(this pass), then split the mega-commit into a small first series,
then RFC. Reconciliation is now done; the commit split and RFC send
are separate tasks and will be owned by the project owner, not
this session.

## 3. Freeze the 18-op surface for the RFC

**Verdict:** Architectural call. **Accepted.**

No ops added, renamed, or removed in this pass. The 18-op surface
documented in `../../02-workstreams/A-backend-abstraction/01-ops-table.md`
and realized in `arch/um/include/shared/backend.h` is the RFC
surface. Per-op semantics will be questioned during LKML review;
preemptive churn is the specific anti-pattern the review flags.

## 4. Keep `init_backend()` as the early boot installation point

**Verdict:** Accepted (matches D12).

The boot-order fix is already committed. The review's caveat — that
lifecycle ops are not yet authoritative because `os_early_checks()`
still owns the real probe — is accurate and is captured in
recommendation 5 below. See D12 in
`../../04-risks/decisions-log.md`.

## 5. Be explicit that five cold ops are staged placeholders

**Verdict:** Mid-plan artifact, but docs should say so plainly.
**Accepted in doc-surface form.**

The affected ops (`probe`/`init`/`shutdown` stubs,
`read_guest_regs`/`write_guest_regs` stubs) are already documented as
staged in `Documentation/virt/uml/backend-contract.rst`. The lifecycle
code comments were edited in this pass to drop "deferred to A-04"
phrasing (which implies imminent work) in favor of "future cleanup,
no workstream currently owns this", which is the honest state.

No code change: these stubs are the plan's intended landing for A-02
through A-05, not a deviation.

## 6. Keep the unified `struct mm_id`

**Verdict:** Architectural call. **Accepted.**

No change. Split `backend_private` design is rejected for the same
reasons the review lists (accessor churn paid now for value that only
appears with KVM). Revisit triggers are recorded in the review itself
and would surface as a new decision entry if invoked.

## 7. Keep `shared/backend.h` as the shared contract header

**Verdict:** Architectural call. **Accepted.**

No change. D11 already captures the rationale: USER TUs in
`arch/um/os-Linux/` need the full struct, prototypes, dispatch macro,
and `um_backend` declaration. A thinner adapter layer would duplicate
the surface or multiply forwarding functions without solving anything.

Minor consistency fix applied in this pass:
`01-ops-table.md`'s "Backend ops header" row now points at
`arch/um/include/shared/backend.h` (with `asm/backend.h` for
kernel-only bits) instead of the old all-in-`asm/` location.

## 8. Keep `backend=auto` legacy-compatible for the first send

**Verdict:** Policy call. **Accepted.**

No change in this pass. D15 already records the decision that
`backend=auto` keeps historical behavior (seccomp probe runs only
if `seccomp=` opted in) while named requests (`backend=seccomp`)
trigger the probe themselves. The flip to "seccomp-first" default
is deferred to a separate UX change, to be raised with maintainers
as an explicit question rather than bundled with the abstraction.

## 9. Describe A-05 tests as "first-pass structural conformance"

**Verdict:** Real deviation in framing. **Accepted.**

`../../02-workstreams/A-backend-abstraction/05-contract.md`
already labels the KUnit suite as "first pass complete". No edit
needed there in this pass.

`Documentation/virt/uml/backend-contract.rst` conformance section
should be audited for any language that reads as "fully validated";
that audit is not in this pass's scope (see recon-audit.md §"Out
of scope for this pass") and is queued as a doc-polish followup
before the RFC send.

## 10. Do not publish the 42% boot-cycles delta as a strong claim

**Verdict:** Framing call. **Accepted.**

No doc edit in this pass. The perf number lives in
`notes/timings.md` and the perf capture outputs, neither of which
frame it as a headline. The cover-letter and RFC drafts should
treat it as exploratory only, per this review. A syscall-focused
microbench guest is not in-scope for workstream A; if a
publishable number is wanted, that would be a new workstream-A
followup (or a B-workstream deliverable).

## Bottom line

One pass of doc reconciliation + comment cleanup addresses the
"different sources of truth" risk the review calls out. No
architecture churn. No ops added, renamed, or removed. The
RFC-readiness bar, as the review describes it, is met after this
pass.
