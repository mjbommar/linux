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

## (Future entries here, as decisions are made)
