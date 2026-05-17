# backend-ops-abstraction-rfc — submission-readiness notes

## Status (2026-05-14)

**DRAFTED.** Cover letter + this notes file in-tree;
`0001-*.patch` … `0012-*.patch` **not yet emitted**. The on-branch
substrate exists as one ~6.6 kLoC megacommit
(`3c2d95683123` "um: backend: introduce typed ops table +
ptrace/seccomp backends (workstream A)") plus five follow-on
refactor commits. A structured squash + rebase pass is the
gating prerequisite before `git format-patch` can produce the
12-patch reviewable sequence the cover letter advertises.

Transition: **TO WRITE → DRAFTED 2026-05-14 → (after squash audit)
READY → (after Series 3 lands) SENT → LANDED.**

This is Series 4 of the seven-series upstream queue per
`upstream-patches/SUBMISSION-QUEUE.md` (D63). Series 1-3 are
upstream-independent hygiene that go out first; Series 4 is the
A-workstream tentpole.

## Queue position

4th, after Series 1 (bpf-hygiene-v1, READY), Series 2
(kmsan-arch-callback-rfc, READY), Series 3
(ftrace-notrace-generic-v1, READY) land. Per D63 the queue is
**dependency-ordered, not strictly time-ordered** — Series 1+2+3
can be sent in any order on their own merits; Series 4 must wait
until Series 3's `notrace` annotations on `kthread()` /
`smpboot_thread_fn()` land so that the conformance suite under
Series 4 reads cleanly under
`-fpatchable-function-entry` instrumentation.

Strictly speaking Series 4 compiles and conformance-passes
without Series 3 (the `notrace` annotations affect tracing
correctness, not ops-table semantics); the dependency is
operational hygiene, not a hard build-time block. If Series 3
stalls in review past Series 4's emission window, Series 4 may
emit ahead, with a note in the cover letter that Series 3 is
recommended-but-not-required.

## Motivating workstream

Workstream A (backend abstraction) of the UML redesign plan:

  `Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/`

The workstream produced `struct um_backend_ops` — the ops table
contract — and two implementers (ptrace + seccomp at A-02/A-03;
ptrace removed post-A in memo 25 R11, so only seccomp survives
upstream-bound) plus an arbiter, conformance suite, Kconfig
choice block, perf-CI harness, and documentation. The seven
on-branch tasks (A-01..A-07) all completed 2026-04-17/18 and
were validated by A-02..A-07 closing without any change to the
contract surface.

Workstream A's README:

  `02-workstreams/A-backend-abstraction/README.md`

Per-task memos under the same directory; in particular the
LKML design memo at `notes/10-lkml-memo.md` is the precursor
this RFC publishes upstream-bound.

## Pre-submission cleanups — what needs to happen before format-patch

The on-branch state today is one foundational megacommit + five
refactor commits. None of those are in a shape we can
`git format-patch` and send to LKML. The squash audit produces
the 12-patch sequence the cover letter advertises.

### On-branch commit map (squash audit input)

The following on-branch commits feed Series 4 patches:

| Slot     | Patch subject (target)                                                | On-branch commit(s)                                                                                          | Notes                                                                                                                  |
|----------|-----------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------|
| Patch 1  | um: backend: contract header + dispatch macro                          | `3c2d95683123` (hunks for `<asm/backend.h>` + `<shared/backend.h>`)                                          | Pure new files; extract from the megacommit. Apply R2's `mm_id *`→`mm_struct *` change inline.                          |
| Patch 2  | um: backend: arbiter scaffold (init_backend stub)                      | `3c2d95683123` (hunks for `arch/um/kernel/backend.c` + `um_arch.c`)                                          | Stub `init_backend()` panics; will be functional after patches 3+8.                                                    |
| Patch 3  | um: backend: arch/um/backend/seccomp/ + ops struct populated           | `3c2d95683123` (hunks for `arch/um/backend/seccomp/*.{c,h}` + `Makefile`)                                    | Lift seccomp entry points into the per-category files; `seccomp_backend.c` carries the ops struct initialiser.         |
| Patch 4  | um: backend: route the trap loop through `vcpu_run`                    | `3c2d95683123` (hunks for `os-Linux/skas/process.c::userspace`); `764eac6d79de` (R2 op rename)               | The hottest call-site dispatch; renames `run_userspace`→`vcpu_run` per R2.                                             |
| Patch 5  | um: backend: route mm lifecycle + region changes through the ops table | `3c2d95683123` (hunks for `kernel/skas/mmu.c` + `kernel/tlb.c`); `d3b2035f4079` (R5 `struct um_memory_region`)| R5's region-shape collapse from `mm_map`/`mm_unmap` to region-added/removed/protected applies here.                    |
| Patch 6  | um: backend: route scheduling, time, debug through the ops table        | `3c2d95683123` (hunks for `kernel/{smp,time,exec,process}.c` + `os-Linux/main.c`)                            | Mechanical migration of the remaining `using_seccomp` branches.                                                        |
| Patch 7  | um: backend: capability flags replace `using_seccomp` side channel      | `24699ccc795e` (Phase II Lift #4d+, D59); `d6c9618f10c5` (D59 cataloguing)                                   | Removes the public `using_seccomp` extern; adds the 4 flag fields to the ops struct.                                  |
| Patch 8  | um: backend: boot-param parser (`backend=…`)                            | `3c2d95683123` (hunks for `os-Linux/start_up.c::uml_backend_config`)                                         | The legacy `seccomp=` alias stays one release per D15.                                                                  |
| Patch 9  | um: backend: Kconfig — choice block + matrix coverage                  | `3c2d95683123` (hunks for `arch/um/Kconfig` + `Kbuild`)                                                      | Default `SECCOMP_ONLY` matches mainline shape; `DYNAMIC` is the slot KVM-v2 plugs into.                                |
| Patch 10 | um: backend: contract KUnit suite                                      | `3c2d95683123` (hunks for `arch/um/backend/contract/test_ops.c`)                                             | 20 tests gated on `CONFIG_UM_BACKEND_CONTRACT_TEST=y`.                                                                  |
| Patch 11 | Documentation: backend-contract.rst + backends.rst                     | `3c2d95683123` (hunks for `Documentation/virt/uml/{backend-contract,backends}.rst` + index.rst)              | One-shot doc landing; reviewers want the contract spec landing with the contract.                                      |
| Patch 12 | MAINTAINERS: USER MODE LINUX entry update                              | `3c2d95683123` (MAINTAINERS hunks) + future-bind for `arch/um/backend/`                                      | Trivial; final patch keeps the megacommit's MAINTAINERS noise out of the per-substrate patches.                        |

Two on-branch commits do **not** feed Series 4:

  - `06c88545ae2c` "um: refactor 11 — remove ptrace backend
    (memo 25 R11 Option B)" — the upstream emission keeps ptrace
    in the contract surface (Series 4 abstracts over seccomp +
    a slot for future backends); ptrace removal is a workstream
    decision that doesn't need to go upstream. The ptrace impl
    files (`arch/um/backend/ptrace/*`) are emitted as part of
    Patch 3's scaffolding even though they live in the
    `kvm-v1-archive-20260428` tag on the redesign branch.
  - `77cc1821c595` "um: backend: contract v2 — add nullable
    tlb_kick_others op" — the tlb_kick_others op is added in
    Series 4 patch 1 directly (contract version 2 in the
    landing header); the on-branch v1→v2 bump is internal
    history that doesn't help the upstream reader.

The squash audit produces twelve `.patch` files where the commit
messages read as upstream-shape patches, not as
redesign-walkthrough commits. Each intermediate state MUST build
and boot to the cpython parity gate (21/21 stdlib modules) on
`tools/testing/selftests/um/cpython-parity/cpython-parity.sh`.
Patches that break the build are merge-conflict bugs in the
emission, not legitimate intermediate states.

### Sequencing checklist before format-patch

Per `bpf-hygiene-v1/SUBMISSION-NOTES.md` and
`ftrace-notrace-generic-v1/SUBMISSION-NOTES.md` precedent, the
following must be done before `git format-patch`:

  1. **Submission branch.** Create `backend-ops-abstraction-rfc-v1-submit`
     off mainline `master` (not `umlctl-deploy`). Cherry-pick the
     12 squash-audit outputs onto it in order.

  2. **Author + signoff.** `Author:` and `Signed-off-by:` on every
     patch read `Michael Bommarito <michael.bommarito@gmail.com>`
     (per per-user memory: never bommaritollc.com on kernel
     work). The redesign branch carried `Co-authored-by: Claude
     Opus 4.7 …` trailers as in-fork convention; per
     `ftrace-notrace-generic-v1/SUBMISSION-NOTES.md` precedent
     these are **dropped** for the upstream emission. AI assistance
     is documented in `Documentation/process/coding-assistants.rst`
     and does not require a trailer; LKML convention is
     `Signed-off-by:` only unless there is an explicit human
     co-author to credit.

  3. **No UML-specific log references in commit messages.** The
     commit-message bodies must read as upstream-shape patches.
     References to decisions-log entries (D8, D11, etc.) stay
     in this SUBMISSION-NOTES.md and the cover letter; they do
     NOT go in the per-patch commit messages, per the rule in
     `upstream-patches/README.md`.

  4. **Checkpatch on each patch + on the cover letter.** Target:
     0 errors, 0 warnings on each `.patch` file. Code-snippet
     line-length warnings in commit messages (per
     `bpf-hygiene-v1/SUBMISSION-NOTES.md`) are acceptable false
     positives; structural warnings are not.

  5. **Build verification per intermediate state.** Each of the
     12 patches must:
       (a) build clean on `make ARCH=um O=/tmp/uml-N defconfig`
       (b) boot under qemu-x86 wrapping the UML guest
       (c) pass the cpython-parity gate on the post-patch state
     Failing intermediate states must be fixed in the squash, not
     papered over; per kernel commit-bisect discipline every
     commit must work.

  6. **`scripts/get_maintainer.pl` run on each patch + on the cover
     letter** right before `git send-email`. The 2026-05-14 list
     under "Maintainer routing" below will drift; rerun before
     send.

  7. **Format-patch with the `RFC` subject-prefix.**

        git format-patch -12 --cover-letter \
            --subject-prefix='PATCH RFC' \
            --base=master \
            -o /tmp/backend-ops-abstraction-rfc-emit/

     Cover-letter subject:
     `[PATCH RFC 00/12] um: introduce struct um_backend_ops`.
     First iteration goes as RFC, not v1; v1 is the second
     iteration after the design feedback round closes.

## Verification author performed

(Captured 2026-05-14 against the on-branch state; needs re-run on
the cherry-picked submission branch before send.)

  - **Build-tested on ARCH=um defconfig** with both
    `CONFIG_UM_BACKEND_SECCOMP_ONLY` and
    `CONFIG_UM_BACKEND_DYNAMIC` flavours; cpython-parity 21/21
    under both. The 134-module wide-parity sweep
    (`02-workstreams/D-kvm-backend/wide-parity/`) reads 134 PARITY
    / 0 REGRESSION on the contract layer (regressions in that
    sweep are all v2-specific, none touched by Series 4 surface).

  - **Build-tested on ARCH=x86_64 defconfig** — `arch/um/`
    isn't compiled here, but the new header
    `<shared/backend.h>` is `#include`-clean (no symbol leakage
    into other archs).

  - **Boot-tested under qemu-x86 wrapping ARCH=um vmlinux** for
    five-minute uptime, on both backend flavours; no panics, no
    wedges, no contract-violation `WARN_ON_ONCE` firings.

  - **A-07 perf-CI** ran against the checked-in baselines
    (Ryzen 7 7840HS, Xeon E3-1225 v6) via
    `Documentation/virt/uml/redesign/scripts/uml-perf*.sh`. Δ
    ≤ 0.5 % under `SECCOMP_ONLY` (within noise) and ≤ 1.2 %
    under `DYNAMIC` (one indirect-call cost on each HOT op);
    both pass invariant I2's ≤ 5 % regression budget.

  - **20-test conformance suite** under
    `CONFIG_UM_BACKEND_CONTRACT_TEST=y` passes on both
    flavours: lifecycle (4), memory (5), scheduling (4), time
    (3), debug (3), capability-flag invariants (1). One
    KVM-only test (the v2 backend's HOT op probe under
    `KVM_ONLY` Kconfig) is intentionally `SKIP`-ed under
    `SECCOMP_ONLY` and `DYNAMIC` builds; it will move to PASS
    when Series 7 lands.

  - **Manual `git apply --check` simulation** against a 2026-05
    `linux-next` snapshot indicates the dispatch macro
    introduction at `<shared/backend.h>` does not conflict with
    any in-flight UML series.

The build-verification matrix needs re-running on the
cherry-picked submission branch (against current `master`, not
`umlctl-deploy`) before the actual `git send-email`.

## Maintainer routing

Run `scripts/get_maintainer.pl` right before send; the 2026-05
view:

```text
$ scripts/get_maintainer.pl 0001-*.patch
Richard Weinberger <richard@nod.at>          (MAINTAINER: USER MODE LINUX)
Anton Ivanov <anton.ivanov@cambridgegreys.com>  (MAINTAINER: USER MODE LINUX)
Johannes Berg <johannes@sipsolutions.net>    (MAINTAINER: USER MODE LINUX)
linux-um@lists.infradead.org                  (open list: USER MODE LINUX)
linux-kernel@vger.kernel.org                  (open list)
```

The MAINTAINERS file's USER MODE LINUX section covers
`arch/um/` already; Patch 12 adds `arch/um/backend/` and
`arch/um/include/shared/backend.h` to the path list, so future
runs of `get_maintainer.pl` on those paths route correctly.

Manual additions for cover-letter Cc:

  - **Benjamin Berg `<benjamin@sipsolutions.net>`** — author of
    the 6.16-merged seccomp work that Series 4 wraps. The
    seccomp dispatch path is what Patches 3-7 lift into the
    ops table; he has the strongest review signal on whether
    the table accommodates seccomp cleanly.

  - **Tiwei Bie `<tiwei.btw@antgroup.com>`** — UML contributor
    via the Ant Group port; reviews most cross-cutting UML
    series.

  - **`linux-arch@vger.kernel.org`** — the ops-table pattern
    might be of interest to other archs that currently use
    similar informal flag-based dispatch (powerpc's pseries vs
    powernv branches, riscv's various m-mode trap dispatchers).
    Not asking for ack; signalling availability.

  - **`linux-kernel@vger.kernel.org`** — open list per kernel
    convention.

Total Cc list (target): 4 maintainers + 2 lists + 1 cross-arch
list. Well under the "20 recipients max for a single mail" LKML
heuristic.

### Send recipe (when authorized)

```text
cd /tmp/backend-ops-abstraction-rfc-emit
git send-email \
    --to=linux-um@lists.infradead.org \
    --cc=linux-kernel@vger.kernel.org \
    --cc=linux-arch@vger.kernel.org \
    --cc=richard@nod.at \
    --cc=johannes@sipsolutions.net \
    --cc=anton.ivanov@cambridgegreys.com \
    --cc=benjamin@sipsolutions.net \
    --cc=tiwei.btw@antgroup.com \
    *.patch
```

(Rerun `scripts/get_maintainer.pl --file arch/um/include/shared/backend.h`
right before sending to pick up any maintainer rotation since
2026-05.)

## checkpatch findings (anticipated)

Until the squash audit produces the actual `.patch` files,
checkpatch findings are projected from the on-branch state:

  - **Patch 1 (header) — checkpatch SHOULD see**:
    `MACRO_ARG_REUSE` on the `um_backend_dispatch(op, ...)` macro.
    The macro intentionally evaluates `op` once (token-paste at
    the call site); the warning is the well-known
    `__VA_ARGS__`-passthrough macro idiom that checkpatch
    misfires on. Documented in the cover letter §"Anticipated
    review questions" (d).

  - **Patches 4-7 (the dispatch migrations) — checkpatch SHOULD
    see**: zero new warnings; the migration is mechanical and
    preserves the surrounding code's existing checkpatch score.

  - **Patch 10 (KUnit suite) — checkpatch SHOULD see**:
    `LONG_LINE_STRING` on a few `KUNIT_EXPECT_*` invocations
    where the expected-value string runs > 75 chars. Code-snippet
    line-length warnings on KUnit assertions are widely accepted
    by tracing / arch maintainers per recent precedent
    (`bpf-hygiene-v1/SUBMISSION-NOTES.md`'s patch 2 walked the
    same false positive).

The checkpatch script will run on every `.patch` produced by
the squash audit and on `0000-cover-letter.patch.md` after it
becomes the real `0000-cover-letter.patch`. Per the
ftrace-notrace-generic-v1 precedent, prose `.md` files pass
checkpatch `--no-tree -f` at 0 errors / 0 warnings because they
contain no source-code patterns; only `.patch` files trigger
real findings.

## False-positive false alarms to pre-empt

Reviewers occasionally object to patterns Series 4 uses; the
following are pre-emptively addressed:

  - **"Why not put the dispatch macro behind a `static inline`
    instead of a preprocessor macro?"** Per D11 — a `static
    inline` would need to know which backend is active, which
    means the function needs an `enum um_backend_kind` switch
    inside it. A preprocessor macro lets the build mode
    (`*_ONLY` vs `DYNAMIC`) decide which form expands, with no
    runtime branch. The macro is also the only form that lets
    USER TUs in arch/um/ use the dispatch surface, because USER
    TUs can't include `<asm/backend.h>` (kernel-only path).

  - **"Why not use `kvm_x86_ops` or `arch_ops` as precedent?"**
    Those table patterns exist for the host kernel's own
    dispatch; UML's backend table is the *guest's* dispatch
    surface, in a different binary layer entirely. The naming
    convention `um_backend_*` deliberately mirrors `kvm_*_ops`
    so reviewers reading both can compare the shapes, but the
    abstraction layers don't overlap.

  - **"This looks like a `struct device_driver` ops table; why
    not register backends through the driver model?"** The
    driver model is wired for hot-plug + sysfs + power
    management; UML backends are compile-time-selected
    singletons that live for the kernel's lifetime. The
    overhead of `bus_type` / `device_driver` / `probe()` /
    `remove()` machinery is wholly wasted. The simpler
    function-pointer struct is what `kvm_x86_ops`, the
    network-device qdisc ops, the irq_chip ops, etc. all use
    for the same reason.

## Why this series stands alone — the upstream-interest pitch

The abstraction is upstream-friendly for three reasons:

  1. **It's a cleanup of arch/um's existing
     fork-and-syscall-trap path.** Even without KVM, the
     abstraction simplifies the dispatch loop in
     `arch/um/os-Linux/skas/process.c::userspace` — today
     ~290 lines of `if (using_seccomp) { ... } else { ... }`
     branches, post-Series-4 ~6 lines + an ops dispatch
     macro. The same applies to the seccomp probe in
     `os_early_checks`, the mm-attach path in
     `kernel/skas/mmu.c`, and the timer path in
     `kernel/time.c`. Reviewers see a net negative line count
     across the dispatch surface even though the new
     `<shared/backend.h>` and `arch/um/backend/seccomp/*` files
     add LoC.

  2. **It's a forward-looking enabling RFC for KVM-v2.** The
     redesign branch has demonstrated that the ops table
     accommodates a third backend (KVM-v2 stub registered under
     EXPERT in Phase A.1, real KVM-v2 in Series 7). Reviewers
     of Series 4 are evaluating the *abstraction's* fitness;
     they're not on the hook for the KVM-v2 implementation
     which has its own series and its own review thread. This
     decouples the politically-sensitive KVM-v2 design from
     the comparatively-mechanical contract design.

  3. **The vtable lives entirely in arch/um.** No
     cross-subsystem impact. `kernel/`, `mm/`, `fs/`, `net/`,
     `drivers/` are all unaffected. Series 4's maintainer-list
     is `linux-um@` + `linux-arch@` for the pattern signal;
     other subsystem maintainers have no review surface here.

## v1 → v2 if review feedback comes in

Keep the local branch `umlctl-deploy` as-is; cherry-pick the
squashed v2 patches onto a fresh submission branch for each
iteration:

```text
git checkout -b backend-ops-abstraction-rfc-v2 master
git cherry-pick <12 squash-audit outputs from v2 squash pass>
# apply review-feedback edits via git commit --amend / rebase -i
git format-patch -12 --cover-letter --subject-prefix='PATCH v2' \
                 --base=master
```

The RFC subject-prefix on v1 drops to plain `PATCH v2` once the
design has converged. (RFCs that go through multiple
iterations are stylistically allowed to keep `[PATCH RFC v2]`
etc., but the kernel convention is to drop RFC once a maintainer
has acked the shape; we follow that convention.)

## Cross-references — lineage back to the UML redesign

The analysis that surfaced and validated this contract lives in:

  - `Documentation/virt/uml/redesign/00-vision.md` — the
    three-layer architecture (Layer 1 = backend abstraction).
  - `Documentation/virt/uml/redesign/01-architecture/
    three-layers.md` — per-layer rationale; backend ops is
    Layer 1.
  - `Documentation/virt/uml/redesign/02-workstreams/
    A-backend-abstraction/` — workstream A's seven tasks
    (A-01..A-07), notes, sketches, and the per-task memos.
  - `Documentation/virt/uml/redesign/02-workstreams/
    A-backend-abstraction/notes/10-lkml-memo.md` — the
    precursor LKML memo (internal artifact; this Series 4
    cover letter is the upstream emission).
  - `Documentation/virt/uml/redesign/04-risks/
    decisions-log.md` — D2, D8, D9, D10, D11, D12, D13, D14,
    D15, D45, D59, D63 (see cover letter §"Lineage" for the
    per-D walk).
  - `Documentation/virt/uml/redesign/06-sequencing/
    PLAN-2026-05-14.md` §6.2 — Series 4's queue position +
    rationale.
  - `Documentation/virt/uml/redesign/upstream-patches/
    SUBMISSION-QUEUE.md` row 4 — the per-series sequencing
    decision.

That analysis is intentionally NOT cited in the upstream
commit messages per the "reviewers see the patch on its own
merits" rule (`upstream-patches/README.md`). If upstream
reviewers ask where the contract design came from, the answer
is "the A-workstream of the UML redesign plan; six months of
sketches against three candidate backends (ptrace, seccomp,
KVM)." The redesign branch is offered for inspection; nothing
in it depends on Series 4 landing upstream.

## When this lands

On acceptance:

  1. Add the upstream commit SHA range to this file
     (`Status` section header note).
  2. Move this directory to `upstream-patches/landed/`
     (create if needed) per `upstream-patches/README.md`.
  3. Update `SUBMISSION-QUEUE.md` row 4 status: "landed
     (upstream <SHA range>)".
  4. Unblock Series 5 + Series 7 in the queue (both cite
     Series 4 as a hard dependency).
  5. Update `04-risks/decisions-log.md` D45 with a follow-up
     entry noting the in-fork scope policy can drop the
     "carry on fork" framing for the A-workstream content.
  6. Update `02-workstreams/A-backend-abstraction/README.md`
     to mark the workstream as "landed upstream + in-fork
     extensions deferred to Series 5 + Series 7."

## Status

  - Cover letter: **DRAFTED 2026-05-14** (this revision).
  - SUBMISSION-NOTES.md: **DRAFTED 2026-05-14** (this file).
  - README.md: **DRAFTED 2026-05-14** (reader's guide).
  - Patches: **NOT YET EMITTED.** Awaiting squash audit pass on
    the on-branch substrate.
  - Submission branch: **not yet created**. Will be
    `backend-ops-abstraction-rfc-v1-submit` off `master`.
  - Send timeline: gated on Series 3 landing; realistic
    earliest send-out is ~2 weeks after Series 3 reaches LKML.
