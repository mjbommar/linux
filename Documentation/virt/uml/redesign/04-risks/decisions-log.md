# Decisions log

Every architectural decision recorded with date, rationale,
alternatives considered, expected lifetime, and triggers for
revisitation. Format inspired by ADRs (Architecture Decision
Records).

---

## D1: Three layers (backend / static-key / compile-time)

**Date:** 2026-04-17
**Status:** Accepted
**Decided by:** Project owner + AI synthesis

**Decision:** The architecture is three orthogonal layers as
documented in `01-architecture/three-layers.md`.

**Alternatives considered:**

1. Single backend (KVM only) + everything compiled in,
   runtime-toggled. **Rejected**: nested-virt environments
   need a non-KVM fallback; sandbox needs minimum TCB.
2. Per-profile forks. **Rejected**: forks diverge; the host
   kernel's discipline (one source, many configs) is the proven
   model.
3. Plugin architecture (loadable backend modules). **Rejected**:
   hot-path module load + indirect call is more expensive than
   compile-time selection + static branch.

**Lifetime:** Indefinite. This is the architectural commitment.

**Revisit if:** A radically different trap mechanism emerges
(e.g., hardware-level userspace VM that supersedes KVM).

---

## D2: Backend ops table is `struct um_backend_ops`

**Date:** 2026-04-17
**Status:** Accepted (design, pending impl)
**Decided by:** Project owner + reviewer #2 critique

**Decision:** Layer 1 is realized as a struct of function
pointers (`struct um_backend_ops`) with ~15 ops. Single-backend
inline builds available for sandbox/embedded profiles via
preprocessor macros.

**Alternatives considered:**

1. Backend as a class hierarchy (C++/Rust). **Rejected**: kernel
   is C; idiom is function-pointer struct.
2. Backend selection at function-call granularity (per-call
   `if backend == kvm`). **Rejected**: branch prediction is
   worse than indirect call to a stable target.

**Lifetime:** Until ops-table contract bumps major version
(unlikely in v1).

**Revisit if:** A new backend can't fit the ops without adding
N backend-specific extensions. Means the ops are too narrow.

---

## D3: Defer KVM backend; seccomp is the default fast path

**Date:** 2026-04-17
**Status:** Accepted (per reviewer #2)
**Decided by:** Project owner after reviewer feedback

**Decision:** KVM backend is the third backend, not the first.
Seccomp is the default fast path because it works in nested-virt
environments (which is most UML deployments).

**Alternatives considered:**

KVM-first design (was the v1 of the redesign doc).
**Rejected** because gVisor's own docs recommend systrap (the
seccomp equivalent) over KVM in nested virt; UML's use cases are
heavily nested.

**Lifetime:** Permanent. Even after KVM lands, seccomp remains
the default in nested environments.

**Revisit if:** Bare-metal-only UML deployments grow
significantly (unlikely).

---

## D4: Profiles ship as defconfig fragments

**Date:** 2026-04-17
**Status:** Accepted

**Decision:** Each profile is `arch/um/configs/profiles/<name>.config`
plus `make ARCH=um uml/<name>` build target. Same source tree.

**Alternatives considered:**

1. Profiles as separate repositories. **Rejected** per
   invariant I1.
2. Profiles as runtime selection only. **Rejected**: sandbox
   profile needs minimum TCB which requires compile-time
   exclusion.

**Lifetime:** Permanent.

**Revisit if:** Kconfig itself is replaced or fundamentally
changed (unlikely).

---

## D5: Library mode is a sibling artifact

**Date:** 2026-04-17
**Status:** Accepted

**Decision:** Library mode (`liblinux.a`) ships as a sibling of
the ring-split UML binary, not as a runtime mode. Same source
tree, different `main()`.

**Alternatives considered:**

1. Library mode as a Kconfig of ring-split UML. **Rejected**:
   different consumer API; can't be runtime-flipped.
2. Library mode as fork of LKL with UML compatibility.
   **Rejected**: forks diverge.

**Lifetime:** Permanent.

**Revisit if:** Tazaki's LKL/UML unification gets formal
upstream blessing; we'd merge with it.

---

## D6: Time-travel forces UP (resolves conflict C2)

**Date:** 2026-04-17
**Status:** Accepted

**Decision:** Time-travel mode forces UP at boot. SMP without
time-travel and UP with time-travel are the supported
combinations.

**Alternatives considered:**

1. Vector-clock-based deterministic SMP scheduler.
   **Rejected**: out of scope for v1; significant research
   effort.
2. Disable time-travel when SMP detected. **Rejected**: less
   user-friendly than the inverse.

**Lifetime:** Until research delivers practical deterministic
SMP scheduling.

**Revisit if:** Such a scheduler emerges; relax this restriction.

---

## D7: Don't reimplement; rehost

**Date:** 2026-04-17
**Status:** Accepted (WSL1 lesson)

**Decision:** UML's value is that it IS the upstream Linux kernel.
We don't reimplement subsystems "cleanly" inside UML. Layer 1
changes the host interface; layers 2 and 3 add observability.
Kernel code itself is unchanged from upstream.

**Alternatives considered:**

Sentry-style reimplementation in safer language.
**Rejected** per WSL1 cautionary tale. Reimplementation diverges
from upstream; semantic drift is endless.

**Lifetime:** Permanent.

**Revisit:** Never. This is foundational.

---

## D8: Backend ops table — 18 ops, 5 hot, sync, contract-versioned

**Date:** 2026-04-17
**Status:** Accepted; contract landed and validated by A-02..A-07 in
ptrace + seccomp (20 KUnit tests pass); still pending LKML review
**Decided by:** claude-code session, working from project owner's
brief, A-01.0 through A-01.7 analysis, and the gating rules in
01-architecture/three-layers.md

**Decision:** Workstream A-01 has produced
`arch/um/include/asm/backend.h` and
`Documentation/virt/uml/backend-contract.rst` with the following
shape:

- 18 ops in 5 categories (lifecycle/trap=4, memory=4, scheduling=4,
  time=3, debug=3).
- 5 ops marked HOT (`run_userspace`, `mm_map`, `mm_unmap`,
  `context_switch`, `read_clock_ns`); these are inlinable in
  single-backend builds via the `um_backend_dispatch()` macro.
- All ops are synchronous; no completion handles.
- Per-backend state lives in `arch/um/backend/<kind>/` module
  globals; per-mm state in `struct mm_id`; per-thread state in
  `task->thread`. **No `void *backend_private`** in the ops table.
- Selection: `CONFIG_UM_BACKEND_*_ONLY` for single-backend inline
  builds (sandbox/embedded profiles); `CONFIG_UM_BACKEND_DYNAMIC`
  with `backend=` boot param for multi-backend builds. Default
  arbiter probes in order kvm → seccomp → ptrace.
- Versioning: `UM_BACKEND_CONTRACT_VERSION = 1` (u32). Op
  additions don't bump; signature changes do.

**Sub-resolutions** (resolves A-01 task questions Q1–Q4 and
workstream A README questions Q1–Q4):

- A-01.Q1 sync vs async → sync. (`notes/08-decisions.md`)
- A-01.Q2 backend_private → no slot; mm_id + task->thread.
- A-01.Q3 hot vs cold dispatch → 5 hot via macro inlining,
  13 cold always indirect.
- A-01.Q4 contract version → exposed as a u32 field; bump policy
  defined.
- README.Q1 selection mechanism → both Kconfig and boot param.
- README.Q2 force= behavior → panic on probe failure.
- README.Q3 granularity → 18 ops (architecture doc target was ~15;
  the +3 is justified by the per-backend sketches).
- README.Q4 add ops without bump → yes, supported by the
  versioning policy.

**Validation:**

- All 87 backend-relevant call sites in `arch/um/` (inventory in
  `02-workstreams/A-backend-abstraction/notes/01-call-site-inventory.md`)
  map cleanly to one of: an op, shared kernel/host code, a Layer-2
  static-key gate (workstream B), or a `using_seccomp` branch that
  becomes per-backend impl.
- ptrace, seccomp, and KVM sketches (`notes/04-`, `05-`, `06-`)
  each fit the contract without a per-backend extension to the
  core ops.
- Header compiles standalone; dispatch macro tested correct
  across all 4 build variants (PTRACE_ONLY, SECCOMP_ONLY,
  KVM_ONLY, DYNAMIC).

**Alternatives considered:**

1. Bigger ops table (split `set_timer` into 3 separate ops).
   **Rejected** for granularity rule (cold ops with mode tag are
   indistinguishable in cost from N separate cold ops).
2. Smaller ops table (fold `mm_attach`/`mm_detach` into `init`/
   `shutdown`). **Rejected** because mm lifecycle is per-mm and
   independent from backend lifecycle.
3. `void *backend_private` per op. **Rejected** (see Q2 above).
4. Separate `struct mm_id_kvm` etc with per-backend allocation.
   **Rejected** for ≤24 byte cost vs allocator round-trip + cross-
   backend access discipline.

**Lifetime:** Indefinite. The ops set is expected to grow (new
ops appended without contract bump) but the existing 18 ops are
expected to be stable through workstreams A, B, C, and D.

**Revisit if:**

- LKML feedback on the RFC memo (`notes/10-lkml-memo.md`)
  identifies a missing op or wrong granularity.
- Workstream D's KVM implementation discovers a side channel that
  forces a signature change (would bump
  `UM_BACKEND_CONTRACT_VERSION` to 2).
- Performance CI in A-07 shows the indirect-call cost on a HOT op
  exceeding invariant I2 (≤5% prod-fast regression).

**Cross-references:**

- Header: `arch/um/include/asm/backend.h`
- Contract: `Documentation/virt/uml/backend-contract.rst`
- Workstream task: `02-workstreams/A-backend-abstraction/01-ops-table.md`
- Design notes: `02-workstreams/A-backend-abstraction/notes/`

---

## D9: Per-backend op prototypes live in asm/backend.h

**Date:** 2026-04-17
**Status:** Accepted
**Decided by:** claude-code session, during A-02.6

**Decision:** The per-backend op prototypes (e.g.
`u64 ptrace_read_persistent_clock_ns(void)`) are declared in
`arch/um/include/asm/backend.h` under per-Kconfig blocks
(`#ifdef CONFIG_UM_BACKEND_PTRACE` etc.), not in per-backend
headers (`arch/um/backend/<kind>/<kind>_backend.h`).

**Reasoning:** The `um_backend_dispatch()` macro token-pastes
`<kind>_<op>` at the call site. For the symbol to resolve at compile
time, its prototype must be visible wherever the macro is used —
which is across the whole arch/um tree. Forcing every dispatch caller
to `#include "arch/um/backend/<kind>/<kind>_backend.h"` is fragile
(the right header depends on which `*_ONLY` config is set) and
defeats the macro's "transparent dispatch" property.

**Alternative considered:** Per-backend headers in
`arch/um/backend/<kind>/`. Rejected: violates the "dispatch is
transparent" property; would require call sites to know the active
backend.

**Lifetime:** Stable. The prototype list grows as ops migrate
(workstreams A-02, A-03, D); the structural choice doesn't change.

**Revisit if:** Out-of-tree backends become a goal (they aren't
per the contract spec). In that case the prototypes would need to
move to a generic op-table macro pattern.

**Cross-reference:** `arch/um/include/asm/backend.h`
"per-backend op declarations" section.

---

## D10: A-02 ships in slices, not as one 6-week megaseries

**Date:** 2026-04-17
**Status:** Accepted (operational; aligns with upstream-strategy.md)
**Decided by:** claude-code session, A-02.0

**Decision:** Workstream A-02 (ptrace refactor) is broken into
slices that each land as a small (3–5 patch) reviewable series.
First slice = scaffolding + one cold-op migration + conformance
skeleton. Subsequent slices migrate one HOT op at a time
(`run_userspace`, `mm_map`+`mm_unmap`, `context_switch`) plus a
final cleanup pass for remaining cold ops.

**Reasoning:** `05-validation/upstream-strategy.md` is explicit that
the first deliverable is "3-5 patches." The 6-week budget is
calendar time, not patch volume. Compressing 6 weeks of refactor
into one megaseries would:

- Violate the "no 50-patch monsters" rule.
- Make bisection harder (each intermediate patch must build & run).
- Miss the chance to incorporate maintainer feedback on the
  scaffolding before doing 5 more weeks of mechanical migration.

**Alternative considered:** One-shot full refactor as a single
series. Rejected as above.

**Lifetime:** Operational, applies to A-02 only. Future workstreams
follow the same pattern (small reviewable series) but the slicing
boundaries are workstream-specific.

**Revisit if:** Maintainer feedback on the first slice indicates the
ops table needs structural revision (then we restart from A-01.3).

---

## D11: Dispatch surface lives in arch/um/include/shared/backend.h; struct stays in asm/backend.h

**Date:** 2026-04-17
**Status:** Accepted
**Decided by:** claude-code session, during A-02.HOT-1.2

**Decision:** The `um_backend_dispatch()` macro, the `extern const
struct um_backend_ops *um_backend` declaration, and the per-backend
op prototypes live in `arch/um/include/shared/backend.h`. The full
struct definitions (`struct um_backend_ops`, `struct um_backend_args`,
`enum um_timer_mode`, `enum um_backend_kind`, `init_backend()` and
the `UM_BACKEND_CONTRACT_VERSION` macro) stay in
`arch/um/include/asm/backend.h`, which now `#include <backend.h>`.

**Reasoning:** USER TUs in `arch/um/` (those built with `USER_CFLAGS`
via `arch/um/scripts/Makefile.rules`, including legacy
`os-Linux/skas/*` and the new `arch/um/backend/<kind>/*_user.c` files
introduced by HOT-1) cannot include `<asm/backend.h>` because
`asm/` is on the kernel-only include path. They CAN include shared
headers (those under `arch/um/include/shared/`).

The `um_backend_dispatch()` macro is needed at every call site that
goes through the ops table — including USER-side trap loop code like
`os-Linux/skas/process.c::userspace()`. Splitting the header lets
USER TUs use the dispatch macro while keeping the kernel-only struct
definitions out of USER TU build context (those use kernel types like
`struct task_struct` that USER TUs don't see).

**Alternative considered:** Keep everything in `asm/backend.h` and
have USER TUs call per-backend symbols directly (bypass the dispatch
macro). Rejected: leaks build-mode dependencies (`PTRACE_ONLY` vs
`SECCOMP_ONLY` vs `DYNAMIC`) into every call site.

**Lifetime:** Stable. Future ops added to the prototype list go into
shared/backend.h.

**Cross-reference:** `arch/um/include/shared/backend.h`,
`arch/um/include/asm/backend.h`, A-01.3 deliverable.

---

## D12: init_backend() is the single source of truth for backend selection

**Date:** 2026-04-18
**Status:** Accepted (in response to external review)
**Decided by:** External review + claude-code session

**Decision:** All backend selection runs through
`arch/um/kernel/backend.c::init_backend()`, called from
`arch/um/kernel/um_arch.c::linux_main()` immediately after
`os_early_checks()`. After init_backend returns:

- `um_backend` points at the chosen ops table (immutable thereafter).
- `using_seccomp` (the legacy global flag) is set to match
  `um_backend->kind == UM_BACKEND_KIND_SECCOMP`. Both representations
  agree from this point on.
- `validate_hot_ops()` has panicked the kernel if any HOT op is NULL.

`os_early_checks()` is now Kconfig-aware: it only runs the seccomp
probe if `CONFIG_UM_BACKEND_SECCOMP=y` (and `seccomp=on`), and only
runs the ptrace probe if `CONFIG_UM_BACKEND_PTRACE=y`. The trap-loop
router in `arch/um/os-Linux/skas/process.c::userspace()` is a single
`um_backend_dispatch(run_userspace, regs)` call — no `using_seccomp`
branching.

**Reasoning:** Pre-D12 there were three independent authorities:

1. `init_backend()` (Kconfig only).
2. `os_early_checks()` setting `using_seccomp` from boot probe.
3. `userspace()` routing on `using_seccomp`.

These could disagree — e.g. DYNAMIC builds compiled in seccomp_ops
but `using_seccomp=0` (default) → dispatch went to seccomp_run_userspace
through um_backend, but mm_attach (sharing the global) ran the
ptrace setup. External review flagged this as a "high" finding.

**Alternative considered:** Keep `using_seccomp` as the source of
truth, derive `um_backend` from it. Rejected because the dispatch
macro needs a compile-time decision in `*_ONLY` builds — it can't
read a runtime flag.

**Lifetime:** Stable. The selection logic gets refined by A-04
(boot-param parsing, `force=` panic) but the single-source-of-truth
shape remains.

**Cross-reference:** REVIEW.2 in this session's task list;
`arch/um/kernel/backend.c::init_backend()`,
`arch/um/os-Linux/start_up.c::os_early_checks()`,
`arch/um/kernel/um_arch.c::linux_main()`,
`arch/um/os-Linux/skas/process.c::userspace()`.

---

## D13: Kconfig invariant — _ONLY = exactly one backend; DYNAMIC = both

**Date:** 2026-04-18
**Status:** Accepted (in response to external review)

**Decision:** The dispatch-mode choice block uses `select` to
auto-include the right set of backend implementations:

- `CONFIG_UM_BACKEND_PTRACE_ONLY` selects `UM_BACKEND_PTRACE` only.
- `CONFIG_UM_BACKEND_SECCOMP_ONLY` selects `UM_BACKEND_SECCOMP` only.
- `CONFIG_UM_BACKEND_DYNAMIC` selects both.

The previous Kconfig allowed mixed builds (PTRACE+SECCOMP both
compiled in, *_ONLY chosen for dispatch) which permitted the
split-brain bug behind D12 and wasted binary size. Per the planned
shape in `02-workstreams/A-backend-abstraction/04-kconfig.md`.

**Lifetime:** Stable through workstream A; KVM_ONLY/KVM-included-in-
DYNAMIC slots open for workstream D.

---

## D14: All ops must be non-NULL — no NULL/-ENOSYS fallback

**Date:** 2026-04-18
**Status:** Accepted (in response to external review)

**Decision:** The contract spec previously promised that cold ops
could be NULL with -ENOSYS fallback. The dispatch macro can't
deliver this (it's a plain function-pointer call; can't return
-ENOSYS for void or u64 ops). All in-tree backends must populate
every op. Stubs that aren't yet meaningful return -EOPNOTSUPP (for
ops returning int) — see `ptrace_read_guest_regs` and
`seccomp_read_guest_regs`.

`init_backend()`'s `validate_hot_ops()` enforces this for the 5
HOT ops at boot; cold ops are convention-enforced.

**Lifetime:** Stable.

**Cross-reference:** `Documentation/virt/uml/backend-contract.rst`
"Conventions" + "Versioning policy" sections, REVIEW.3 in this
session's task list.

---

## D15: backend=seccomp triggers the seccomp probe; backend=auto stays legacy-compatible

**Date:** 2026-04-18
**Status:** Accepted (in response to second external review pass)

**Decision:** `os_early_checks()` runs the seccomp probe whenever
the new `backend=` boot param requested seccomp (either named or
forced), in addition to the legacy `seccomp=on/auto` trigger. This
makes `backend=seccomp` self-sufficient — users no longer need to
also pass `seccomp=on` for the new param to actually take effect.

`backend=auto` (the default) keeps the historical behavior:
seccomp probe runs only if `seccomp=` opted in. A future release
may change `auto` to unconditionally probe seccomp first (gVisor
pattern), but that's a UX change and is deferred.

**Reasoning:** External review caught that `backend=seccomp`
silently degraded to ptrace because the probe never ran. The probe
is the prerequisite for `using_seccomp = 1`, which the dynamic
arbiter reads to pick the backend.

**Alternatives considered:** Make `auto` unconditionally probe
seccomp. Rejected for now — it's a behavior change for default
users.

**Cross-reference:** `arch/um/os-Linux/start_up.c::os_early_checks`,
`Documentation/virt/uml/backend-contract.rst` (selection mechanism
section), `Documentation/virt/uml/redesign/scripts/uml-boot-matrix.sh`
(updated DYNAMIC rows).

---

## D16: Reconciling review-01 recommendations vs mid-plan artifacts

**Date:** 2026-04-18
**Status:** Accepted

**Decision:** The recommendations in
`Documentation/virt/uml/review-01/README.md` are triaged into two
buckets before any edits:

1. **Real deviations from the plan** (must correct) — issues where the
   implementation diverged from what the plan specified and the
   deviation is still on the current head:
   - stale op names / arg fields in `01-architecture/three-layers.md`
     (the delivered contract is `run_userspace`/`mm_map`/`mm_unmap`
     with `(requested, force, runtime_opts)`; the narrative still
     showed `syscall_dispatch`/`map_user` with `want_*`);
   - stale "A-04 will…" scaffolding comments in shipped code where
     A-04 has already landed;
   - stale "design draft" status on A-01 even though A-02..A-07
     implemented and validated the design.

2. **Mid-plan artifacts** (not deviations; do not edit) — items that
   look unfinished but are exactly what the plan said should be
   deferred to a later workstream:
   - lifecycle ops stubbed (probe/init/shutdown return 0) — the
     real probe lives in `os_early_checks()`; lifting it into the
     ops table is future cleanup, not A-02 scope;
   - `host_io_submit` reserved but unused — virtio-uml uses
     `os_*` directly, and wiring the reserved op is a B/C workstream
     concern;
   - per-backend `_user.c` split enforced by Makefile.rules — this
     is the USER/KERNEL TU discipline the plan mandates, not a bug;
   - KVM backend absent — it's workstream D, deliberately deferred.

**Reasoning:** Reviewing an in-flight project that conflates
"unfinished per the plan" with "deviates from the plan" produces
spurious cleanup pressure and scope drift. Making the distinction
explicit in the decisions log lets future reviewers (and future-me)
triage similar feedback consistently: "does this recommendation
describe a delta from the design that should be corrected, or is
it observing that a workstream we haven't started hasn't happened?"

**Alternatives considered:** Accept all recommendations as equally
weighted followups. Rejected because it conflates policy ("finish
what you started") with scope drift ("do workstreams B/C/D in A's
commit window").

**Cross-reference:** `Documentation/virt/uml/review-01/README.md`
(the review itself), `review-01/RESPONSE.md` (per-item response),
`02-workstreams/A-backend-abstraction/notes/recon-audit.md`
(file-by-file verdict for the RECON pass).

---

## D19: Layer 2 gates ship on C fallback until B-04 unlocks JIT patching

**Date:** 2026-04-18
**Status:** Accepted

**Decision:** Workstream B-02 lands the seven static-key gates and
six hook sites using the kernel's generic `static_branch_unlikely`
machinery. Because `arch/um/Kconfig` does not yet
`select HAVE_ARCH_JUMP_LABEL`, off-state gates compile to the
C-fallback form (load + test + predicted-not-taken branch) rather
than the 5-byte JIT NOP. Off-state cost is ~1–2 ns per gate
today, not the ~0.3 ns the three-layers.md model assigns to the
JIT form.

Full JIT patching is gated on **B-04** (`.text` section split), which
provides the writable `.text.patchable` region required for
`text_poke`-style runtime transformation. Once B-04 lands,
`arch/um/kernel/jump_label.c` is a thin wrapper over B-04's
mprotect helpers and `select HAVE_ARCH_JUMP_LABEL` becomes
trivial to enable.

**Reasoning:** UML's `.text` is mapped RO by the host loader, so
in-place instruction rewriting requires an explicit writable window.
Building that window without the section split risks making the
*entire* kernel `.text` RW for the duration of a patch — a security
regression relative to today's host-enforced RO text.

Until B-04 lands, invariant I3 ("gates JITted to NOPs") is met **in
spirit** (~1 ns per gate, well under the 2 ns ceiling) but not **in
letter**. Progressing B-02 on C fallback unblocks B-03/B-05/B-06
and workstream C's consumers; holding B-02 for B-04 would serialize
the workstream unnecessarily.

**Alternatives considered:**

1. Implement `text_poke` without section split — rejected; makes
   all of `.text` writable.
2. Port x86's jump-label machinery wholesale — rejected; `text_poke_bp`
   depends on fixmap + INT3 trampoline that UML (userspace-hosted)
   doesn't share.
3. Hold B-02 until B-04 lands — rejected; serializes B workstream
   for no capability gain. Gates work today; the cost delta is
   within the architecture's I3 budget.

**Revisit:** When B-04 lands. At that point add `HAVE_ARCH_JUMP_LABEL`,
implement `arch/um/kernel/jump_label.c`, verify objdump shows NOPs,
re-measure in B-05, and mark D19 as superseded.

**Cross-reference:**
`02-workstreams/B-static-key-hot-paths/notes/jump-label-status.md`,
`02-workstreams/B-static-key-hot-paths/04-section-split.md` (the
unblocking workstream), `01-architecture/invariants.md §I3`.

---

## D20: Gate every IRQ-delivery path, including time-travel direct dispatch

**Date:** 2026-04-18
**Status:** Accepted (in response to external review finding B-02.IRQ)

**Decision:** `um_on_irq_entry` is invoked from every path that
reaches `generic_handle_irq()` — the three paths are `do_IRQ` (the
standard trap-delivered IRQ entry), the time-travel
`irq_event_handler` (synthetic event delivery), and
`irq_do_pending_events` (replay of events that arrived while IRQs
were suspended). The single gate fires from all three call sites
so a record/replay consumer observes every IRQ regardless of
time-travel state.

The two synthetic paths do not have a trap `struct uml_pt_regs`
available; the hook is called with `regs = NULL` there. The B-02
slow paths swallow `regs`, so this is harmless today; a future
real consumer that dereferences `regs` must explicitly handle NULL
for those two sites.

**Reasoning:** External review pointed out that the original B-01
audit claimed "one `do_IRQ` hook covers all meaningful IRQ delivery",
but the time-travel direct-dispatch path bypasses `do_IRQ` and hit
`generic_handle_irq` directly. Leaving that path ungated would give
observability consumers a lopsided view (real IRQs observed, time-
travel IRQs invisible). The fix is four lines of code; narrowing
the doc to declare the gap "out of scope" was the alternative but
produces a worse architectural story.

**Alternatives considered:** Narrow the audit doc to say "do_IRQ
path only; time-travel synthetic IRQs observed via `um_on_clock_read`
not `um_on_irq_entry`." Rejected — `um_on_clock_read` is semantically
different (it observes clock reads, not IRQ delivery); a record/replay
consumer that wants every IRQ delivery would silently miss the
time-travel path.

**Cross-reference:** `arch/um/kernel/irq.c` (three call sites),
`02-workstreams/B-static-key-hot-paths/notes/hot-paths.md` §"Hook 5".

---

## D21: Drop `sanitize_paranoid` as named-but-dead infrastructure

**Date:** 2026-04-18
**Status:** Accepted (in response to external review finding B-sanitize_paranoid)

**Decision:** The `um_hook_sanitize_paranoid` gate introduced in
B-02 is removed. It was declared in `arch/um/include/asm/um-hooks.h`
and defined in `arch/um/kernel/hooks.c` but not dispatched from any
`um_on_*()` helper. Reviewer feedback: a named-but-dead gate draws
pushback as "dead infrastructure" rather than praise for forward
compatibility.

**Policy going forward:** new gates land together with their first
real call site. The comment in `um-hooks.h` §"Gate declarations"
states the rule explicitly. The `sanitize_paranoid` intent
(reserved for aggressive sanitizer-mode fuzz work) is preserved in
`08-future-phases/01-end-user-ideal-world.md` as a phase-F item; it
will be reintroduced as a real gate when a consumer lands.

**Reasoning:** The cost of re-adding a gate when its consumer is
ready is near-zero (one enum entry + one declaration + one dispatch
line). The cost of shipping an undispatched gate is a credibility
hit in maintainer review.

**Cross-reference:**
`arch/um/include/asm/um-hooks.h` (declaration removed),
`arch/um/kernel/hooks.c` (definition + EXPORT_SYMBOL_GPL removed,
enum row removed, names/keys tables trimmed).

---

## D22: Per-CPU slow-path counters (replacing the global atomic64_t)

**Date:** 2026-04-18
**Status:** Accepted (in response to external review finding B-stats)

**Decision:** `um_hook_hits` moves from a global
`atomic64_t[UM_HOOK__COUNT]` to a per-CPU
`struct um_hook_percpu_counters` with a plain `u64[]` per CPU.
Writes are `this_cpu_ptr() + ++`; reads sum across
`for_each_possible_cpu`.

**Reasoning:** The global atomic form introduced cross-CPU
cacheline contention on every slow-path invocation, which distorts
"gate on" benchmarks — every gate-on measurement was measuring a
shared-cacheline atomic, not the hook's actual cost. A future
workstream C consumer (real ftrace, real kcov) would inherit the
distortion when comparing its own overhead against this baseline.

Per-CPU counters are the kernel-standard pattern for high-frequency
observability counters (see `kernel/trace/`, `kernel/sched/stats.c`).
Read-side sum is O(nr_cpus), which matters only for the `stats`
file read — not a hot path.

Reads are inherently racy against concurrent writers on other CPUs;
acceptable for observability telemetry where rate of change matters
more than a precise snapshot.

**Measurement impact:** baseline rebuilt at `notes/bench-baseline.json`.
On-state numbers rose (per-CPU write + function-call overhead >
single-atomic cost on an uncontended single-CPU UML, but this
reflects the real steady-state cost of a non-inlined stub with a
real counter write). The baseline is now a cleaner comparison
target for workstream C's real consumers.

**Cross-reference:** `arch/um/kernel/hooks.c`
(`DEFINE_PER_CPU_ALIGNED` + `um_hook_stats_inc`/`um_hook_stats_read`
rewrite), `02-workstreams/B-static-key-hot-paths/notes/bench-baseline.json`.

---

## D23: RFC patch-series split by compile boundary (not by B-NN task)

**Date:** 2026-04-18
**Status:** Accepted (policy for upstream send)

**Decision:** The workstream-B commit (`570b0c386e0c`) will not be
RFC'd as a single 33-file, ~1500-LOC patch, nor will it be split
into six patches by B-NN task boundary. Instead it splits by
**compile boundary** into six logically independent patches, in
the order:

1. Gate infrastructure + first live hook site (header, `hooks.c`,
   one insertion so the call site is immediately visible).
2. Remaining five hook-site insertions.
3. debugfs + stats (`um_debugfs.c`, `debugfs.rst`).
4. Section split + patchable text helpers (`patchable.h`,
   `section_split.c`, linker-script edits, `section-split.rst`).
5. Benchmark harness + baseline + host scripts (`hooks_bench.c`,
   `uml-gate-bench.sh`, `uml-gate-bench-compare.sh`,
   `bench-baseline.json`).
6. Docs + selftest + any review-response artifact.

Each intermediate patch must compile and boot (bisect-clean). The
B-NN task boundary is too fine-grained for LKML review — B-NN
steps often touch files that only make sense in context of the
next B-NN step. The compile-boundary split keeps each patch
functionally self-contained: reviewers can apply patch 1 alone,
build, and observe the gate in action at a single call site
before seeing how it grows.

**Execution:** not yet executed; hold until the user decides to
RFC. Splitting now would complicate any further review-driven
fix commits. Expected mechanism: `git rebase -i` on
`570b0c386e0c` to split, or a fresh branch + cherry-pick; pick
whichever produces a cleaner history.

**Cross-reference:** `02-workstreams/B-static-key-hot-paths/README.md`
task table (B-01..B-06 map to the compile-boundary patches in the
order above), `05-validation/kernel-dev-checklist.md` §Scale 2.

---

## D24: Future-phases parking lot begins strictly where workstream C ends

**Date:** 2026-04-18
**Status:** Accepted

**Decision:** `08-future-phases/` is a parking lot for ideas
beyond the current A-D roadmap. To prevent it from quietly
re-planning workstream C, the policy is:

- Items in `08-future-phases/` begin strictly where C's task
  list ends. Any item that fits an existing `C-NN.md` spec
  is a C task, not a future phase.
- When scope is ambiguous, it moves to C first. Future phases
  inherit only what C has explicitly written off.
- "Phase E wants X" does not override "C plans X" — C wins by
  default; phase E takes over only if C's owner says so in
  writing.
- Adding a new note here requires a one-line check against C's
  task list; on match, reclassify the note as a C proposal and
  move it to `02-workstreams/C-profiles-and-gaps/`.

**Reasoning:** Parking-lot drift is a slow-motion failure mode:
ideas accumulate, the team later rediscovers that two-thirds
overlap with the next workstream, and decisions get re-litigated.
An explicit boundary avoids that.

**Applied in this pass:** PARK.8's spike (which found that
`user_events`, `ftrace`, KCOV already work in UML) resulted in
three "missing" items being reclassified as C-workstream work
(document + selftest) rather than future-phase engineering.
`uprobes` stays in the "needs real port" bucket but should land
as a C task, not a Phase-F task.

**Cross-reference:** `08-future-phases/README.md` (boundary rule
published there), `08-future-phases/notes/uprobes-spike.md`
(spike findings that exercised the boundary).

---

## D25: Non-kernel artifacts (rootfs recipes, image data) live out-of-tree

**Date:** 2026-04-18
**Status:** Accepted (user directive during workstream C kickoff)

**Decision:** Everything needed to *run* a workload inside UML that
isn't itself kernel source — rootfs build recipes, golden guest
images, test workload bundles, captured trace artifacts, large data
fixtures — lives in **separate GitHub repositories**, not in this
kernel tree.

What stays in-tree:

- ``arch/um/configs/base_defconfig`` and
  ``arch/um/configs/profiles/*.config`` — these are text Kconfig
  fragments and belong with the code they configure.
- ``Documentation/virt/uml/profiles/*.rst`` — user-facing
  documentation.
- ``tools/testing/selftests/um/`` — selftest scripts that run
  against in-tree builds.
- Shell scripts and JSON baselines under
  ``Documentation/virt/uml/redesign/scripts/`` and
  ``Documentation/virt/uml/redesign/02-workstreams/.../notes/`` —
  these reference in-tree behaviour and are small.

What moves out-of-tree (repos to be created when phase E lands):

- **uml-recipes**: rootfs build recipes for Debian, Alpine, etc.
  Makefiles / Dockerfiles / debootstrap invocations that produce a
  bootable UML rootfs.
- **uml-images** (or a data branch / release artifact): prebuilt
  rootfs images for reviewer convenience. Binary data does not belong
  in the kernel git history.
- **uml-workloads** (optional, may fold into uml-recipes): golden
  service workloads (FastAPI, nginx, redis) used by phases I and H.

**Reasoning:**

- Linux kernel convention: the tree holds source, not large data
  artifacts. kselftest's bpf harness, rust-for-linux's CI images,
  KVM's testing artifacts all use external storage.
- Keeps the kernel tree small; rootfs builders churn on a different
  cadence than kernel code.
- Separates upstream-LKML audience (needs kernel code) from
  end-user-reviewer audience (wants a CLI and images).

**Applied in this pass (C-01):** everything we shipped is in-tree
because it's text. The ``uml run`` launcher, rootfs builder, and
golden workloads that phase E introduces will all live in the
separate repositories.

**Cross-reference:**
``08-future-phases/01-end-user-ideal-world.md`` §"Phase E" (will be
updated to name the specific repos once they exist).

---

## D26: KASAN + KCSAN are mutually exclusive upstream — ship as separate profiles

**Date:** 2026-04-18
**Status:** Accepted

**Decision:** The original plan matrix for `fuzz-deep` called for
both `KASAN` and `KCSAN` in the same build. Upstream Linux's
`lib/Kconfig.kcsan` declares `depends on DEBUG_KERNEL && !KASAN`,
making the combination impossible on a stock kernel.

Resolution: split into two profiles.

- `fuzz-deep` stays KASAN-focused (heap corruption; its current
  shape pre-C-03).
- A new `race` profile is added (workstream C-03) with
  `CONFIG_KCSAN=y`, no KASAN, plus lockdep and debugfs for the
  `/sys/kernel/debug/kcsan` runtime control.

Users pick the detector class matching the bug class they're
hunting.

**Reasoning:** Lifting the upstream exclusion would require a
substantial patch series to both the KASAN and KCSAN runtimes to
avoid instrumentation collisions. That is orthogonal to the UML
port and shouldn't be done as part of it. Splitting profiles is
cheap (one fragment + one doc + one selftest row) and gives the
user the same menu of tools, just requiring a build-time choice.

**Alternatives considered:**

1. Fork fuzz-deep into fuzz-deep-kasan / fuzz-deep-kcsan — more
   profile clutter, same outcome.
2. Patch KASAN+KCSAN to be compatible — out of scope for a UML
   port.
3. Document fuzz-deep as "KASAN only; for KCSAN build a custom
   config" — worse UX, no discoverable menu entry.

**Cross-reference:** `arch/um/configs/profiles/race.config`,
`Documentation/virt/uml/profiles/race.rst`,
`Documentation/virt/uml/redesign/03-profiles/fuzz-deep.md`
(reconciliation note at the top),
`Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/03-port-kcsan.md`.

---

## D27: C-05 ships function tracer + dynamic ftrace; function-graph deferred

**Date:** 2026-04-18
**Status:** Accepted (scope decision for C-05 design pass)

**Decision:** The C-05 (ftrace) task lands `HAVE_FUNCTION_TRACER`
and `HAVE_DYNAMIC_FTRACE` but **not** `HAVE_FUNCTION_GRAPH_TRACER`
in its first series. Graph lands in a follow-up task after a
dedicated signal-stress test demonstrates that the return
trampoline survives UML's SIGALRM-driven preemption.

**Reasoning:**

Function graph works by overwriting a traced function's saved
return address on the kernel stack with the address of
`return_to_handler`; the real return address is pushed on ftrace's
per-task return stack. When the traced function executes RET, it
jumps to `return_to_handler`, which pops from the return stack
and tail-jumps to the real caller.

On UML, preemption arrives as SIGALRM (see
`arch/um/os-Linux/signal.c:64,128,156..158`). If SIGALRM fires
between the return-address rewrite and the trampoline's pop-and-
jump, the signal handler runs with the kernel stack in a
transient state where the "return address" slot holds the
trampoline target — not the real caller. Most signal paths don't
care, but any code that walks the stack for unwinding (lockdep,
KASAN report paths, soft-lockup watchdog) sees a frame that
points to `return_to_handler` and may produce misleading traces
or, worse, re-enter the trampoline.

The bug shape is narrow but real and architecturally awkward.
Shipping the function tracer first gets the bulk of the
observability story — `trace_printk`, tracepoints,
`set_ftrace_filter` — into the research profile without the
hazard. Graph remains an achievable follow-up.

**Alternatives considered:**

1. Ship both at once. Rejected — more surface area, one race
   delays the whole series.
2. Ship graph with graph-specific disablement around signal
   paths. Rejected — identifying every signal-walked frame is
   open-ended; the surface isn't obvious without concrete
   failure cases to drive it.
3. Skip graph permanently. Rejected — kprobes-on-ftrace and
   several selftests depend on the graph tracer being
   available under `research`.

**Revisit:** When a concrete signal-stress test exists
(`tools/testing/selftests/um/ftrace-graph-signal-race/` or
similar) and demonstrably passes 10k iterations without a lockdep
splat, UBSAN report, or visible stack corruption. At that point a
C-05-followup task lands graph and updates this entry to
superseded.

**Upstream precedent for conditional `HAVE_FUNCTION_GRAPH_TRACER`:**
three in-tree arches already gate graph selection on a subordinate
capability rather than selecting it unconditionally:

- `arch/arm/Kconfig`: `select HAVE_FUNCTION_GRAPH_TRACER if (!THUMB2_KERNEL)`
- `arch/x86/Kconfig`: `select HAVE_FUNCTION_GRAPH_TRACER if X86_32 || (X86_64 && DYNAMIC_FTRACE)`
- `arch/riscv/Kconfig`: `select HAVE_FUNCTION_GRAPH_TRACER if HAVE_DYNAMIC_FTRACE_WITH_ARGS`

UML deferring graph behind its own capability (a to-be-added
`UM_FTRACE_GRAPH_SAFE` or equivalent) is well within existing
kernel convention. The
`Documentation/livepatch/reliable-stacktrace.rst` note on
`return_to_handler` unwinder reliability is the upstream
acknowledgment that this frame is architecturally tricky and
may warrant per-arch opt-outs.

**Cross-reference:**
`02-workstreams/C-profiles-and-gaps/05-port-ftrace.md` §"Out of
scope for this task",
`arch/x86/kernel/ftrace_64.S` (`return_to_handler`, reference
implementation).

---

## D28: UML ftrace text patching — page-scoped mprotect + stop_machine; no alias mm available

**Date:** 2026-04-18 (revised after security review; earlier draft
specified process-wide mprotect without stop_machine)
**Status:** Accepted (mechanism decision for C-05 design pass)

**Decision:** When C-05 patches nop5↔call __fentry__ at runtime,
it uses the following sequence on UML:

1. Acquire `text_mutex` (generic ftrace core already requires it).
2. `stop_machine_cpuslocked()` — freeze all other UML vCPU host
   threads so no peer UML kernel code can run while the target
   page is writable.
3. Inside the stopped region, on the patching CPU only:
   a. `um_kernel_text_patch_begin(addr, 5)` — new companion helper
      (see "B-04 extension" below) that `mprotect(RW)` on the one
      (or, when the 5-byte site straddles a page boundary, two
      adjacent) PAGE_SIZE page(s) containing `addr`, validated
      to lie within `[_text, _etext)`. Longer ranges are
      rejected with -EINVAL.
   b. `memcpy(addr, new_insn, 5)`.
   c. `um_kernel_text_patch_end(addr, 5)` — `mprotect(RX)` on
      the same pages.
4. `stop_machine_cpuslocked()` returns.
5. Release `text_mutex`.

No port of `text_poke_bp` or INT3 breakpoint emulation — UML is
already explicitly excluded from the `int3_emulate_*` helpers via
`#ifndef CONFIG_UML_X86` in `arch/x86/include/asm/text-patching.h:134..216`.

**Why `stop_machine` is required (not optional):**

UML has exactly one `struct mm_struct` — the host process's.
Every UML vCPU is a host thread sharing that one mm. When we
`mprotect(page, RW)` to patch, the page becomes writable for
**every UML kernel host thread**, not just the patching CPU.

This is architecturally weaker than what bare-metal arches do:

- **arm32 and arm64** use `FIX_TEXT_POKE0` fixmap slots — the
  target page is mapped RX in the primary `init_mm` throughout;
  a separate virtual address (the fixmap slot) is mapped RW on
  the patching CPU only, for the duration of the write. See
  `arch/arm/kernel/patch.c` and `arch/arm64/kernel/patching.c`.
- **riscv** uses the same fixmap pattern (`FIX_TEXT_POKE0/1`).
  See `arch/riscv/kernel/patch.c`.
- **x86** goes further: `text_poke_mm` (arch/x86/kernel/
  alternative.c:2503..2637) is a dedicated `struct mm_struct`
  with a per-PTE RW alias at a KASLR-randomized virtual address;
  `use_temporary_mm()` loads its page tables on the local CPU
  for the write. The primary kernel mm **never** has text RW.

None of these approaches are available to UML. UML cannot
create a second mm with page-table aliasing because UML is a
userspace program and its host process has one mm by
construction. A second `mmap()` of the same backing pages is
not equivalent (MAP_PRIVATE COW, MAP_SHARED changes the
semantic, and neither gives per-thread view control).

Given that a RW window on the target page is structurally
necessary, the mitigation is: **ensure no other UML kernel
code runs during the window**. `stop_machine_cpuslocked()`
gives exactly that property. The window is bounded in time
(single 5-byte memcpy) and in space (one 4 KB page).

This is the closest UML can approximate the arm/riscv/x86
"primary mapping stays RX; only the patching CPU sees RW"
property.

**Security envelope (what D28 does and does not buy):**

With D28 as specified, a memory-safety bug elsewhere in the
UML kernel that produces a stray write cannot corrupt kernel
text during the patch window, because `stop_machine` has
paused every kernel path that could issue such a write.

What D28 does **not** buy:

- A concurrent host-side attacker with write access to the UML
  host process's memory (e.g. a ptrace attacher) can still
  write to the page during the window. UML has always assumed
  the host process is trusted; this does not regress that
  assumption.
- If a future hardware interrupt handler runs without being
  paused by `stop_machine` (unlikely on UML — signal-delivered
  "IRQs" go through the UML scheduler which respects stop),
  it would be exempt from the guarantee. The invariant should
  be re-verified if UML ever grows a bottom-half path that
  bypasses the scheduler.
- `prod-fast` and `sandbox` profiles do not enable
  `DYNAMIC_FTRACE` (verified: both profile fragments contain
  `# CONFIG_FTRACE is not set`). The patch window therefore
  never opens in those profiles. `research` / `fuzz` / `fuzz-deep`
  accept the window as part of their instrumentation posture.

**B-04 extension required (separate commit, same series):**

B-04 shipped `um_text_patch_begin/end` that range-validate
against `[__start_um_patch_text, __end_um_patch_text)`. Mcount
call sites are scattered through every traceable function in
`.text` at large, not confined to `.um_patch_text`. C-05
therefore lands a companion pair in the same header:

- `um_kernel_text_patch_begin(void *addr, unsigned long len)`
- `um_kernel_text_patch_end(void *addr, unsigned long len)`

scoped to `[_stext, _etext)` and enforcing single-page coverage
(`addr..addr+len` must lie within one 4 KB page; otherwise
-EINVAL). Keeping the two helper pairs distinct preserves
B-04's tighter invariant for `.um_patch_text` (patchable-
function JIT, future jump-label) and makes the call site
reveal the intent.

**Alternatives considered:**

1. Port `text_poke_bp` anyway. Rejected — UML has no IDT, no
   INT3 emulator, no need for cross-CPU instruction-fetch
   atomicity.
2. Widen existing `um_text_patch_begin/end` to accept any
   `.text` page. Rejected — conflates B-04's patchable-function
   scope with ftrace's kernel-wide scope; reviewers would
   justifiably ask why the range check was loosened.
3. Process-wide `mprotect` across all of `.text`. Rejected —
   maximizes blast radius of any stray write during the window.
4. Skip `stop_machine`, rely on `text_mutex` alone. Rejected —
   `text_mutex` serializes *text patchers*, not *every kernel
   thread that might write to memory by accident*. The
   shared-mm property of UML makes `stop_machine` necessary
   here even though it would be over-engineering on bare-metal.
5. Move text patching into a dedicated host helper process
   (crosvm-style; see C-10). This is the UML analog of
   `text_poke_mm` and would return the primary UML process's
   `.text` to permanent RX. Deferred as a future hardening
   task once C-10 infrastructure exists — too big a dependency
   for C-05's scope.

**Revisit triggers:**

- If UML ever grows a path that runs UML kernel C code without
  honoring the scheduler (e.g. a signal handler that doesn't
  trampoline through `irq_enter`/`irq_exit`), the `stop_machine`
  guarantee weakens. Re-audit then.
- When C-10 (crosvm-style launcher, planned) lands, re-evaluate
  whether a helper-process text-patching path is viable and
  worth the complexity. At that point the follow-up task
  "D28-followup: move ftrace text patching to C-10 helper" is
  the right entry point.

**Cross-reference:**
`arch/um/include/asm/patchable.h` (B-04's helpers + new C-05
helpers to be added),
`arch/x86/include/asm/text-patching.h:134..216`
(`CONFIG_UML_X86` excludes UML from `int3_emulate_*`),
`arch/arm/kernel/patch.c`, `arch/arm64/kernel/patching.c`,
`arch/riscv/kernel/patch.c` (fixmap-based per-page alias
precedent),
`arch/x86/kernel/alternative.c:2503..2637` (`text_poke_mm`,
the dedicated-mm approach that UML cannot replicate),
`02-workstreams/C-profiles-and-gaps/05-port-ftrace.md` §Approach
commit 2, §Risk,
`02-workstreams/C-profiles-and-gaps/10-host-launcher-crosvm.md`
(host helper process that may enable future hardening).

---

## D29: C-05 uses `-fpatchable-function-entry=5,0`, not `-pg -mfentry`

**Date:** 2026-04-18
**Status:** Accepted (toolchain decision for C-05 design pass, revised
during pre-implementation verification)

**Decision:** UML's ftrace port compiles ftrace-instrumented TUs
with `-fpatchable-function-entry=5,0`, not `-pg -mfentry
-mrecord-mcount`. The arch Makefile (`arch/um/Makefile`) gains:

```make
ifeq ($(CONFIG_DYNAMIC_FTRACE),y)
  KBUILD_CPPFLAGS += -DCC_USING_PATCHABLE_FUNCTION_ENTRY
  CC_FLAGS_FTRACE := -fpatchable-function-entry=5,0
endif
```

and `arch/um/Kconfig` gains
`select FTRACE_MCOUNT_USE_PATCHABLE_FUNCTION_ENTRY if DYNAMIC_FTRACE`.
No `__fentry__` symbol, no `scripts/recordmcount` post-processing,
no `-pg`.

**Reasoning:**

UML unconditionally sets `-mcmodel=large` on 64-bit builds
(`arch/um/Makefile:33..35`). Pre-implementation verification
(2026-04-18) compiled a trivial translation unit with each
combination and inspected the emitted prologue:

| Compile flags                                  | Function prologue bytes |
|------------------------------------------------|-------------------------|
| `-mcmodel=small -pg -mfentry -mrecord-mcount`  | endbr64 + 5-byte `e8` CALL (recordmcount-compatible) |
| `-mcmodel=medium -pg -mfentry -mrecord-mcount` | endbr64 + 5-byte `e8` CALL (recordmcount-compatible) |
| `-mcmodel=large  -pg -mfentry -mrecord-mcount` | endbr64 + 33-byte `movabs/lea/add/movabs/add/call *%r10` (**indirect call; recordmcount cannot find the site**) |
| `-mcmodel=kernel -pg -mfentry -mrecord-mcount` | **cc1 error: "code model kernel does not support PIC mode"** (UML is PIE) |
| `-mcmodel=large  -fpatchable-function-entry=5,0` | **endbr64 + 5-byte NOP; `__patchable_function_entries` section populated** |

The `-mfentry` path is therefore structurally broken for UML
given the current `-mcmodel=large` global flag. Removing
`-mcmodel=large` cross-cuts UML's addressing model (the reason
for the large model is UML's nonstandard text layout — removing
it was not investigated under C-05 and would expand scope
materially). `-fpatchable-function-entry=5,0` works unchanged
under `-mcmodel=large`: the 5 bytes of NOP at function entry
are pure data (no relocation), and the
`__patchable_function_entries` section records absolute
addresses without depending on the call encoding.

**Second benefit: Clang parity.** Prior research (see D27's
upstream-precedent footnote and the web-search prior-art pass)
documented that Clang rejects `-mrecord-mcount` on x86_64 (the
flag is SystemZ-only in LLVM per review D71627). The
`FTRACE_MCOUNT_USE_CC` Kconfig probe would therefore fail under
`LLVM=1`, forcing a fallthrough to objtool or legacy
`recordmcount`. `-fpatchable-function-entry` is supported
identically by GCC ≥ 8 and Clang ≥ 10 on x86_64, giving a
uniform toolchain path.

**Alternatives considered:**

1. **Remove `-mcmodel=large` from ftrace-instrumented TUs only**
   (per-TU CFLAGS_REMOVE / CFLAGS_override). Rejected — needs
   a UML mm-model audit to prove every ftrace-able TU is safe
   under `-mcmodel=medium` (or small); affects thousands of
   TUs; wide CFLAGS surgery; hard to reverse if a corner case
   surfaces. Compiler-flag decisions of this scope belong in a
   dedicated change, not C-05.
2. **Remove `-mcmodel=large` globally.** Rejected for the same
   reason, amplified — a cross-cutting change to UML's
   addressing model is a separate workstream.
3. **Keep `-mfentry` and post-process to patch the indirect
   call sequence to a nop-equivalent.** Rejected — reinventing
   recordmcount for a non-standard instruction form, unused by
   any upstream arch; maintenance burden with no upside.
4. **`-fpatchable-function-entry=5,0` (this decision).**
   Accepted.

**Upstream precedent:** Three architectures already wire ftrace
this way — the arm64, riscv, and parisc patterns (cited below)
are the closest analogues to UML's new configuration. Each
overrides `CC_FLAGS_FTRACE` in its arch Makefile and selects
`FTRACE_MCOUNT_USE_PATCHABLE_FUNCTION_ENTRY` in its Kconfig:

- `arch/arm64/Makefile:142,145` + `arch/arm64/Kconfig:195`
- `arch/riscv/Makefile:18,20` + `arch/riscv/Kconfig:103`
- `arch/parisc/Makefile:77` + `arch/parisc/Kconfig:89`

UML joins that club. Reviewers with ftrace-on-arm64 background
will recognize the Makefile stanza immediately; that is part of
the upstreamability argument per `05-validation/upstream-strategy.md`.

**Interaction with D28:** D28's patching mechanism is unchanged.
D28 specifies patching 5 bytes under `text_mutex` + `stop_machine`
through `um_kernel_text_patch_begin/end`. Whether the source of
those 5 bytes is "compiler-emitted NOP5" (D29) or "GCC-emitted
CALL patched to NOP5 by recordmcount" (pre-D29 plan) does not
change the runtime patch mechanism at all. The target of the
CALL differs: under D29 it resolves directly to `ftrace_caller`
(no `__fentry__` indirection), which simplifies
`arch/um/kernel/mcount.S`.

**Revisit triggers:**

- If a future UML change removes `-mcmodel=large` globally
  (because the mm-layout rationale for it goes away), the
  `-mfentry` path becomes viable again. D29 would not
  automatically flip back — `-fpatchable-function-entry` is the
  modern, simpler path and upstream is migrating arches onto
  it, not off it.
- If `-fpatchable-function-entry` develops a UML-breaking
  regression in a future GCC/Clang, fall back to a recordmcount
  path that accepts the large-model indirect-call sequence
  (write a UML-specific scanner, paralleling `scripts/recordmcount.c`).

**Cross-reference:**
`arch/um/Makefile:33..35` (source of `-mcmodel=large`),
`arch/arm64/Makefile:142,145`, `arch/arm64/Kconfig:195`,
`arch/riscv/Makefile:18,20`, `arch/riscv/Kconfig:103`,
`arch/parisc/Makefile:77`, `arch/parisc/Kconfig:89`,
`kernel/trace/Kconfig:877..884`
(FTRACE_MCOUNT_USE_PATCHABLE_FUNCTION_ENTRY),
`reviews.llvm.org/D71627` (Clang `-mrecord-mcount` is SystemZ-only),
`Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/05-port-ftrace.md`
§Approach.

---

## D30: UML ftrace uses bulk mprotect inside stop_machine; per-site path kept for standalone callers

**Date:** 2026-04-19 (pragmatic refinement of D28 after boot
testing uncovered VMA fragmentation)
**Status:** Accepted (supersedes D28's per-site mprotect mechanism
for the ftrace batch path; D28's per-site semantics remain correct
for standalone single-site callers — the helper pair stays in the
B-04 API for future use)

**Decision:** The ftrace batch path in
`arch_ftrace_update_code()` does a **bulk** mprotect on
`[_text, _etext)` once inside the `stop_machine_cpuslocked()`
callback, calls `ftrace_modify_all_code()` which iterates every
record and invokes per-record `ftrace_make_call` /
`ftrace_make_nop` — those helpers now do a plain 5-byte
`memcpy` with no mprotect of their own — then bulk mprotects back
to RX. Two mprotect syscalls per enable/disable regardless of
record count.

In addition, `ftrace_init_nop` is a no-op: under
`-fpatchable-function-entry=5,0` the compiler-emitted NOP5 is
already in place at every traced function's entry, so init
has nothing to write.

The `um_kernel_text_patch_begin/end` helpers (B-04 extension)
still exist and still validate single-or-two-page ranges within
`[_text, _etext)`. They are the right surface for future
single-site callers (kprobes, live-patch, standalone one-off
patches) where taking the bulk hit is wrong. ftrace's batch path
is the outlier where N patches per operation make per-site
mprotect prohibitive.

**Reasoning (what testing surfaced):**

Per-site mprotect under the original D28 mechanism turned one
"enable function tracer" call into `2 * N` mprotect syscalls,
where N = `__mcount_loc` size (21275 on research profile with
GCC; 19592 with clang). Each mprotect on a different address
range splits the host-process VMA covering kernel text. After N
such operations the host process holds `~N` VMAs for one
formerly-contiguous region. VMA operations slow O(log N) per
operation, total O(N²) — verified empirically: research-profile
first-enable hangs for 5+ minutes, boot never reaches the
tracer-ready state.

x86 does not hit this because `text_poke_mm` uses one PTE in a
dedicated mm; arm/arm64/riscv do not hit this because fixmap
reuses one virtual slot. Both are single-mapping-slot designs.
UML's "use the process's mprotect" has no analogue slot, so
bulk-or-bust is the only practical design.

`ftrace_init_nop` as a no-op eliminates an earlier boot-time hang
from the same cause: `ftrace_process_locs()` calling
`ftrace_make_nop()` per record during init produced the same
42000-syscall avalanche before the tracer was ever enabled.

**Security envelope (what D28 said vs what D30 delivers):**

D28's "no other UML kernel code runs during the RW window" is
preserved — `stop_machine_cpuslocked()` guarantees it. The change
is that the window covers all of `[_text, _etext)` for the
duration of the batch, not one page per patch. Given that
`stop_machine` has frozen every peer kernel thread, the scope of
the RW window does not change what code can write to text: no
code is running except the one patching CPU. The D28 security
argument stands; only the implementation tactic changed.

**Alternatives considered:**

1. Keep per-site mprotect and hope VMA operations stay fast.
   Rejected — verified hang on research profile.
2. Batch per-page rather than per-site. Rejected — still O(pages
   in text) ≈ 6000 syscalls, still fragmenting.
3. Use a pre-allocated "patching alias mm" à la x86
   `text_poke_mm`. Rejected for this task as out of scope —
   would require building a UML-side equivalent of
   `mm_alloc()` + PTE aliasing, which the host process's mm
   model does not naturally support. Revisit if C-10 (host
   helper process, crosvm-style) lands — that's the place to
   build a real alias mechanism if we want one.
4. mprotect all of text permanently RWX at boot. Rejected —
   defeats the point of `.text` being RX.

**Known limitation (deferred follow-up):**

Research profile's `CONFIG_FUNCTION_TRACER=y +
CONFIG_DYNAMIC_FTRACE=y` still hangs on first `echo function >
current_tracer` — inside `ftrace_modify_all_code()`, after our
bulk mprotect completes. Not the VMA-fragmentation hang (which
D30 fixed); something in the research profile's debug surface
(KASAN + UBSAN + KFENCE + PROVE_LOCKING + DEBUG_PAGEALLOC +
DEBUG_OBJECTS) interacts poorly with the generic
`ftrace_modify_all_code()` iteration under UML's UP
`stop_machine` (which falls through to `local_irq_save`).

Minimal-config `CONFIG_FUNCTION_TRACER=y` works correctly:
verified 51293 trace lines captured from a basic `ls /` run.
Commit 3 of the C-05 series (enable tracer in research profile)
is therefore **deferred** pending root-cause analysis of the
research-specific hang. The ftrace port itself is correct; the
research-profile interaction is a separate bug to investigate.

**Revisit triggers:**

- If a UML C-10 host helper lands, reconsider alias-based
  patching (removes the "bulk RW in primary mm" property).
- Once the research-profile hang is root-caused, commit 3 of
  C-05 can land with whichever debug feature the investigation
  identifies as the culprit gated or reported upstream.

**Cross-reference:**
`arch/um/kernel/ftrace.c`
(`um_ftrace_update_code_cb`, `ftrace_init_nop`),
`arch/um/include/asm/ftrace.h`
(`ftrace_init_nop` arch-override),
`arch/um/kernel/section_split.c`
(per-site helpers kept for standalone callers),
`04-risks/decisions-log.md` §D28 (original per-site spec,
partially superseded), §D27 (graph deferred, independent),
§D29 (toolchain choice, independent),
`02-workstreams/C-profiles-and-gaps/05-port-ftrace.md` §Approach.

---

## D31: KCOV × FUNCTION_TRACER compounds on UML-UP; defer fix, profile-separate for now

**Date:** 2026-04-19
**Status:** Deferred (documented so a future engineer returning to this
can pick up where we left off without re-discovering the bisect)

**Decision:** Do not ship an upstream or in-tree fix for the
interaction between `CONFIG_KCOV=y` and `CONFIG_FUNCTION_TRACER=y`
on UML-UP. Instead, the `research` profile disables KCOV (it
already has the tracer + sanitizers for observability), and
`fuzz` / `fuzz-deep` remain the profiles that want KCOV (they
don't enable FUNCTION_TRACER — fast reboots beat observability in
fuzz-loop contexts). This is consistent with the a-plus-quality
anti-pattern #1 ("don't chase all checks in one image").

**The reproducer (preserve for future debugging):**

Host: Ubuntu 25.10, gcc 15.2, clang 21.1.8, Linux 7.0.0-13-generic.
Tree: this branch post `81779620ea8a` (ftrace port + bulk
mprotect).

Hanging config:

```
make ARCH=um uml/research
scripts/config --file .config -e FUNCTION_TRACER -e DYNAMIC_FTRACE
make ARCH=um olddefconfig
make ARCH=um -j"$(nproc)"
./linux mem=512M rootfstype=hostfs rootflags=/ rw \
        init=/tmp/ftrace-test.sh panic=1 console=tty
```

where `/tmp/ftrace-test.sh`:

```sh
#!/bin/sh
mount -t tracefs none /sys/kernel/tracing
echo function > /sys/kernel/tracing/current_tracer   # <-- hangs 5+ min
```

Symptom: boot reaches userspace, writes the `current_tracer` file,
kernel enters `ftrace_modify_all_code()` inside
`stop_machine_cpuslocked()` with IRQs disabled (UML UP:
`local_irq_save` ≡ `block_signals`), and stays there > 5 minutes
with no progress. No ftrace_bug, no WARNING, no panic — just
the kernel thread pegged at 99% CPU on the host process.

Minimal config (defconfig + FUNCTION_TRACER + DYNAMIC_FTRACE)
works in milliseconds: 51228 trace lines captured from
`ls /`, tracer on+off cleanly.

**Bisect matrix (which configs hang, which don't):**

| Config                                              | `echo function > current_tracer` |
|-----------------------------------------------------|----------------------------------|
| defconfig + FUNCTION_TRACER                         | ~1 s ✅ 51228 lines              |
| defconfig + FUNCTION_TRACER + DEBUG_OBJECTS         | ~1 s ✅ 51217 lines              |
| defconfig + FUNCTION_TRACER + KASAN                 | ~1 s ✅ 51248 lines              |
| defconfig + FUNCTION_TRACER + PROVE_LOCKING         | ~1 s ✅ 51261 lines              |
| defconfig + FUNCTION_TRACER + DEBUG_VM              | ~1 s ✅                           |
| defconfig + FUNCTION_TRACER + DEBUG_PAGEALLOC       | ~1 s ✅ 51297 lines              |
| defconfig + FUNCTION_TRACER + KFENCE                | ~1 s ✅ 51209 lines              |
| defconfig + FUNCTION_TRACER + MODULES               | ~1 s ✅ 51309 lines              |
| defconfig + FUNCTION_TRACER + SECCOMP_ONLY backend  | ~1 s ✅ 51281 lines              |
| defconfig + FUNCTION_TRACER + all three debug blocks (KASAN + UBSAN + KFENCE + PROVE_LOCKING + DEBUG_SPINLOCK + DEBUG_MUTEXES + DEBUG_ATOMIC_SLEEP + DEBUG_VM + DEBUG_PAGEALLOC + DEBUG_OBJECTS + DEBUG_OBJECTS_RCU_HEAD) | ~1 s ✅ 51268 lines              |
| base_defconfig + FUNCTION_TRACER + DEBUG_KERNEL     | ~1 s ✅ 51293 lines              |
| **research** (full)                                 | **hangs 5+ min ❌**              |
| research minus CC_OPTIMIZE_FOR_DEBUGGING + UML_TIME_TRAVEL_SUPPORT | hangs 5+ min ❌ |
| **research minus KCOV (just KCOV_ENABLE_COMPARISONS on or off)** | **~1 s ✅ 51225 lines** |
| research minus KCOV_ENABLE_COMPARISONS (KCOV still on) | hangs 5+ min ❌              |

Conclusion: the sole necessary ingredient on top of the already-
hefty debug surface is **plain `CONFIG_KCOV=y` (PC-trace)**. Once
`CONFIG_KCOV=n`, research's full debug surface + the function
tracer enable path completes in normal time.

**Root-cause narrative:**

`ftrace_modify_all_code()` runs under `stop_machine_cpuslocked()`
and iterates ~21000 `dyn_ftrace` records, for each calling
`ftrace_update_record()` → `__ftrace_replace_code()` → our
`ftrace_make_call()` / `ftrace_make_nop()`. Per record it touches
on the order of tens of internal function calls in kernel/trace/
ftrace.c and its inlined helpers.

Under `CONFIG_KCOV=y`, the compiler emits `call
__sanitizer_cov_trace_pc` at the start of every basic block of
every function in `kernel/trace/ftrace.c` (the tracer's source
file is not excluded from KCOV — see "Why we didn't upstream a fix"
below). Each `__sanitizer_cov_trace_pc` call is cheap when no task
has KCOV enabled (early-exit on `current->kcov_mode`), but cheap ≠
free. On top of that:

- Under `CONFIG_KASAN=y`, each `current->kcov_mode` load goes
  through a KASAN shadow-memory check.
- Under `CONFIG_PROVE_LOCKING=y` the bookkeeping around any
  per-basic-block return path adds instructions.
- Under UML UP, `stop_machine_cpuslocked` falls through to
  `local_irq_save` (which UML translates to `block_signals`),
  so no SIGALRM tick advances — we can't break up the batch with
  `cond_resched()`.
- Bulk mprotect (D30) already made the batch a single pair of
  syscalls around ~21000 memcpy. That's not the bottleneck; the
  bottleneck is what happens between the two mprotects.

21000 records × ~50 instrumented function calls per record × the
compounded per-call overhead (KCOV + KASAN shadow + lockdep
bookkeeping) = empirically > 5 minutes on this host, with no
cond_resched point.

On x86 SMP bare-metal with the same configs, the overhead exists
but is hidden behind parallel CPU throughput and is "slow" rather
than "hang." That's why this has been latent in syzkaller fleets
for ~9 years without complaint — real hardware absorbs it.

**Why we didn't upstream a fix (2026-04-19 review):**

Initial reflex (before researching): add `KCOV_INSTRUMENT := n`
to `kernel/trace/Makefile` next to the existing `KCSAN_SANITIZE
:= n`. Review by the project owner rejected this framing for
three correct reasons:

1. **KCSAN exclusion ≠ KCOV exclusion.** The comment "Avoid
   recursion due to instrumentation" on `KCSAN_SANITIZE := n`
   applies to KCSAN's genuine reentrancy hazards in ftrace's
   recursion-lock paths. KCOV has no analogous reentrancy — its
   recursion was already solved years ago from the KCOV side
   with `notrace` on `__sanitizer_cov_trace_pc`,
   `check_kcov_mode`, `canonicalize_ip`, `write_comp_data` (see
   Anders Roxell, "kcov: Don't trace the code coverage code",
   `https://lkml.iu.edu/hypermail/linux/kernel/1811.0/04706.html`).
   Framing a patch as "mirror the KCSAN exclusion" would land
   badly with a reviewer who knows the KCOV history (Vyukov
   definitely does).

2. **KCOV is coverage-maximizing by design.** Vyukov's original
   commit `33787098ffc3` sets `KCOV_INSTRUMENT_ALL=y` and opts
   out only where coverage is *meaningless* (early boot, VDSO,
   idle loops) — not merely where it's expensive. `ftrace.c`
   coverage *is* meaningful: syzkaller reaches it via `tracefs`,
   `perf_event_open`, `kprobe_events`. Blanket exclusion narrows
   the fuzz surface for a benefit that only UML-UP observes
   dramatically.

3. **No upstream complaint history.** 9+ years of syzkaller
   running `CONFIG_KCOV=y` + `CONFIG_FUNCTION_TRACER=y` on real
   hardware with zero LKML thread about the compounding cost.
   That's weak evidence of "it's not a kernel-wide problem,"
   strong evidence of "Rostedt will correctly point at the UML
   config."

**Three angles for later engineering:**

Preserving the analysis so a future session can pick up without
re-doing the research:

- **Angle 0: Bug report.** Write a mail to linux-trace-kernel +
  kasan-dev + Dvyukov describing the UML-UP timing and the
  instrumentation compounding, *asking* "is there a targeted
  workaround you'd accept" rather than proposing a patch. Low
  cost; high information-yield if the maintainers respond;
  maintainer-blessed scope for subsequent patch work.

- **Angle 1: `__no_sanitize_coverage` on ftrace hot-loop
  helpers.** Per-function attribute (GCC ≥ 10, Clang supported)
  on `ftrace_replace_code`, `ftrace_update_record`,
  `__ftrace_replace_code`, and the 2–3 other inner-loop helpers
  that drive the 21k-iteration batch. Argument: "coverage under
  `stop_machine` is structurally meaningless because no userspace
  fuzzer context is observing new PCs during the freeze window."
  Small patch (5ish annotations) but requires x86 SMP perf
  measurement to justify — this host can't provide that.
  Vyukov's counter may be "the coverage is still recorded and
  observed on the next KCOV_DISABLE read," which is technically
  true but weakly relevant to the 5+ minute UML hang.

- **Angle 2: Per-CPU static-key KCOV soft-off.** Rewrite
  `__sanitizer_cov_trace_pc`'s opening check to use a per-CPU
  static key rather than a `current->kcov_mode` load. Goal:
  ~1 cycle when KCOV is compiled in but no task has enabled it,
  vs ~10+ cycles today. Generic upstream contribution, not
  UML-specific. Requires ~50–200 LOC in `kernel/kcov.c` plus
  careful x86 SMP benchmarks (e.g., `stress-ng --switch 0` with
  KCOV=y vs KCOV=n). This is a week-scope project, not a
  fresh-context one-shot. Worth doing separately from the UML
  redesign if someone wants a real upstream contribution.

**Semantic argument worth reusing** (shows up in angle 1 and
angle 2): coverage collection inside a `stop_machine` callback
has no live observer. Syzkaller's fuzzer thread is frozen
alongside every other userspace thread; the kernel is running one
CPU's callback. Coverage samples collected in that window can be
read after the freeze lifts, but by then the instrumentation
target (a batch-text-patching loop) has completed and the fuzzer's
decision point has moved on. The instrumentation cost is paid; the
information value is near zero.

**Revisit triggers:**

- When a future UML engineer or syzkaller user file an LKML bug
  about similar compounding on x86 (likely eventually — the issue
  is real, just hidden by SMP throughput).
- If someone wants a Real upstream Kernel Contribution from this
  project, angle 2 is the right-sized target.
- When C-10 (host launcher) lands, revisit whether the patch can
  move to the helper process — removing the instrumentation
  concern entirely because ftrace.c isn't running in the main UML
  process anymore.

**Cross-reference:**

- `kernel/trace/Makefile` (existing `KCSAN_SANITIZE := n`, the
  anchor for angle-1's reasoning; not itself being modified).
- `kernel/kcov.c` (the target of angle 2).
- `33787098ffc3` "kernel: add kcov code coverage" (Vyukov,
  Kconfig + semantic stance).
- Anders Roxell, "kcov: Don't trace the code coverage code"
  (`notrace` on kcov hooks — the prior art that shows KCOV
  recursion is already solved and my initial "recursion bomb"
  framing was wrong).
- `02-workstreams/C-profiles-and-gaps/05-port-ftrace.md`
  §Approach commit 3 (the resolution in-tree: research disables
  KCOV).
- §D30 (bulk mprotect — the *previous* hang, which this one
  supersedes as the current research-profile blocker).

---

## D32: C-04 ships the full kprobes surface; supersedes D27 with empirical validation

**Date:** 2026-04-19
**Status:** Accepted (scope decision for C-04, supersedes D27's
function_graph deferral pending commit 7's stress test result)

**Decision:** C-04 (port kprobes to UML) delivers the full
user-visible dynamic-instrumentation surface in one workstream:

- `HAVE_KPROBES` via int3 + single-step (entry + mid-function
  probes, both through the int3 path)
- `HAVE_KRETPROBES` via a return-address-rewrite trampoline
  (return probes)
- `HAVE_FUNCTION_GRAPH_TRACER` via the same return-trampoline
  pattern (call graphs)

**Out of scope (follow-up task "C-04b"):**
`HAVE_KPROBES_ON_FTRACE` — the ftrace fast path for function-
entry kprobes. Would need `HAVE_DYNAMIC_FTRACE_WITH_REGS`, which
means a new `ftrace_regs_caller` in mcount.S that saves a full
UML pt_regs frame on every traced function's entry. ~200 LOC of
delicate assembly that adds risk to C-05's working ftrace without
providing new user-visible capability — int3 kprobes covers the
same function-entry use case at the cost of ~400 ns per probe
hit (trap + single-step overhead vs ftrace's ~50 ns dispatch).
On the research profile (~14× native baseline already), the
difference is imperceptible. Per a-plus-quality-plan anti-
pattern #3 ("optimize before workloads exist"), the ftrace fast
path is a Q4-style tuning task, not a C-04 prerequisite. Defer
until profiling shows it matters.

As part of the same series, D27's deferral of function_graph
(from the C-05 workstream) is lifted. An empirical validation —
`tools/testing/selftests/um/kprobes-stress/` — runs kretprobes +
function_graph under SIGALRM pressure for 10 000 iterations and
asserts no lockdep splat / no KASAN report / no kernel oops. If
that test passes, D27's theoretical race concern is superseded
by observed correctness. If it fails, the specific failure mode
becomes a narrow follow-up.

**Reasoning:**

Three inputs drove this scope up from the earlier phased
proposals (Option B: ftrace-only; Option D: int3 + ftrace,
defer kretprobes):

1. **Vision alignment.** `00-vision.md` lists kprobes and ftrace
   together in "full instrumentation," and the success criterion
   is `make ARCH=um uml/research` producing a kernel with "all
   sanitizers and tracing on" that reproduces a syzbot CVE in
   <30 s. Shipping kprobes without kretprobes — "I can trace
   every call but not returns" — is a visible gap against that
   target. bpftrace's `kretprobe:` / `uretprobe:` and perf's
   `--add probe=foo%return` are standard tools in the debugging
   workflow.

2. **Architectural research cleared the risks.** Reading the UML
   signal infrastructure on 2026-04-19 produced three green-
   lights (see `02-workstreams/C-profiles-and-gaps/
   04-port-kprobes.md` §"Architectural research"):
   - `arch/um/kernel/trap.c:417` (`relay_signal()`) has a clean
     kernel-mode-SIGTRAP hook point already — just needs a
     kprobe_int3_handler dispatch before the existing panic.
   - `arch/x86/um/os-Linux/mcontext.c:81..84` already
     propagates `X86_EFLAGS_TF` through the mcontext ↔ regs
     roundtrip via an existing `single_stepping` mechanism; no
     new single-step emulation work needed.
   - Livepatch already rewrites return addresses on UML kernel
     stacks without signal-race issues; the D27 concern about
     return_to_handler racing SIGALRM was theoretical rather
     than observed. UML's SIGALRM delivery
     (`arch/um/os-Linux/signal.c:131..150`) runs in host
     context against a captured mcontext and writes modified
     registers back via `set_stub_state()`, without racy
     mutation of the guest kernel stack mid-trampoline.

3. **Window pressure (per `00-vision.md`).** "The window is open
   now and is not large. Five years from now, either UML has
   caught up or LKL + Rust VMM crates + crosvm + eBPF together
   have eaten the niche." The incremental path (B → D →
   A → lift-D27, across multiple sessions with C-04 carrying
   "partially complete" status) burns calendar time on
   conservatism that the architectural research showed was
   unwarranted. Ship more, phase less.

**On D27 specifically (function_graph deferral from C-05):**

D27 was written on 2026-04-18 during the C-05 design pass. It
cited signal-race risk between the function_graph return
trampoline and UML's SIGALRM-delivered preemption. At the time,
I had not read the UML signal-handling code in depth; the
deferral was precautionary. The architectural research on
2026-04-19 (informing this D32) showed the race is theoretical
and that livepatch already does comparable return-address
rewriting on UML without incident. D27 is therefore not wrong
in spirit — "a stress test is good hygiene" — but the
"defer until test exists" framing should have been "build the
test alongside the feature and validate empirically." Commit 7
in the C-04 series corrects this: it builds the test and
exercises graph + kretprobes under SIGALRM pressure. The test
result retires D27.

**Alternatives considered and why rejected:**

- **Option B: ship only HAVE_KPROBES_ON_FTRACE first.** Fastest
  (~1 week), safe, but leaves both mid-function probes and
  returns/graphs out. Research profile UX visibly incomplete
  against vision.
- **Option D: int3 + ftrace, defer kretprobes with D27.**
  Consistent with D27 but leaves the biggest user-value gap
  (return probes). Consistency is the wrong target when the
  consistent position is itself empirically unvalidated.
- **Full scope but keep D27 theoretical deferral (no stress
  test).** Defers the question indefinitely. The right move is
  to decide based on data; commit 7 generates the data.
- **Split kretprobes from function_graph: ship kretprobes with
  kprobes, defer function_graph via D27.** They share the
  trampoline pattern, so if one is safe the other is. Splitting
  is incoherent.

**Revisit triggers:**

- Commit 7's stress test fails: that specific failure gets a
  narrow fix (likely signal masking across a small window) or
  a narrow deferral with empirical evidence, not the broad
  D27-style deferral.
- A maintainer reviews the C-04 series and asks why UML ships
  full kprobes while other arches phase: the answer is D32 —
  we had the architectural research clearing the usual
  concerns, and the stress test backed it.
- C-10 host-launcher or D-* KVM-backend work changes the
  signal-delivery model: revisit the stress test under the new
  model.

**Cross-reference:**
- `02-workstreams/C-profiles-and-gaps/04-port-kprobes.md` (the
  task spec with the full slice plan).
- D27 — superseded by this entry pending commit 7 validation.
- D29 (toolchain choice — `-fpatchable-function-entry`),
  D30 (bulk mprotect) — C-05 decisions that C-04 builds on.
- `arch/um/kernel/trap.c:417` — the SIGTRAP hook site.
- `arch/x86/um/os-Linux/mcontext.c:81..84` — TF propagation
  source.
- `arch/x86/kernel/kprobes/{core.c,ftrace.c}` — primary
  reference implementation.

---

## D33: C-04 commit 2 implements rethook, not the legacy kretprobe_trampoline

**Date:** 2026-04-19
**Status:** Accepted (supersedes the "kretprobe_trampoline" phrasing
in D32 and in the C-04 task spec's earlier draft of commit 2)

**Decision:** C-04 commit 2 selects `HAVE_RETHOOK` and ports
`arch/x86/kernel/rethook.c` to `arch/um/kernel/rethook.c`. It
does **not** implement the legacy
`arch_prepare_kretprobe` + `kretprobe_trampoline` pair that the
earlier draft of `04-port-kprobes.md` described.

**Why the pivot:**

1. **Upstream x86_64 has moved.** `arch/x86/kernel/kprobes/core.c`
   no longer contains a `kretprobe_trampoline`; the last legacy
   shape was removed years ago. The current x86_64 kretprobe
   implementation is a 128-line `arch/x86/kernel/rethook.c` file
   plus the generic core in `kernel/trace/rethook.c`. Mirroring
   x86 means porting rethook.

2. **`KRETPROBE_ON_RETHOOK` auto-activates.** `arch/Kconfig:252`:
   ```
   config KRETPROBE_ON_RETHOOK
       def_bool y
       depends on HAVE_RETHOOK
       depends on KRETPROBES
       select RETHOOK
   ```
   Selecting `HAVE_RETHOOK` is the whole contract. `HAVE_KRETPROBES`
   on the UML Kconfig line becomes unnecessary and is dropped.

3. **Less assembly, cleaner contract.** Rethook is four ops:
   `arch_rethook_trampoline` (asm), `arch_rethook_trampoline_callback`
   (C), `arch_rethook_prepare` (C), `arch_rethook_fixup_return`
   (C). The generic `rethook_trampoline_handler()` in
   `kernel/trace/rethook.c` drives the shadow-stack walk and
   handler dispatch. The legacy trampoline had to duplicate
   that logic per-arch.

4. **UML pt_regs is byte-compatible with the x86 rethook
   trampoline.** The trampoline builds a pt_regs image on the
   stack using x86-native field ordering (R15 … SS). UML's
   `struct pt_regs` wraps `struct uml_pt_regs` whose `gp[]`
   indexing (`HOST_IP=16`, `HOST_SP=19`, `HOST_EFLAGS=18`, …)
   is derived from the host's `struct user_regs_struct` and
   matches that same ordering for every field the trampoline
   reads or writes. This is why the port is copy-paste plus an
   accessor swap (`regs->ip` → `UPT_IP(&regs->regs)`, etc.)
   rather than a ground-up redesign.

**What the earlier D32 phrasing implied (now superseded):**

D32 says "`HAVE_KRETPROBES` via a return-address-rewrite
trampoline (return probes)." The *user-visible* effect is
unchanged — kretprobes work, kernel_clone return values get
reported, bpftrace's `kretprobe:` works. The internal contract
shifts from per-arch to generic-via-rethook. Commit 2's task
spec (`02-workstreams/C-profiles-and-gaps/04-port-kprobes.md`
§"Commit 2") now describes the rethook port explicitly.

**Alternatives considered and why rejected:**

- **Implement the legacy `kretprobe_trampoline` / `arch_prepare_kretprobe`
  anyway for symmetry with s390 / arm64.** Rejected: both of
  those arches also provide rethook paths (s390 ships both; arm64
  has moved entirely to rethook). Nothing in the UML plan
  benefits from the legacy path. Would be dead code the moment
  it landed.
- **Skip kretprobes for now; re-land commit 2 post-C-05b.**
  Rejected: D32 is already the scope-expansion decision. Walking
  that back on discovering rethook is easier than the legacy
  path would be self-defeating.
- **Port rethook to `arch/um/kernel/kprobes/rethook.c` so it
  lives under the kprobes subdirectory.** Rejected: rethook is
  a generic return-hooking framework used by fprobe and
  (optionally) kprobes, not a kprobes subcomponent. Its home on
  every other arch is `arch/<arch>/kernel/rethook.c`; follow
  that convention so grep discovery and file-reviewer
  expectations hold.

**Revisit triggers:**

- Upstream revives a per-arch legacy kretprobe path (unlikely;
  the trend is the other direction).
- UML grows a KVM backend (workstream D) and its pt_regs
  layering differs from the ptrace/seccomp backends — may
  require a kind-indexed rethook dispatch.
- FINEIBT / IBT / kCFI lands for UML (currently a no-op on UML;
  the `ANNOTATE_NOENDBR` in the trampoline stays harmless).

**Cross-references:**

- `02-workstreams/C-profiles-and-gaps/04-port-kprobes.md`
  §"Commit 2 — Kretprobes via rethook (revised 2026-04-19)".
- `arch/x86/kernel/rethook.c` — the reference implementation.
- `arch/s390/kernel/rethook.c` — shortest rethook port; useful
  sanity check for how minimal the callback can be.
- `arch/Kconfig:248..256` — `KRETPROBES` + `KRETPROBE_ON_RETHOOK`
  selection logic.
- `include/linux/rethook.h` — the four-op arch contract.
- `kernel/trace/rethook.c` — generic shadow-stack driver.
- D32 — the scope decision this refines.

---

## D34: C-04 commit 3 (HAVE_FUNCTION_GRAPH_TRACER) deferred — generic fgraph assumptions conflict with UML's execution model

**Date:** 2026-04-19 (findings); deferral decision 2026-04-20
**Status:** Accepted (narrow-scope retreat from D32's commit-3 deliverable;
commit 3 held out of C-04 until the blocker below is resolved upstream
or worked around end-to-end)

**Decision:** C-04 ships **commits 1a–1d + commit 2 (kretprobes via
rethook)** only. `HAVE_FUNCTION_GRAPH_TRACER` is not selected. Commit 3
as described in D32 / `04-port-kprobes.md` stays `planned` and blocked
on #33 + #34 + a new generic-kernel question captured below. The
downstream commits that depended on commit 3 are adjusted:

- Commit 4 (research profile): enables kprobes, kretprobes, function
  tracer — **not** function_graph.
- Commit 5 (kprobes-stress selftest): reframed as the harness that
  would have validated commit 3; useful on its own for kretprobes
  regressions and kept on the critical path.
- Commit 6 (docs + landed status): lands with a §"Function graph —
  deferred" subsection pointing at this decision.

**Why commit 3 cannot land as-designed:**

The generic function_graph trampoline (`kernel/trace/fgraph.c`) assumes
every traced function return executes via a `ret` that pops the
graph-rewritten parent slot. On native x86 this holds — including
across `__switch_to`, because the switch restores %rsp such that the
outgoing task's ret resumes at a call site with the trampoline's
rewritten slot still in place. On UML it does not, for three distinct
reasons discovered empirically over a debugging session on
2026-04-19:

1. **UML signal dispatch uses `rt_sigreturn`, not `ret`.** The host-
   delivered signal handler (`hard_handler` in
   `arch/um/os-Linux/signal.c`) and its downstream (`sig_handler`,
   `timer_alarm_handler`, `sig_handler_common`, `block_signals_trace`,
   …) runs on the task's kernel stack. If any of them is graphed, its
   prepare_ftrace_return rewrites a parent slot on the signal frame,
   but that frame is torn down by the kernel's `rt_sigreturn` syscall
   rather than the function's normal `ret`. The shadow stack entry is
   never popped. One leak per signal delivery; SIGALRM fires HZ times
   per second. Observed: `curr_ret_stack` climbing from 0 into the
   100s within seconds.

2. **UML enters new kernel tasks via `kernel_longjmp`.** `new_thread_handler`
   and `fork_handler` in `arch/um/kernel/process.c` are not reached
   via a `call`; they are the landing sites of a `UML_LONGJMP` during
   task creation. Their prologue prepare_ftrace_return rewrites
   whatever garbage happens to sit at `8(%rsp)` when the new task's
   kernel stack is first activated. That rewrite is never popped.
   One leak per task creation.

3. **Generic `kthread()` and `smpboot_thread_fn()` never return.**
   Both live in `kernel/kthread.c` / `kernel/smpboot.c`. Both end in
   `do_exit`. Neither can be stripped from arch/um/'s Makefiles
   (they're generic kernel code, not UML-specific). Graph push happens
   at entry; the corresponding pop never fires because the function
   doesn't return. One leak per kthread created (ksoftirqd, migration
   threads, kworkers). `ftrace_graph_exit_task` cleans up on
   `free_task`, but between `do_exit` and `free_task` the entry sits
   on the task's shadow stack as a "live" leaked push.

**What we tried and what it bought us:**

| Attempt | Result |
|---|---|
| Wrap `hard_handler` body with `atomic_inc/dec(&current->tracing_graph_pause)` so prepare_ftrace_return skips pushes during signal handling | Didn't help — hard_handler's own prologue push happens BEFORE the inc, so the signal-frame leak persisted |
| `ccflags-remove-y := $(CC_FLAGS_FTRACE)` in `arch/um/kernel/Makefile` and `arch/um/kernel/skas/Makefile` | Stripped patch sites from new_thread_handler, fork_handler, hard_handler (kernel-side glue), sched_clock, and all arch/um/kernel/ functions. Closes sources (1) and (2) |
| `USER_CFLAGS := $(filter-out $(CC_FLAGS_FTRACE),$(USER_CFLAGS))` in `arch/um/Makefile` | Closes USER_OBJS patch sites (os-Linux/signal.c, sigio.c, irq.c, skas/process.c). `CFLAGS_REMOVE_<file>.o` can't do this because `arch/um/scripts/Makefile.rules` overrides c_flags for USER_OBJS |
| Combined: all arch/um/ patch sites stripped | Reduced shadow-stack growth. Didn't eliminate crashes under workload. ksoftirqd eventually SEGVs in `schedule+0x4a` — reproducibly. The residual leak is reason (3). |

**Two concrete crashes from the last build (arch/um/ stripped, no
graph pause):**

- **Under fork+exec workload (`/bin/ls /etc`):** `Kernel panic - not
  syncing: Segfault with no mm`. RIP=`schedule+0x4a` (`mov (%rax), %rax`
  with RAX=0). Stack shows `return_to_handler+0` twice, `ftrace_graph_caller`,
  `__schedule`, `kthread_should_park`, `smpboot_thread_fn`, `kthread`,
  `new_thread_handler+0x48`. The crash address isn't a real instruction
  pointer — it's where a leaked `return_to_handler` pop landed.
- **Under `wc -l /sys/kernel/tracing/trace` with graph active:**
  `Kernel panic - not syncing: Kernel tried to access user memory at
  addr 0xb00000000`. Six `return_to_handler+0` entries on the stack.
  The trace-reading code (`print_graph_function` → `s_show` →
  `seq_read_iter`) walks a ret_stack that has leaked entries and
  dereferences what should have been a valid shadow-stack pointer.

Both crashes are the same root cause manifesting via different stack
walks.

**Alternatives considered for closing reason (3):**

1. **Generic-kernel annotation** — mark `kthread()` and
   `smpboot_thread_fn()` `notrace`. Would suppress the leak but leaves
   any user-written kthread-entry function that ends in `do_exit` to
   leak. Cross-subsystem change; would need tracing maintainer sign-off.
   Per AGENT-PROMPT "when to stop and ask," cross-subsystem changes need
   explicit user sign-off first — out of scope for commit 3's arch
   work.
2. **UML-local graph gate around do_exit** — arch/um/kernel/exit.c or
   equivalent could `atomic_inc(&current->tracing_graph_pause)` when
   `do_exit` is entered. Doesn't help because the push happened at
   kthread()'s entry, long before do_exit. The gate would need to
   retroactively un-push, which would require new generic API.
3. **Relax fgraph's pop-failure behavior** — today `__ftrace_return_to_handler`
   returns `(unsigned long)panic` on a failed pop (kernel/trace/fgraph.c:828).
   If it instead returned 0 (or the most recent valid ret on the
   stack), stale entries would at worst cause a skip, not a crash.
   Generic change; upstream-able but needs a real discussion with
   the tracing maintainers about what "failed pop" should mean.
4. **Per-task shadow-stack compaction** — on task yield, walk
   current->ret_stack and drop entries whose retp no longer matches
   the task's actual kernel stack. Expensive at every context switch.
5. **UML-specific fgraph implementation** — fork the generic
   trampoline semantics to tolerate longjmp + do_exit. Large
   engineering investment; probably not worth it compared to (1) or (3).
6. **Accept that commit 3 is not a one-commit task.** Ship what we
   have, defer graph until the blocker is addressed upstream.

Going with (6) for now. (1) and (3) stay open as the real paths to
unblocking commit 3 — either in a future workstream or via an
upstream discussion. Task #34 tracks the gcc/clang `notrace` +
patchable-function-entry ineffectiveness that caused us to reach for
Makefile workarounds; task #33 tracks the arch-level patch-site
audit (partially done in the stashed WIP); a new upstream-discussion
task would capture (1)/(3) once someone is ready to file the
tracing-maintainer RFC.

**What D32 promised and what we're shipping:**

D32 said C-04 would ship HAVE_KPROBES + HAVE_KRETPROBES +
HAVE_FUNCTION_GRAPH_TRACER as a unit, with commit 5's stress test
empirically validating the return-trampoline family. D32's
architectural research was right about the int3 kprobes path and the
rethook path — both landed clean. D32 was wrong about function_graph:
the research focused on D27's signal-race concern, which turned out to
be real but fixable via patch-site stripping; it missed the
generic-kernel leak (kthread/do_exit) because that's not a UML-specific
issue. This D34 corrects D32 narrowly: scope drops to
HAVE_KPROBES + HAVE_RETHOOK (commits 1 + 2 of the C-04 series), with
graph held.

**Revisit triggers:**

- A generic change to fgraph that tolerates pop failures (alternative 3
  above) lands upstream. The block to commit 3 disappears.
- A maintainer accepts `notrace` annotations on `kthread()` /
  `smpboot_thread_fn()` (alternative 1).
- Someone writes the UML-specific fgraph shim (alternative 5), at which
  point the commit 5 stress harness in task #24 becomes the regression
  guard.
- Toolchain (`notrace` + `-fpatchable-function-entry`) behavior
  improves so per-file stripping is less necessary; surfaces whether
  the leak from (1) and (2) alone is tolerable.

**Cross-references:**

- `02-workstreams/C-profiles-and-gaps/04-port-kprobes.md` §"Commit 3"
  (updated with the findings and the deferral).
- D32 — scope decision this narrows.
- D27 — original function_graph deferral; superseded by D32, then
  re-asserted-in-spirit by this D34.
- Task #22 (C-04 commit 3) — held `planned`, blocked by #33 + #34.
- Task #33 (signal-path patch-site audit) — partial work in the
  `c04-c3-wip-after-arch-strip-still-crashes` stash.
- Task #34 (notrace + patchable-function-entry workaround) —
  diagnosed: both gcc 15.2 and clang 21.1.8 leave patchable entries
  in place despite the attribute expansion.
- Task #24 (kprobes-stress selftest) — now the next critical-path
  task; its harness will validate any future graph-trampoline fix.
- `kernel/trace/fgraph.c:828` — the `return (unsigned long)panic`
  pop-failure behavior that alternative 3 would soften.
- `kernel/kthread.c`, `kernel/smpboot.c` — the generic kthread
  entrypoints that leak graph pushes.

---

## D35: C-09 v1 is a cooperative AFL-style forkserver; CRIU-style snapshot-to-disk deferred to v2

**Date:** 2026-04-20
**Status:** Accepted (closes `09-snapshot-forkserver.md`'s Q1/Q2/Q3
design questions; narrows the v1 scope of workstream C-09)

**Decision:** C-09 v1 achieves the fuzz profile's <50 ms restart
target by having UML boot to a named "ready point" and then
`fork()` itself for each fuzz iteration. Parent = forkserver
(pristine, quiesced). Child = worker that runs one testcase and
exits. The wire protocol is AFL-compatible (fds 198/199; 12 bytes
per iteration).

CRIU-style serialize-to-disk snapshot/restore is **not** in v1.
It stays parked for a later v2 that needs to survive host
reboots; the fuzz milestone (M8) does not need it.

**Why this shape (model selection):**

1. **UML is one cooperative process that owns its state.** The
   CRIU playbook — ptrace-injected parasite, pagemap walking,
   TCP-repair, mount-tree reconstruction, `/proc/self/fd`
   enumeration — exists because CRIU is dumping uncooperative
   targets from outside. UML already knows every host fd it
   opened (`os-Linux/file.c`), every mmap region (one RAM
   `mmap`, optional KASAN shadow `mmap`, vmalloc area), and
   every vCPU thread (it created them). So the useful
   machinery is tiny: a fork point, a post-fork re-init. The
   expensive CRIU machinery is dead weight.

2. **`fork()` gives COW of guest RAM for free.** A worker that
   touches 16 MB of guest RAM pays ~4k minor faults; 4k × ~2 µs
   ≈ 8 ms. Nyx (Schumilo et al., USENIX Sec '21, [link][nyx])
   reaches 1000+ iter/s in KVM by implementing dirty-page
   bitmaps and incremental restore from a userfaultfd-backed
   pages file — effectively reinventing `fork()`'s COW by
   hand because KVM doesn't expose host process fork semantics
   cleanly. UML inherits them for free by being a regular host
   process.

3. **The syzkaller API we're implementing is already the
   forkserver contract.** `Instance.SetupSnapshot(input)` +
   `Instance.RunSnapshot(input) -> (result, output, err)` in
   syzkaller's `pkg/vm/vmimpl` ([link][syz-snap]) maps
   precisely onto "boot to ready point once; per-iter feed
   testcase and wait for result". The contract is published;
   we bind onto it. No new syzkaller API negotiation is part
   of this.

4. **gVisor validated the architectural shape we need.**
   gVisor's checkpoint/restore owns its "kernel" (Sentry) and
   serializes directly without CRIU ([link][gvisor-cr]).
   gVisor's demand-faulted pages file is the model for UML's
   eventual v2; v1 omits the file because `fork()` suffices
   for in-process fuzzing.

**Alternatives considered and why rejected:**

- **Full CRIU-style snapshot-to-disk in v1.** Rejected: 3-4×
  the engineering effort for v1 value that only materializes
  on host reboot. The fuzz profile is a long-lived host
  process; 1000+ iter/s from a `fork()`-based forkserver
  already saturates host CPU. Snapshot-to-disk is a M8+ "nice
  to have".

- **AFL persistent mode (`__AFL_LOOP(N)`) inside the kernel.**
  Rejected: persistent mode reuses the same child process
  across iterations and relies on the harness to "fully reset
  critical state" between iters (AFL++ surfaces this as
  "stability %"). A running kernel has no defensible notion of
  "reset critical state" — timer ticks, RCU grace periods, and
  kthread work between iters all mutate non-harness state.
  Forkserver (fresh child per iter) is correct; persistent is
  a fuzz-only optimization whose correctness precondition the
  kernel does not satisfy.

- **Nyx-style in-kernel dirty-page bitmap + userfaultfd
  restore.** Rejected for v1: we don't have a UML KVM backend
  yet (workstream D), and replicating the KVM dirty-log
  mechanism against UML's seccomp backend is a substantial
  project on its own. Rebuilds what `fork()` already gives us.
  Revisit post-D if v1 forkserver proves insufficient.

- **mconsole-only trigger for the "ready point."** Rejected:
  `03-profiles/fuzz.md` explicitly sets `CONFIG_MCONSOLE=n`
  for the fuzz profile (mconsole is debug surface; fuzz wants
  minimum TCB). The fuzz profile is the *only* profile that
  needs the forkserver. An mconsole-only trigger would mean
  fuzz profile cannot use the feature C-09 is specifically
  for. We compose three channels (AFL fd handshake, debugfs
  trigger, mconsole command); fuzz profile gets the first,
  research gets the second, `prod-with-hooks` gets all three.

- **Snapshot at arbitrary points via stop_machine + serialize.**
  Rejected: the 50 ms budget cannot absorb the cost of
  serializing 16 TB of KASAN shadow VA + the RAM image, even
  sparsely. Cooperative ready-point with `fork()` sidesteps
  the entire serialize step.

**What v1 commits to:**

- `CONFIG_UM_FUZZ_HOOKS=y` gates compilation of
  `arch/um/kernel/snapshot.c`.
- Three trigger channels (AFL fds 198/199, debugfs, mconsole)
  composed through one `um_snapshot_ready(const char *point)`
  entry point.
- Post-fork `um_snapshot_worker_init()` handling the five
  things `fork()` does not inherit: vCPU pthreads, seccomp
  stub children, timerfd + signalfd, host-fd allowlist,
  pending signals.
- Selftest `tools/testing/selftests/um/snapshot-smoke/` speaks
  the AFL protocol to validate end-to-end without waiting on
  syzkaller's `vm/uml` backend (C-08) to land.
- Nothing committed to `tools/uml/snapshot/`; host-side CLI
  is deferred to a later workstream gated by explicit user
  sign-off per the AGENT-PROMPT cross-subsystem rule.

**Six named risks that v1 budgets for (each → one Open-question
in `09-snapshot-forkserver.md`):**

1. Post-fork page-fault storm (fault path dominates if guest
   working set > ~8 MB).
2. KASAN shadow COW amplification (1/8 of touched VA, scattered
   faults).
3. Stub-child respawn latency (~1–3 ms per stub; multiplies
   by N-vCPU × guest-userspace processes).
4. Host fd hygiene (any leaked fd is a correctness bomb or a
   cleanup-time sink; ~30 call sites to audit).
5. Reaping / pid contention at high rate (above ~500 iter/s).
6. Snapshot drift (AFL "stability < 100%" analogue; any
   parent-side mutation between forks).

Risk #6 is the engineering-effort dominator; commit 2 of C-09
is budgeted against it.

**Lifetime:** Until v1 ships and runs long enough to prove the
50 ms target is reachable. If post-fork fault storms (risk #1)
or stub respawn (risk #3) make 50 ms unreachable, revisit in
favor of the Nyx-like in-kernel dirty-log approach.

**Revisit triggers:**

- The fuzz profile reaches `<<50 ms` steady-state per
  iteration but adding new instrumentation (e.g., KMSAN in
  C-07) pushes it over budget. Re-examine whether snapshot-
  to-disk or Nyx-style incremental restore would be cheaper
  than shrinking instrumentation.
- A host-side use case materializes that genuinely needs
  survive-reboot snapshots (replay of a rare syzkaller
  reproducer across host reboots; CI stamp of a "golden boot
  state"). Open the v2 design thread with gVisor's pages-file
  + demand-fault model as the starting point.
- Workstream D (KVM backend) lands and the KVM backend exposes
  dirty-log cleanly. May be cheaper than the forkserver in the
  prod-with-hooks profile where KVM is the backend anyway.
- `CONFIG_MCONSOLE=n` changes in the fuzz profile (unlikely;
  it's a TCB decision). Would let us collapse the three
  trigger channels into one.

**Cross-references:**

- `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md`
  — the full v1 design.
- `03-profiles/fuzz.md` §"Kconfig fragment" line 46:
  `# CONFIG_MCONSOLE is not set` — the constraint that pins
  the AFL-fd trigger as the required channel.
- `04-risks/decisions-log.md` D1 — three-layer architecture
  the snapshot static key fits under (Layer 2).
- [AFL forkserver technical notes (lcamtuf)][afl-tech]
- [AFL++ `src/afl-forkserver.c`][afl-fs]
- [Nyx: Greybox Hypervisor Fuzzing (Schumilo et al., USENIX
  Sec '21)][nyx]
- [gVisor Checkpoint/Restore user guide][gvisor-cr]
- [syzkaller `pkg/vm` Snapshot API][syz-snap]
- [CRIU Checkpoint/Restore overview][criu-cr] — the playbook
  we are explicitly not using for v1.

[afl-tech]: https://github.com/google/AFL/blob/master/docs/technical_details.txt
[afl-fs]: https://github.com/AFLplusplus/AFLplusplus/blob/stable/src/afl-forkserver.c
[nyx]: https://www.usenix.org/system/files/sec21-schumilo.pdf
[gvisor-cr]: https://gvisor.dev/docs/user_guide/checkpoint_restore/
[syz-snap]: https://pkg.go.dev/github.com/google/syzkaller/vm
[criu-cr]: https://criu.org/Checkpoint/Restore

---

## D36: UML v2 snapshot file format is ELF64 core dump + UML `PT_NOTE` types

**Date:** 2026-04-20
**Status:** Accepted as the format choice for the parking-lot v2
design in `08-future-phases/02-snapshot-to-disk.md`. Does not
commit us to building v2; commits us to what v2 will look like
*if* its D35 revisit triggers fire.

**Decision:** When UML v2 snapshot-to-disk opens, the on-disk
format is an ELF64 core dump with:

- One `PT_LOAD` per enumerated kernel mmap region (guest
  `physmem_fd`-backed RAM, KASAN shadow, populated vmalloc
  runs, stub pages). Sparse representation via filesystem
  holes; never materialize zero regions.
- Standard ELF notes: `NT_PRSTATUS` (per-vCPU registers, via
  the backend ops table's `read_guest_regs`), `NT_FILE`,
  `NT_AUXV`.
- UML-specific `PT_NOTE` types under vendor name `"UML"` for
  state with no standard ELF home: `UML_VERSION`,
  `UML_BACKEND`, `UML_MMAP_TABLE`, `UML_FD_TABLE`,
  `UML_TIMERS`, `UML_STUB_TABLE`, `UML_READY_POINT`. Type
  numbers start at `0x554d4c00` (`'UML\0'`) + ordinal.

See `08-future-phases/02-snapshot-to-disk.md` §"Layout" for the
detailed schema.

**Alternatives considered and why rejected:**

- **QEMU savevm / live-migration stream format** (`QEMU_VM_FILE_MAGIC
  0x5145564D` + `VMStateDescription` device tables). Rejected:
  (1) no existing non-QEMU producer of this format exists —
  the only third-party tool in this space (`lqs2mem`) reads it,
  does not write it; reproducing `VMStateDescription` tables
  from outside QEMU means vendoring QEMU's device catalog.
  (2) UML's "devices" (hostfs, mconsole, virtio-over-unix) have
  no QEMU peers, so **zero real interop is unlocked** — QEMU
  still could not load a UML-produced savevm stream even if we
  wrote one byte-for-byte compatible. (3) LKML has no precedent
  for a kernel producer of a QEMU-private userspace wire
  format; reviewer friction would be high. Pay-for-what-you-
  don't-use.

- **CRIU image format** (`.img` files, protocol buffers,
  per-subsystem schemas). Rejected: (1) libprotobuf-c is not
  realistically landable as a kernel dependency — it would
  need to be a userspace-only post-processor, meaning the
  kernel writes some intermediate format anyway. (2) CRIU's
  per-subsystem image proliferation (`mm-*.img`, `pagemap-*.img`,
  `fdinfo-*.img`, `files.img`, `core-*.img`, `tcp-stream-*.img`,
  …) is shaped for dumping uncooperative multi-process targets
  from outside; UML is one cooperative process. The format's
  value-add over a flatter scheme is the subset we don't need.
  (3) No in-kernel precedent for protobuf-shaped state dumps.

- **gVisor statefile (Go `gob` + reflection).** Rejected: Go-
  only; no C equivalent to reflection-driven per-object
  serialization. Not a proposal path.

- **Firecracker snapshot format (Serde + bitcode).** Rejected:
  Rust/userspace only; every field addition requires a MAJOR
  version bump per Firecracker's own versioning docs — their
  own maintainers call this a "sharp edge" that forces migration
  work on every clone-user. Wrong shape for a kernel that wants
  long-term format stability.

- **kexec/kdump ELF + vmcoreinfo (`/proc/vmcore`).** Strongly
  considered; adopted as the template. The decision is not
  "reuse kexec's format as-is" but "reuse ELF64 + `PT_NOTE`
  and produce it from the same kernel-side shape that
  `fs/binfmt_elf.c` and `fs/proc/vmcore.c` already produce."
  This gives us their battle-tested ELF producer code patterns
  without inheriting kdump's "post-mortem read-only" posture
  (v2 needs restore, kdump does not).

- **Plain ELF64 core with no UML-specific notes.** Rejected:
  insufficient. A stock ELF core captures registers, memory,
  and a couple of standard notes; it does not capture
  UML-specific state like backend kind, stub-child table,
  timer state, or ready-point identity. The `PT_NOTE`
  extensibility pattern is the whole point of picking ELF.

**Why ELF + vendor `PT_NOTE` is the right shape:**

1. **LKML precedent.** `fs/binfmt_elf.c` and
   `fs/proc/vmcore.c` already produce ELF from kernel context.
   Reviewers recognize the shape; the code idiom is in-tree.
   No new wire format is being proposed — we're reusing the
   one the kernel already owns.

2. **Sparseness is native.** `PT_LOAD` + FS holes handle UML's
   16 TB sparse KASAN shadow natively; per-segment `p_filesz
   != p_memsz` encodes "this region is larger in memory than
   on disk" cleanly. No zero-page compression schemes needed.

3. **Extensibility is linear.** New UML subsystems add new
   `PT_NOTE` types. Adding a field = new type number OR
   backward-compatible suffix in existing type. Dropping a
   field = keep reading, ignore, bump minor in `UML_VERSION`.
   Never renumber existing types. This is the versioning model
   kdump vmcoreinfo uses and has lived with for years.

4. **Tooling is free.** Stock `gdb`, `objdump`, `readelf`,
   `crash(8)` all already parse ELF core dumps. Scenario 4
   (read-only post-mortem review) falls out of the format
   choice for zero additional engineering — open the
   `.umsnap` in gdb, walk the state. This is the strongest
   single argument for ELF.

5. **UML's self-knowledge collapses the hard part.** CRIU's
   ~5.6 kLOC memory-path machinery (parasite injection,
   pagemap walking, page-pipe bulk xfer, TCP repair) exists
   because CRIU dumps processes from outside. UML knows its
   own mmaps by construction — the mmap-table pull-forward
   from D37 makes the memory-path ~hundreds of LOC, not
   thousands. Picking a format that takes advantage of this
   (ELF PT_LOAD per-region) rather than one that doesn't
   (CRIU's per-page-tuple scheme) matters.

**What we give up:**

- No out-of-the-box loader. A new `uml-restore` host tool
  lives in `tools/uml/snapshot/` (AGENT-PROMPT sign-off gated)
  or as a `scripts/` shim. Cost: bounded, small.
- No postcopy-grade live migration. Acceptable for now; can
  layer `userfaultfd` on top as the lazy-pages tier (v2 phase
  3) later.
- No ecosystem compatibility with QEMU's snapshot tooling.
  Acceptable — there was never going to be any, because UML
  devices don't round-trip into QEMU.

**Lifetime:** Indefinite once adopted. The ELF + `PT_NOTE`
shape is deliberately stable; the mutable part is the set of
UML-specific note types, which grow linearly and never shrink
(deleted types remain read-only-tolerant for back-compat).

**Revisit triggers:**

- A concrete user / maintainer asks for QEMU-savevm interop
  and we discover a real use case (not just the abstract idea
  of it).
- kexec/kdump format evolves in a way that invalidates our
  reuse of its in-kernel producer shape.
- We learn that `PT_NOTE` type-number collisions with other
  vendor extensions are causing tooling confusion.
- The format layer starts accumulating its own correctness
  bugs (serialization bugs we didn't anticipate); may indicate
  the schema-per-note pattern needs to move to something
  typed-and-versioned like protobuf after all.

**Cross-references:**

- `08-future-phases/02-snapshot-to-disk.md` — the v2 design
  this format decision underpins.
- D35 — the v1-vs-v2 split this format decision follows on
  from.
- D37 — v1 pull-forward items that make this format cheap to
  produce.
- `fs/binfmt_elf.c`, `fs/proc/vmcore.c` — in-tree ELF
  producers whose code shape the v2 writer will mirror.
- QEMU savevm format: <https://www.qemu.org/docs/master/devel/migration/main.html>
- CRIU image format: <https://criu.org/Images>
- gVisor checkpoint/restore: <https://pkg.go.dev/gvisor.dev/gvisor/pkg/sentry/pgalloc>
- Firecracker versioning: <https://github.com/firecracker-microvm/firecracker/blob/main/docs/snapshotting/versioning.md>
- `juergh/lqs2mem`: <https://github.com/juergh/lqs2mem>
  (the one non-QEMU *reader* of savevm format, cited as evidence
  that no non-QEMU *writers* exist).

---

## D37: Five v1 scope additions pulled forward from v2 snapshot-to-disk design

**Date:** 2026-04-20
**Status:** Accepted (expands the C-09 v1 commit plan committed
in D35; each item is cheap now and expensive to retrofit later).

**Decision:** The design research for v2 snapshot-to-disk (see
D36 and `08-future-phases/02-snapshot-to-disk.md`) surfaced five
pieces of infrastructure that are **load-bearing for v2 but also
useful for v1**, plus one v1 correctness bug found during the
deep-reading. Rather than ship v1 and then retrofit these when
v2 opens, land them as part of C-09 v1.

Items pulled forward (updates the C-09 v1 commit plan from D35's
original 6 commits to the revised plan in
`02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md`):

1. **Kernel mmap enumeration table** (`arch/um/include/asm/
   um-mmaps.h` + population in the call sites that today `mmap`
   blindly). One `struct um_mmap_region { name, base, len,
   disposition, flags }` array. Why pull forward: v1 fork-server
   already wants this for the FD-hygiene audit, for KASAN
   workaround logic (item 6 below), and for the selftest's
   ready-point assertion. Retrofit cost: find every `mmap` call
   site, wire into a table — high. Forward cost: ~100 LOC.

2. **Strict ready-point contract (assert-on-entry).** At
   `um_snapshot_ready()` entry, assert: no dirty inodes, no
   pending RCU callbacks, no kthread in `TASK_RUNNING` other
   than caller, no IRQ in flight, no pending signals except
   SIGCHLD. Already named as Risk Q6 in the C-09 v1 design;
   pull-forward promotes it from "we should probably check" to
   "we check, loudly, on every snapshot-ready". Retrofit cost:
   classes of Heisenbugs that live under it without assertion.
   Forward cost: ~50 LOC.

3. **FD allowlist extended with disposition annotation.** v1's
   existing `FD_CLOEXEC` sweep already walks every
   `socket()`/`open()` site in `arch/um/os-Linux/`. At each
   site, record disposition: `INHERIT_ACROSS_FORK`,
   `SERIALIZE_CONTENT` (hostfs files, etc.),
   `RECONSTRUCT_BY_PATH` (sockets re-openable from a name),
   `SKIP` (fuzz control socket, log fd, etc.). Retrofit cost:
   second sweep of the same 30 sites at a later date. Forward
   cost: ~1 extra line per site (a disposition tag).

4. **`/sys/kernel/um/state_version` sysfs node.** One integer
   that represents the current snapshot schema version.
   Commits us to a versioning narrative; surfaces in boot logs;
   lets selftests assert compatibility. Retrofit cost: low, but
   surfacing it now sets expectations. Forward cost: ~20 LOC.

5. **`physmem_fd` + other file-backed mmap invariants
   documented and asserted.** `arch/um/kernel/physmem.c`
   already sets up `physmem_fd` as a tempfile-backed
   `MAP_SHARED` region — v2 depends on this (v2 reads the fd
   directly to serialize RAM). v1 pull-forward: add a
   `BUILD_BUG_ON`-style assert that `physmem_fd` remains
   `MAP_SHARED` and file-backed, and comment the invariant at
   the call site. Retrofit cost: none if it stays, potentially
   enormous if someone flips it to `MAP_PRIVATE` without
   realizing. Forward cost: ~5 LOC + comment.

**Plus one v1 bug found in deep-reading:**

6. **KASAN shadow has `MADV_DONTFORK` set
   (`arch/um/os-Linux/mem.c:49`).** D35 Q2 assumed fork() COWs
   the shadow — it **doesn't**, it unmaps it in the child.
   Workers under fuzz profile (which has `CONFIG_KASAN=y`) would
   SEGV on first KASAN-instrumented access. Fix: under
   `CONFIG_UM_FUZZ_HOOKS`, either (a) drop the
   `MADV_DONTFORK` on shadow so it COWs like RAM, or (b)
   re-call `kasan_map_memory()` in `um_snapshot_worker_init()`.
   Choosing (a) is simpler and matches the RAM mapping's
   disposition; (b) is cleaner isolation but requires careful
   re-poisoning of in-use slabs in the child. Picking (a) in
   v1; note in the workstream that (b) remains open if (a)
   shows drift bugs.

**Why all of these pull forward:**

Each item is under ~100 LOC of kernel code. Each item is useful
for v1 regardless of whether v2 ever lands. Together they
make v2's writer-side ~hundreds of LOC instead of thousands.
And — critically — item 6 is a v1 correctness bug; not doing
it is not an option once fuzz profile enables both KASAN and
`UM_FUZZ_HOOKS`.

**Alternatives considered:**

- **Defer all five to v2, ship v1 without them.** Rejected:
  each is cheap now and expensive to retrofit. Ready-point
  contract assertions (item 2) in particular are the kind
  of thing that "should have been there from day 1" a year
  later when a Heisenbug surfaces.

- **Pull forward only the correctness bug (item 6), defer the
  others.** Rejected: item 1 (mmap table) is the enabler for
  cleanly fixing item 6 (you want to flip DONTFORK based on a
  per-region disposition, not hardcode it in one call site).
  The set is load-bearing together.

- **Treat all five as v1 re-scope needing new design.**
  Rejected: each item fits cleanly into a C-09 v1 commit the
  plan already budgets for. They're refinements to an existing
  commit plan, not new commits.

**Lifetime:** Permanent. These five items become part of UML's
snapshot/restore contract; removing any of them in a later
version would be a semantic regression.

**Revisit triggers:**

- v1 lands and an item above shows no concrete benefit. Would
  indicate the design research overestimated its usefulness.
- v2 opens and an item's shape is wrong for v2. Would mean
  the pull-forward guessed wrong about v2's needs; retrofit
  at that point is still cheaper than the alternative of
  having not pulled anything forward.

**Cross-references:**

- D35 — original C-09 v1 scope and the v1-vs-v2 split this
  expands.
- D36 — v2 format choice (ELF + `PT_NOTE`) that these items
  make cheap to produce.
- `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md`
  — updated v1 commit plan reflecting these additions.
- `08-future-phases/02-snapshot-to-disk.md` — the v2 design
  whose research surfaced the pull-forward opportunities.
- `arch/um/os-Linux/mem.c:49` — the `MADV_DONTFORK` call
  site on KASAN shadow addressed by item 6.
- `arch/um/kernel/physmem.c:75` — the `create_mem_file(len)`
  + `os_map_memory` call pattern that item 5 asserts.

---

## D38: v2 snapshot has two capture modes — crash-consistent (Mode A) and application-consistent (Mode B)

**Date:** 2026-04-20
**Status:** Accepted (expands the v2 parking-lot design in
`08-future-phases/02-snapshot-to-disk.md`; does not touch the
C-09 v1 forkserver design).

**Decision:** UML v2 snapshot-to-disk offers two capture modes
with the same on-disk ELF + `PT_NOTE` format (from D36), but
different coordination stories with the running UML guest:

- **Mode A (crash-consistent, external, non-interruptive):** an
  outside-the-UML process (sidecar, `uml-snapshot` CLI) reads
  UML state via `ptrace(PTRACE_GETREGS)` for registers,
  `cp --reflink=always` of `physmem_fd` from `/proc/$pid/fd/`
  for RAM, and `/proc/$pid/fd/` enumeration for the fd table.
  UML is never asked to cooperate; it sees no pause, no signal,
  no quiesce. On restore, a **recovery pass** (documented in
  the design doc §"Recovery pass") walks scheduler / RCU /
  writeback / lock state to fix up anything caught mid-
  mutation, then runs normal restore.
- **Mode B (application-consistent, internal, cooperative):** a
  signal (`SIGRTMIN+N`), debugfs write, mconsole command, or
  AFL-fd handshake pokes the UML kernel to hit the cooperative
  ready point from the C-09 v1 design. Kernel quiesces
  (parks kthreads, drains RCU, stops timer, asserts D37 pull-
  forward #2 contract), serializes itself to an ELF file,
  resumes. Restore is deterministic; no recovery pass needed.

A `PT_NOTE` of type `UML_CAPTURE_MODE` records which mode
produced the file; the restore path branches on it.

**Why two modes (the honest argument for not picking one):**

1. **Crash-consistent is uniquely cheap on UML.** `physmem_fd`
   is already a tmpfs file (C-09 D37 pull-forward #5 documents
   the contract); capturing RAM is a `cp --reflink=always` from
   `/proc/$pid/fd/`. On btrfs/xfs this is O(1). On any
   filesystem it is ≤ 100 ms for a 2 GB guest. **Zero UML
   pause**, because the UML kernel doesn't know it happened.
   Every other hypervisor has to work for this property —
   VMware pauses briefly to mark pages read-only, qemu does a
   live-migration-style dirty-bitmap dance. UML gets it for
   free. Declining to ship Mode A would be leaving load-bearing
   value on the table.

2. **Application-consistent is the semantics users expect for
   "save and resume".** Survive-reboot, portable-reproducer,
   "checkpoint here and resume later with deterministic
   behavior" — all expect Mode B semantics. Shipping only Mode
   A and saying "call the recovery pass and hope" is not the
   snapshot contract mainstream hypervisors offer.

3. **The same .umsnap file format works for both.** One writer
   layer (serialize mmaps + notes to ELF); two capture drivers
   (external ptrace+cp; internal quiesce+write). One restore
   layer with a single `if mode == A: recovery_pass()` branch.
   Incremental engineering cost of Mode A on top of Mode B is
   ~host-side tool + ~200 LOC of kernel-side recovery pass.

4. **The dichotomy is the industry norm.** Microsoft VSS
   formalizes it (crash-consistent vs application-consistent);
   MySQL ships both (`cp` of InnoDB tablespace vs `FLUSH TABLES
   WITH READ LOCK; mysqldump`); qemu ships both (external snapshot
   via LVM/disk-level vs `savevm` internal). Committing to both
   aligns UML with the precedent users arrive expecting.

5. **Different use cases need different modes.** CI + forensics
   + "snapshot everything hourly in production" want Mode A
   (zero pause, fsck-on-restore). Survive-reboot + portable
   reproducer + "clean handoff to a maintainer" want Mode B
   (deterministic restore). Forcing either camp to use the
   other mode's semantics would be user-hostile. (See the
   per-use-case matrix in the design doc.)

**Alternatives considered and why rejected:**

- **Mode A only; skip Mode B.** Rejected: the "I want to
  snapshot and then resume deterministically tomorrow" use
  case is load-bearing for scenarios 1 and 2 in the design
  doc. Mode A's recovery pass is best-effort by definition;
  users who need determinism need Mode B.
- **Mode B only; skip Mode A.** Rejected: leaves the "zero
  pause" lever unused. Mode A is almost free on UML
  specifically because `physmem_fd` is already tmpfs-backed;
  not shipping it means every CI / forensics caller pays a
  pause they don't need.
- **One "auto" mode that picks based on some heuristic.**
  Rejected: the two modes have different restore semantics
  (recovery pass runs or not). Caller MUST know which one
  they got. Auto-mode hides that decision and creates
  surprising behavior on restore.
- **Three-mode variant** (also a "live" mode that uses
  userfaultfd-driven dirty tracking for no-pause-no-recovery-
  pass). Rejected for now: that's the Nyx-shaped approach,
  requires in-kernel dirty-page tracking infrastructure that
  UML doesn't have yet, and the use-case matrix doesn't show
  it adding value over Mode A for the forensics/CI cases or
  over Mode B for the handoff cases. Revisit if a concrete
  user lands that needs "snapshot while running with
  guaranteed clean restore" — that's the gap a live mode
  would close.
- **Adopting qemu's external/internal terminology verbatim.**
  Considered; the terms overload with too many other meanings
  in the UML codebase (external vs internal backends,
  user-external modules, etc.). Crash-consistent / application-
  consistent is unambiguous and matches the VSS terminology
  that wider industry uses.

**What this does NOT change:**

- v1 (C-09, `09-snapshot-forkserver.md`) is unchanged. v1's
  forkserver is Mode B by construction (cooperative ready
  point + fork). The v1 commits proceed as planned; no re-
  scope.
- The ELF + `PT_NOTE` format (D36) is unchanged. Adding the
  `UML_CAPTURE_MODE` note is a new type in the existing
  namespace; no breaking change.
- The v1 pull-forward items (D37) are unchanged. They
  remain load-bearing for v2 regardless of mode.

**Lifetime:** Stable once v2 opens. The two-mode split is
deliberately the industry norm; reversing to single-mode would
be a regression.

**Revisit triggers:**

- A concrete user arrives with a "snapshot while running +
  deterministic restore" requirement that neither Mode A nor
  Mode B satisfies. Opens the v3 "live mode" design thread.
- The Mode A recovery pass proves too fragile in practice
  (too many "can't recover, abort" outcomes). May narrow
  Mode A's recommended use cases to forensics-only.
- The Mode B kernel-side writer proves inordinately expensive
  (e.g. `um_snapshot_write()` interacts badly with some
  subsystem's quiesce). May push us toward "Mode B = fork +
  async write from worker" which reuses more v1 plumbing.

**Cross-references:**

- `08-future-phases/02-snapshot-to-disk.md` §"Two capture
  modes: crash-consistent and application-consistent" — the
  full design.
- `08-future-phases/02-snapshot-to-disk.md` §"Recovery pass
  (Mode A only)" — what the restore does for Mode A captures.
- D35 — v1 scope (cooperative-only forkserver).
- D36 — on-disk format; `UML_CAPTURE_MODE` is a new note type
  in that namespace.
- D37 — v1 pull-forward items; the mmap registry and
  ready-point assertions are consumed by both modes.
- Microsoft VSS "shadow copy" terminology:
  <https://learn.microsoft.com/en-us/windows-server/storage/file-server/volume-shadow-copy-service>
- MySQL hot-backup vs cold-backup terminology:
  <https://dev.mysql.com/doc/refman/8.4/en/backup-types.html>
- qemu external vs internal snapshots:
  <https://www.qemu.org/docs/master/system/images.html#vm-snapshots>

---

## D39: C-09 commit 3 splits into 3a/3b/3c/3d due to risk tranches

**Date:** 2026-04-20
**Status:** Accepted after commits 1-2 landed and exposed the
four distinct risk classes inside what was originally scoped
as a single commit 3.

**Decision:** Split the original commit 3 ("um_snapshot_worker_init
+ KASAN fix") from `09-snapshot-forkserver.md` into four
sub-commits, each independently buildable + bootable:

- **3a:** parent loops forever (replace one-shot with while(1)).
- **3b:** KASAN `MADV_DOFORK` fix + populate mmap registry.
- **3c:** `um_snapshot_worker_init()` minimum (no-crash reinit
  in worker; worker still exits at end).
- **3d:** worker runs guest code + parent does waitpid + status
  byte.

**Why the split:**

Working commit-2 in place, the four pieces have very different
risk profiles, and bundling them would have broken the AGENT-
PROMPT bisectability rule in practice:

- 3a is trivial. `while (true) { handshake_iteration() }`. Parent
  never returns from `um_snapshot_ready`, so commit 2's
  "parent-panics-after-return" limitation disappears by
  construction. Half a day.
- 3b is surgical. One-line `MADV_DONTFORK` → `MADV_DOFORK` flip
  plus two `um_register_mmap_region()` call sites. Half a day.
  Independently valuable (unblocks KASAN-enabled workers even
  if 3c/3d stall).
- 3c is where UML's host-thread model fights back. Fork
  inherits exactly the calling thread; every other helper
  (IRQ driver, timer, mconsole, seccomp stubs) is lost in the
  child. Worker has to either abandon them or recreate them
  in a fresh shape. The scope is bounded — worker still exits
  at the end — so the validation bar is "reinit doesn't
  crash" rather than "worker runs real code". 2-5 days.
- 3d is where we learn what we don't know. Worker returns from
  reinit and continues as a real UML guest. The lazy stub
  respawn path gets exercised for the first time. Guest
  userspace syscalls start flowing. Any residual inherited
  state that reinit missed shows up as a SEGV, hang, or
  corruption. 1-2 weeks, possibly longer if UML's internals
  have surprises.

Bundling these into one commit means a regression anywhere
above can only be bisected to "commit 3". Splitting lets us
bisect to the specific sub-commit, which is more useful when
the failure mode is "kernel panic at iteration 37" rather than
"kernel panic at boot".

**Alternatives considered and why rejected:**

- **Keep commit 3 as one big patch per the original plan.**
  Rejected: bisectability, reviewability, and the dev-cost
  imbalance (half-day items gated by 2-week items in the same
  commit) all push against this.
- **Split commit 3 into only two parts (3a+3b together; 3c+3d
  together).** Rejected: 3a's risk profile (trivial) and 3c's
  (gnarly) are incompatible; if 3c breaks something subtle,
  bisect would finger 3a+3c jointly. Each sub-commit in the
  split is its own logical change per AGENT-PROMPT.
- **Slip the pull-forward items (KASAN DOFORK, mmap registry
  populate) out of commit 3 into a separate "infra" commit
  series.** Rejected: the items are load-bearing for 3c/3d's
  worker-side logic (the registry is what 3c iterates; the
  DOFORK flip is what lets 3c/3d run under KASAN). Shipping
  them as "infra" before the consumer exists means they sit
  as dead code.

**Lifetime:** Until commits 3a-3d are all landed. Then this
decision becomes historical.

**Revisit triggers:**

- 3c turns out to be >2 weeks of work (likely: split 3c
  further — worker signal handling, worker stub teardown,
  worker thread abandonment as separate sub-sub-commits).
- 3d turns out to be >4 weeks (likely: add a formal pause +
  pre-3d design pass + possibly a decisions-log entry on
  "which UML subsystems can't survive a fork and must be
  turned off in fuzz profile").
- A user shows up wanting the forkserver at a different
  stability point (e.g. "fork for snapshot-to-disk Mode A"
  — but that's v2 territory per D35/D36/D38, not this split).

**Cross-references:**

- `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md`
  §"Commit plan" — updated with the 3a/3b/3c/3d split.
- D35 — original C-09 v1 scope and the 6-commit plan.
- D37 — v1 pull-forward items consumed by 3b, 3c, 3d.
- Commit `b78df759dfbd` — C-09 commit 1 (skeleton + registry).
- Commit `c29a9ed9960c` — C-09 commit 2 (handshake + one-shot
  fork); exposed the need for this split.

---

## D40: C-09 commit 3d splits into 3d-a/3d-b/3d-c/3d-d (same rationale as D39)

**Date:** 2026-04-20
**Status:** Accepted after commits 3a, 3b, 3c landed and a careful
testing-plan analysis exposed 3d's four distinct risk tranches.

**Decision:** Split the originally-scoped commit 3d ("worker runs
guest code + waitpid/status in parent") from D39 into four sub-
commits, each independently buildable + bootable + bisectable:

- **3d-a:** parent-side waitpid + 4-byte status byte. Worker
  unchanged (still exit_group). Tests whether the commit-2
  waitpid-call-site crash reproduces in the commit-3c-stable
  world. Low-medium risk; the only change is parent adding one
  blocking syscall + one 4-byte write.
- **3d-b:** REBUILD half of worker reinit (pairs with the FORGET
  half in commit 3c). Adds `os_sigio_worker_rebuild`,
  `os_timer_worker_rebuild`, and mm_list clear in
  `um_snapshot_worker_init()`. Worker still exits at end of
  `um_snapshot_ready`. Tests "rebuild itself doesn't crash"
  without also testing "return path works" at the same time.
  Medium risk.
- **3d-c:** flip the worker path from `os_snapshot_worker_exit(0)`
  to `return 0`. Worker unwinds up through `um_snapshot_ready`,
  past the debugfs-write handler, back into init userspace.
  Init script runs its next trivial command
  (`echo hello; exit 0`). Highest-risk sub-commit: RCU,
  scheduler, kthread state inherited from parent may not
  survive. Bugs may require changes outside `arch/um/`.
  2-5 days.
- **3d-d:** worker `exec /bin/echo hello` — first real guest
  userspace task post-fork, triggers `start_userspace()` →
  fresh seccomp stub owned by the worker. 1-2 weeks worst
  case.

**Why split rather than one commit:**

Same argument as D39. The four pieces have distinct failure modes
with distinct test signals: 3d-a would crash during `waitpid`, 3d-b
during a rebuild helper, 3d-c during return-to-user, 3d-d during
execve. A regression in a combined 3d would bisect to "3d" and
then require reading the full diff to isolate. Splitting lets
bisect point at the specific sub-commit.

Also: the user-visible testing plan I wrote through
(conversation transcript; summarized below as §"Step-by-step
testing approach") naturally has four steps matching these four
sub-commits. Each step has a single incremental test; each sub-
commit's validation is exactly one step.

**Step-by-step testing approach (recorded for context):**

1. Baseline (commit 3c already landed): 100 fork iterations,
   worker exits immediately, no leaks.
2. **3d-a test:** parent survives `waitpid`. Fuzz harness sends
   a cmd, reads pid, reads status; status == 0 (worker's
   exit_group(0)).
3. **3d-b test:** worker runs the rebuild helpers before exit;
   dmesg clean; new timer/signalfd visible in `/proc/$child_pid/fd`
   (captured at a debug break).
4. **3d-c test:** init script `echo hello; exit 0`. Worker
   prints "hello" via existing stderr plumbing and exits(0).
   Parent reaps with status 0.
5. **3d-d test:** init script `exec /bin/echo hello`. Worker's
   first guest-userspace task runs. 10 iterations back to back.
6. Stability: 1000 iterations of `/bin/true` back to back; no
   growth in parent's fd count / mm_list length / rss.

Expected failure modes, each addressed by a specific sub-commit:

- Parent-post-fork `waitpid` null-jump — isolated by 3d-a.
- Worker's first schedule after inheriting parent's runqueue —
  surfaces at 3d-c.
- RCU grace-period stall (worker inherits RCU bookkeeping) —
  surfaces at 3d-c.
- Seccomp stub ptrace-permission — surfaces at 3d-d.
- Stub clone of fresh mm_id — surfaces at 3d-d.
- Leaks / state growth across iterations — surfaces only at
  step 6, not a commit of its own.

**Alternatives considered and why rejected:**

- **Keep commit 3d as one big patch per D39.** Rejected for the
  same bisectability + risk-mixing reasons D39 rejected the
  commit-3 monolith.
- **Split further into 6 sub-commits mirroring the 6 testing
  steps.** Rejected: steps 1 and 6 don't have code deltas
  (step 1 is baseline; step 6 is stability test, not a commit).
  Steps 2-5 map naturally to 3d-a, 3d-b, 3d-c, 3d-d.
- **Land commit 5 (snapshot-smoke selftest) early so 3d's
  sub-commits can use it for validation.** Considered. Rejected
  for v1 of the split because the minimum tests for 3d-a (one
  iteration with status byte check) and 3d-b (fd-count assertion)
  are cheap to write as quick Python drivers. Pulling the full
  commit-5 selftest scaffold forward would itself need the
  state_version sysfs node (pull-forward #4 from D37), which
  isn't blocking the 3d split. Commit 5 still lands after 3d-d
  per the original plan.

**Lifetime:** Until commits 3d-a through 3d-d are all landed.
Then this decision becomes historical.

**Revisit triggers:**

- 3d-c turns out to be >1 week of work (likely: sub-split again
  — return path vs scheduler fixup vs RCU fixup as separate
  sub-sub-commits).
- A failure mode emerges that needs cross-subsystem changes
  (`kernel/sched/core.c`, `kernel/rcu/*`, etc.) and the AGENT-
  PROMPT cross-subsystem sign-off rule fires.
- The commit-2 waitpid crash reproduces in 3d-a: suggests the
  crash wasn't about post-fork parent state (ruled out by
  commit 3a's "parent doesn't return" path), and we need a
  narrower investigation.

**Cross-references:**

- `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md`
  §"Commit plan" — updated with the 3d-a/3d-b/3d-c/3d-d split.
- D39 — commit-3 split that set the precedent for this kind of
  decomposition.
- D35 — the original v1 scope.
- Commit `8f5e8b2159ea` — C-09 commit 3c (worker_init forgets
  parent state); the FORGET half that 3d-b's REBUILD half
  pairs with.

---

## D41: UML `signals_enabled` is the canonical signal-gating primitive for snapshot/forkserver critical sections

**Date:** 2026-04-20
**Status:** Accepted after commit 3d-a's waitpid-crash investigation
surfaced that host `sigprocmask` is the wrong level of abstraction for
gating UML's in-kernel IRQ dispatch.

**Decision:** Any snapshot/forkserver code that runs a blocking host
syscall from inside UML kernel context MUST use UML's
`signals_enabled` machinery (via `um_set_signals()` /
`um_get_signals()` / `block_signals()` / `unblock_signals()` from
`arch/um/os-Linux/signal.c`), NOT raw `sigprocmask`. This applies to:

- The forkserver loop body (handshake + fork + waitpid + status
  write), already using `um_set_signals` since the 3d-a hardening
  commit.
- The worker-side `um_snapshot_worker_init()` rebuild path (commit
  3d-b) — any `waitpid` / `poll` / blocking helper the rebuild uses
  must run under UML signal gating.
- The worker-returns path (commit 3d-c) — when the worker unwinds
  back through `um_snapshot_ready` to run guest code, `signals_enabled`
  must be set to 1 before guest-kernel-mode execution resumes, so
  SIGALRM, SIGIO, SIGCHLD actually drive the worker's timer, I/O,
  and stub-child-reap paths. The worker inherits
  `signals_enabled == 0` from the parent's pre-fork block; 3d-b's
  rebuild helpers, or the tail of `um_snapshot_worker_init()`,
  own re-enabling it.
- Any future v2 snapshot-to-disk code that blocks in a kernel-side
  syscall while writing the ELF file (D36).

**The mechanism, stated concretely:**

UML maintains a per-thread TLS `signals_enabled` flag
(`arch/um/os-Linux/signal.c:94`) that the in-kernel hard signal
handler consults at every delivery (`sig_handler` line 101,
`timer_alarm_handler` line 155). When `signals_enabled == 0`,
inbound SIGIO / SIGCHLD / SIGALRM are queued into
`signals_pending` (a bitmask) and the handler returns immediately
without entering `do_IRQ` / scheduler / RCU / any UML kernel
path. `unblock_signals()` drains the queue synchronously in a
well-defined order (SIGIO, then SIGCHLD, then SIGALRM) when the
caller flips the flag back to 1.

Raw host `sigprocmask(SIG_BLOCK, …)` by contrast stops the host
kernel from *delivering* signals. The UML dispatch layer's flag
is unchanged. When the mask is restored, queued signals fire the
UML handler, which finds `signals_enabled == 1` and enters the
IRQ path from whatever context we happen to be in — including
from the tail of a host syscall that has just returned. That is
the wrong shape for protecting a critical section.

The canonical UML-internal users of this pattern are
`os_kill_process` and `os_kill_ptraced_process`
(`arch/um/os-Linux/process.c:33,53`), which wrap their
`waitpid()` in `block_signals()` / `unblock_signals()`.

**Waitpid investigation (what we tried, what failed):**

During commit 3d-a we attempted to call `os_snapshot_waitpid_status()`
from the parent side of the forkserver loop. Every variant crashed
the parent with a null-jump or a UML-VA-heap-jump consistent with
a longjmp-into-a-stale-target or an indirect call through a
corrupted function pointer:

| Attempt | Signal gate | Wait primitive | Crash RIP |
|---------|-------------|----------------|-----------|
| 1 | none | glibc `waitpid` | `0x0` |
| 2 | host `sigprocmask` block of SIGCHLD / SIGALRM / SIGIO / SIGUSR1 | glibc `waitpid` | `0x61093b80` |
| 3 | UML `um_set_signals(0)` | glibc `waitpid` | `0x0` |
| 4 | UML `um_set_signals(0)` | raw `syscall(__NR_wait4)` | `0x61093bc0` |

The UML-native gate (attempts 3, 4) is the right primitive for the
general class of problem, but does not by itself fix the waitpid
crash. Suspect paths: (a) fault signals (SIGSEGV, SIGBUS, SIGFPE,
SIGILL) that `sig_handler_common` dispatches regardless of
`signals_enabled`; (b) some glibc syscall-wrapper machinery
(cancellation-point hooks, pthread-specific state) that we have
not traced; (c) a UML scheduler re-entry via a path independent
of the host signal flow. The consistent heap-address cluster in
attempts 2 and 4 is structurally similar to `longjmp` into a
jmp_buf whose saved `rip` has been overwritten post-fork.

Deferred to commit 3d-c. By the time 3d-c runs, the worker-side
rebuild (3d-b) changes what state exists in the parent during
the wait window — `signals_enabled` may already be 1 again in
worker paths, timer/sigio helpers exist in the worker's own
address space, etc. If 3d-c still can't call `waitpid()` safely,
the fallback is either (i) pidfd_open + poll, which may have a
different signal profile; (ii) a SIGCHLD-driven wait where the
UML IRQ handler does the reap via the existing mm_sigchld_irq
machinery instead of a host blocking call; or (iii) explicitly
accept "no status byte, just zombies" and document.

**Impact on already-shipped commits 1–3c + 3d-a:**

None. Audited in the same series as this commit and noted in the
"Shipped-commit audit" section of
`02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md`.
Commits 1, 2, 3b do not touch UML signal state. Commits 3a, 3c,
3d-a use `os_snapshot_block_iter_signals` /
`os_snapshot_unblock_iter_signals` which (since the 3d-a
hardening commit) internally use `um_set_signals`, the right
primitive. The worker path in 3a/3c inherits
`signals_enabled == 0` from the pre-fork block and exits
immediately via `os_snapshot_worker_exit(0)`, bypassing any
code that would consult the flag.

**What this commit adds to the code:**

`um_snapshot_assert_ready()` gains a check that
`signals_enabled == 1` at ready-point entry. This documents
that the ready-point contract expects a fully operational UML
signal state before we start quiescing. If a future caller
enters `um_snapshot_ready()` with signals already gated, a
WARN_ONCE fires and the ready-point is refused, preventing a
subtle re-entry where our `block_iter` call is a no-op and the
critical section is already "open" from someone else's
perspective.

**Alternatives considered and why rejected:**

- **Keep using raw `sigprocmask` and document the limitation.**
  Rejected: hides the bug class. Future contributors writing new
  forkserver-adjacent code would re-introduce the same shape.
- **Write a new UML-kernel-side wrapper that combines
  `sigprocmask` + `um_set_signals`.** Rejected: the two primitives
  live at different layers; combining them doesn't add value over
  just using the higher-level one. `os_kill_process` uses only
  `block_signals`.
- **Add a `CONFIG_UM_SIGNAL_CONTRACT_STRICT` Kconfig gate
  around the assertion.** Rejected for now: the assertion is
  cheap and has zero false-positives; adding config complexity
  is premature.

**Lifetime:** Indefinite. `signals_enabled` has been UML's signal
gate since time-travel-mode landing (and conceptually earlier);
it is not going away.

**Revisit triggers:**

- Upstream UML grows a different signal-gating primitive for
  kernel-mode critical sections (e.g., a per-CPU variant for SMP).
  Update D41 to cover the new primitive.
- 3d-c's waitpid retry succeeds with the current primitives —
  update D41 with the specific additional state change that
  unblocked it, so future readers know what was load-bearing.
- 3d-c's waitpid retry fails: D41 becomes the spec for
  alternative wait strategies (pidfd_open + poll, SIGCHLD-IRQ
  reap) and which one we adopt.

**Cross-references:**

- D35 — original C-09 v1 scope.
- D39 — commit-3 split.
- D40 — commit-3d split; gets a follow-up paragraph in this
  series noting that 3d-b/3d-c have explicit signals_enabled
  contracts.
- `arch/um/os-Linux/signal.c` lines 94–316 — the signal-gating
  code: declaration of `signals_enabled`, `sig_handler`,
  `timer_alarm_handler`, `block_signals`/`unblock_signals`,
  `__block_signals`/`__unblock_signals`, `um_set_signals`.
- `arch/um/os-Linux/process.c:33,53` —
  `os_kill_process`/`os_kill_ptraced_process`: the canonical
  existing in-tree pattern of "block UML signals before
  waitpid".
- Commits on this branch that use the right primitive:
  `a0328b6011ed` (3d-a, original sigprocmask shipped, superseded),
  `b2e391348e80` (3d-a hardening, switched to um_set_signals).

---

## D42: C-09 3d-d sanitizes the worker's CFS runqueue via one exported sched helper

**Date:** 2026-04-20
**Status:** Accepted and shipped as a v1 building block; observed
insufficient in isolation — the slab-OOB at
`__set_next_task_fair+0x11b` persists even with the helper in place.
Commit 3d-c demonstrated that a forked worker CAN run guest code
(`/bin/echo`, `/bin/true` observed in the worker's output stream),
and the next voluntary `schedule()` tripped a KASAN slab-out-of-
bounds in `__set_next_task_fair + dequeue_entities` — exactly the
scheduler-state-sharing hazard predicted in the D40 testing plan.
The 3d-d helper removes parent-inherited tasks from `rq->cfs_tasks`
under the scheduler's normal `deactivate_task` primitive; however,
real-world runtime shows the scheduler retains additional
invariants beyond list membership (secondary signal: a `WARN` at
`fair.c:5637` about `sched_delayed` state). We keep the helper in
the tree because (a) the detach is a strict precondition for any
deeper fix, (b) it documents the failure shape in code, and (c)
removing it would hide the fact that v1 is ceilinged at non-
blocking guest programs. v2 replaces this with a freezer-cgroup
pre-fork + per-task re-clone design per D41's revisit triggers;
this helper is removed in the same series that lands v2.

**Decision:** Commit 3d-d adds one small exported helper in
`kernel/sched/core.c` that arch/um calls from
`um_snapshot_worker_init()` after the forget step and before the
rebuild step. The helper walks `rq->cfs_tasks` on the local CPU's
runqueue under `rq_lock_irqsave` and calls `deactivate_task(rq, p,
DEQUEUE_NOCLOCK)` on every queued task other than `current`. Non-
current tasks' `task_struct`s are inherited from the parent via
fork CoW; their scheduling state (saved jmp_buf in
`thread.switch_buf`, per-task irqstack, etc.) references memory
that is valid AS ADDRESSES post-fork but semantically belongs to
the parent's host-thread graph, and attempting to switch to them
in the worker is the load-bearing crash path. Deactivating them
from the rq keeps their `task_struct`s alive (they're not
`do_exit`'d) and simply prevents the scheduler from ever picking
them; they leak for the lifetime of the short-lived worker host
process and the host kernel reclaims them at `exit_group`.

Proposed helper (goes into `kernel/sched/core.c`, ~20 LOC):

```c
/*
 * Arch-specific helper for processes that fork() the UML kernel
 * (workstream C-09, D42). In the forked child (the "worker"), the
 * current CPU's CFS runqueue still references task_structs that
 * were enqueued by the parent before fork. Those tasks exist as
 * memory — the task_struct pages CoW'd — but their scheduling
 * context (jmp_buf'd stack pointers, etc.) targets host-thread
 * state that does not exist in the worker. Picking one via
 * __set_next_task_fair would dereference stale pointers (observed
 * as KASAN slab-OOB in 3d-c's bring-up). Detach them all from the
 * rq so `schedule()` picks only `current`.
 *
 * Called only from arch/um/kernel/snapshot.c's
 * um_snapshot_worker_init(), under that worker's signals_enabled
 * == 0 guard (see UML decisions-log D41).
 */
void sched_worker_detach_other_tasks(void)
{
	struct rq *rq = this_rq();
	struct rq_flags rf;
	struct task_struct *p;
	struct sched_entity *se, *tmp;

	rq_lock_irqsave(rq, &rf);
	update_rq_clock(rq);
	list_for_each_entry_safe(se, tmp, &rq->cfs_tasks, group_node) {
		p = task_of(se);
		if (p == current)
			continue;
		if (!task_on_rq_queued(p))
			continue;
		deactivate_task(rq, p, DEQUEUE_NOCLOCK);
	}
	rq_unlock_irqrestore(rq, &rf);
}
EXPORT_SYMBOL_GPL(sched_worker_detach_other_tasks);
```

Plus one `extern void` declaration in `include/linux/sched.h` so
arch/um can call it without reaching into `kernel/sched/sched.h`
(which is explicitly an internal header).

**User sign-off:** explicitly given in-conversation after the 3d-c
breakthrough ("don't give up now, it's working; either way is ok")
as the concrete go-ahead to touch `kernel/sched/core.c` per the
AGENT-PROMPT cross-subsystem rule.

**Why this shape and not one of the alternatives:**

1. **Most narrowly scoped fix that actually works.** Agent research
   surveyed the failure site (`__set_next_task_fair` list/rbtree
   walks in fair.c), the canonical deactivation primitive
   (`deactivate_task` — the "dequeue from rq, leave task_struct
   alive" API that every cpu-hotplug-down and per-task-migration
   path uses internally), and four alternatives (B: raw CFS rbtree
   surgery; C: PF_KTHREAD flag tricks; D: permanent
   `preempt_disable`; E: freezer-cgroup pre-fork barrier). Only the
   chosen path uses the scheduler's own bookkeeping correctly;
   raw rbtree surgery bypasses psi/uclamp/bandwidth accounting and
   PF_KTHREAD flags are not scheduler skip-predicates.

2. **Zero impact on the parent.** The helper operates on `this_rq()`
   under `rq_lock_irqsave`. In the worker, "this_rq" is the
   worker's CoW'd rq struct; deactivations happen in the worker's
   copy. Parent's rq is untouched.

3. **Zero new locking.** Uses existing `rq_lock_irqsave` which the
   rest of `kernel/sched/core.c` uses for exactly this kind of
   rq-scoped mutation.

4. **Bisectable.** One exported symbol, one `extern` in
   `include/linux/sched.h`, one call site in arch/um. Every other
   part of C-09 (commits 1 through 3d-c) still builds without it.
   Reverting the sched patch cleanly leaves the forkserver working
   for non-blocking testcases — the arch-local v1 limit.

5. **Honest upstream posture.** On upstream submission, sched
   maintainers will ask "is there no other way?" and a freezer-
   cgroup-pre-fork design (D41 / agent alternative E) is the
   cleaner architecture. D42 explicitly scopes the present helper
   as a v1-for-UML choice, not a general-purpose API, and calls
   out the v2 freezer design as the upstream-preferred replacement.

**Alternatives considered and why rejected (recap from the agent's
research, summary form):**

- **B. Direct `dequeue_entity` / rbtree surgery from arch/um.**
  Bypasses `psi_dequeue`, `uclamp_rq_dec`, `update_h_nr_running`
  bookkeeping; duplicates state `deactivate_task` already handles
  correctly.
- **C. `PF_KTHREAD | PF_NOFREEZE` flags as "skip-me" markers.**
  The scheduler does not use task flags as skip predicates;
  state-based only (`__state != TASK_RUNNING` gets dequeued, but
  only for `current` at `schedule()` time, not retroactively for
  queued tasks).
- **D. Permanent `preempt_disable()` in the worker.**
  `preempt_disable` only prevents preemptive scheduling from
  interrupts; voluntary `schedule()` calls (wait_event,
  mutex_lock sleeps, page faults, execve, any blocking syscall)
  still walk the rq. The fuzz init script cannot avoid all of
  these.
- **E. Freezer-cgroup barrier pre-fork, restore by recloning
  every task.**
  Architecturally the cleanest (CRIU's pattern, gVisor's task-
  goroutine model). Large. Cross-multiple-subsystems. Deferred
  to v2 per D41; v1 gets the narrow helper.

**What 3d-d code changes:**

- `include/linux/sched.h`: one `extern void
  sched_worker_detach_other_tasks(void);` declaration, guarded by
  `CONFIG_UM_SNAPSHOT_FORKSERVER` or similar so it only exists
  when the UML snapshot path is compiled in.
- `kernel/sched/core.c`: the ~20-LOC helper above, similarly
  guarded. Exports via `EXPORT_SYMBOL_GPL` so arch/um can link
  against it.
- `arch/um/kernel/snapshot.c`: call
  `sched_worker_detach_other_tasks()` inside
  `um_snapshot_worker_init()`, between the forget helpers
  (`os_sigio_worker_forget`, `os_timer_worker_forget`) and the
  rebuild helpers (`os_sigio_worker_rebuild`,
  `os_timer_worker_rebuild`). Signal-gating contract (D41) holds:
  `signals_enabled` is 0 throughout, so no IRQ dispatch enters
  the scheduler before the detach completes.

**Risk notes:**

- If `list_for_each_entry_safe(&rq->cfs_tasks)` encounters a
  partially-linked sched_entity (e.g. from a fork race), it may
  dereference a stale `group_node` pointer. Mitigation: the pre-
  fork block via `os_snapshot_block_iter_signals` keeps the
  parent from mutating the list during fork; only fully-queued
  entries are present at the detach point. If this proves
  fragile in practice (e.g. under SMP, which commit 3d-d does
  not yet support), add a defensive pointer-sanity check before
  dereferencing `se->group_node`.
- CONFIG_FAIR_GROUP_SCHED: fuzz profile currently inherits
  defconfig; verify at build time whether group scheduling is
  on. If yes, `cfs_tasks` is per-group, and the single-root walk
  may miss nested groups. Mitigation: for-each also over cfs_rqs
  in the root task_group. Defer this nuance to a follow-up if it
  turns out to matter; fuzz profile should eventually disable
  FAIR_GROUP_SCHED for other reasons too.

**Lifetime:** Until upstream lands a freezer-cgroup-based v2 per
D41's revisit triggers, at which point this helper becomes dead
code and is removed in the same series.

**Revisit triggers:**

- Upstream sched maintainers reject the helper on review. We
  fall back to the A2 "scoped-down 3d-d" path and document the
  limitation in commit 6.
- FAIR_GROUP_SCHED edge cases surface in real testing. Helper
  grows to walk nested cfs_rqs.
- v2 freezer-cgroup design arrives (per D41). Helper is removed
  in the same series that lands the freezer path.
- A different scheduler class (deadline / rt / idle) gains queued
  tasks we didn't anticipate. Helper extends to detach those
  classes too.

**Cross-references:**

- `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md`
  §"Commit plan" — 3d-d paragraph updated in this series.
- D40 — commit-3d split.
- D41 — UML signal-gating contract. Helper relies on
  `signals_enabled == 0` during the detach.
- Commit `9d0dd8ed3181` (3d-c) — demonstrated worker runs guest
  code; crash at `__set_next_task_fair+0x11b` is the trigger for
  this decision.
- `kernel/sched/fair.c:13825` — `__set_next_task_fair` (crash site).
- `kernel/sched/fair.c:7293` — `dequeue_entities` (crash stack
  frame).
- `kernel/sched/core.c:2211` — `deactivate_task` (the primitive
  the helper wraps).
- `kernel/sched/core.c:8771` — `dump_rq_tasks` (existing
  precedent for iterating `rq->cfs_tasks` under rq_lock).
- `arch/um/kernel/reboot.c:20-37` — existing arch/um precedent
  for walking the task list.

---

## (Future entries here, as decisions are made)
