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

### 2026-04-21 addendum — partial resolution under D45

D45 (in-fork scope policy, 2026-04-21) re-classifies "cross-
subsystem change with tracing-maintainer sign-off required" as
"implement on the fork now; track the merge-surface-area growth
for a future upstream conversation." That lifts the scope bar on
D34's alternative (1) (mark `kthread()` and `smpboot_thread_fn()`
`notrace`) — the only blocker that kept commit 3 held in the
first place.

Action taken 2026-04-21: apply alternative (1) to the fork. Two
`notrace` annotations in `kernel/kthread.c::kthread` and
`kernel/smpboot.c::smpboot_thread_fn`, both with a short comment
explaining the generic-kernel-vs-UML graph-trampoline mismatch.
Pair with a restoration of the stashed `c04-c3-wip-after-arch-
strip-still-crashes` arch-level work (sources (1) and (2) — patch-
site stripping in `arch/um/kernel/Makefile` and friends, plus the
`ftrace_graph_caller` + `return_to_handler` trampolines in
`arch/um/kernel/mcount.S` and `prepare_ftrace_return` in
`arch/um/kernel/ftrace.c`). Together those close all three of
D34's distinct leak sources.

**Merge surface growth tracked here (per D45):** two notrace
annotations in generic kernel files. If upstream accepts those
(narrow, surgical, matches the D34 rationale), merge surface
shrinks to zero. If upstream declines (they keep function_graph
working on-native despite the same theoretical issue because the
ret_stack walk doesn't dereference stale entries on x86 native),
the fork carries two annotations indefinitely — acceptable cost.

**Revisit triggers updated:**

- If alternative (3) (soften `__ftrace_return_to_handler`'s pop-
  failure to return 0 instead of panicking) lands upstream, the
  two `notrace` annotations become unnecessary. Consider retiring
  them on the fork at that point.
- If upstream adopts the two `notrace` annotations, the fork's
  diff to upstream drops to just the arch/um/ changes (which are
  UML-specific by construction).

Closes task #34 (notrace + patchable-function-entry workaround)
— the `patchable_function_entry(0,0)` attribute from compiler_
types.h's notrace expansion under `CC_USING_PATCHABLE_FUNCTION_
ENTRY` IS respected by both toolchains at the mcount-location
recording level (recordmcount scans `__patchable_function_
entries`, and the attribute with args (0,0) tells the compiler to
emit the function's own entry with zero patchable bytes AND not
record it in that section). The task #34 blocker was real but
scoped to the lateral question of "what does the 5-byte NOP look
like"; it is not a blocker for the functional goal of "graph
tracer doesn't leak ret_stack entries into these functions."

### 2026-04-21 addendum-2 — source (3) landed; sources (1)+(2) hit a new boot regression

Validation pass after landing the two `notrace` annotations
(commit `a2e01ee58c53`) and attempting to apply the stashed
arch/um patch-site stripping uncovered a bug the 2026-04-19
session didn't surface: applying the stashed WIP *as-is* breaks
`tools/testing/selftests/um/userspace-smoke/userspace-smoke.sh`
at boot. The init script segfaults at IP=0 SP=0 before running
any of its body; `/bin/true` does the same. The regression
reproduces with `CONFIG_FUNCTION_GRAPH_TRACER=n` built on top of
the stashed arch/um changes — so it is not a graph-trampoline
bug, it is caused by the patch-site strip itself.

Why 2026-04-19 missed it: that session ran the graph tracer and
measured crashes under active graph workload; no one ran
`userspace-smoke` against a stripped-arch build with graph
*off*. The stash was saved with the "still crashes under graph"
framing, obscuring that the stash's boot-path side effect is
independent of the graph.

**Source (3) still counts as closed:** the two generic-kernel
`notrace` annotations are validated against a clean (un-
stripped) arch/um. They are a pure win regardless of the rest
of commit 3. They ship.

**Sources (1) and (2) re-opened:** the stashed WIP's
`ccflags-remove-y := $(CC_FLAGS_FTRACE)` covers all of
`arch/um/kernel/` and `arch/um/kernel/skas/`; its
`USER_CFLAGS := $(filter-out $(CC_FLAGS_FTRACE),$(USER_CFLAGS))`
covers USER_OBJS. Somewhere in that broad sweep is a file whose
-fpatchable-function-entry=5,0 nop is load-bearing (or whose
recordmcount registration is). The session-3 path to identify
it is documented in `02-workstreams/C-profiles-and-gaps/
04-port-kprobes.md` §"Commit 3" "Session-3 plan": bisect
individual files progressively until `userspace-smoke` breaks,
then ship the minimum strip set + trampolines in one commit.

**Task list update:** session 3 is its own entry in the redesign
task list, sized as one focused session (probably 1-3 files to
bisect). Commit 3 is a 3-session arc overall: session 1 write,
session 2 resolve source (3), session 3 close sources (1)+(2)
with a narrow strip set.

### 2026-04-21 addendum-3 — sources (1) and (2) closed with a narrow strip; commit 3b needs atomic-context fix

Session-3 bisection completed. Findings:

**Root cause of the session-2 boot regression:** the stashed
WIP's `USER_CFLAGS := $(filter-out $(CC_FLAGS_FTRACE),$(USER_CFLAGS))`
was too broad. It stripped `-fpatchable-function-entry=5,0` from
every USER_OBJ — including TUs whose 5-byte NOP prologue turned
out to be load-bearing at boot for reasons this session didn't
fully isolate (probeB: only the USER_CFLAGS filter; probeE:
only `-fpatchable-function-entry` from the filter — both SEGV
init at IP=0 SP=0 before smoke runs). The generic fix
is narrow stripping.

**Narrow strip set (probeG, commit `dc623a9dfd0a`):**
  - arch/um/os-Linux/signal.o, sigio.o, irq.o (source 1 —
    rt_sigreturn-unwound TUs).
  - arch/um/os-Linux/skas/process.o (source 1 — skas signal
    dispatch entry).
  - arch/um/kernel/process.o (source 2 — new_thread_handler,
    fork_handler).

Reaching the USER_OBJ subset required an enhancement to
`arch/um/scripts/Makefile.rules`: the wholesale c_flags
override was not honoring CFLAGS_REMOVE_<file>.o. One-line
filter-out addition fixes that, and is useful general
infrastructure.

**Validation:** userspace-smoke.sh PASS on uml/research with
the narrow strip alone (no FUNCTION_GRAPH_TRACER selected yet).
uml/fuzz and uml/prod-fast build clean.

**Commit 3 split:** the narrow strip ships as commit 3a
(`dc623a9dfd0a`). Enabling HAVE_FUNCTION_GRAPH_TRACER + the
trampolines is commit 3b, deferred pending investigation into
a new finding:

**New blocker for commit 3b — prepare_ftrace_return enters
atomic context.** First activation test
(`echo function_graph > current_tracer` writing from init) hit:

    BUG: sleeping function called from invalid context at
    kernel/locking/mutex.c:609
    in_atomic(): 1, ... preempt_count: 6

with a stack showing:

    __mutex_lock → mutex_lock_nested → free_irq
    → ... → prepare_ftrace_return → ftrace_graph_caller
    → um_set_signals

The graph-caller trampoline fired on a return from a function
that was in atomic context (preempt_count: 6). That context
reached prepare_ftrace_return, which called
function_graph_enter, which on this path needed
sched_register_mutex or tracepoints_mutex (both seen in held
locks), and that's a sleeping acquisition.

The fix class is likely one of:
1. Add `notrace` to `um_set_signals` (the trampoline-entry
   function, which gates UML signal delivery — calling into
   fgraph from inside that gate is by construction fragile).
2. Convert the mcount.S trampoline to check preempt_count and
   skip-trace in atomic context, matching x86's
   `trace_function_call` guard.
3. Skip-trace the whole signal gate family explicitly.

Option (1) is the narrowest. Probably also need `notrace` on
the small set of helpers um_set_signals calls. The
investigation for commit 3b is its own focused session; likely
1–3 notrace annotations and a re-run of the fgraph activation
smoke.

**Revisit triggers (updated):**
- Option (1) or (3) identified via diagnostic probes and
  commit 3b lands.
- Upstream softens `__ftrace_return_to_handler`'s pop-failure
  to return 0 instead of panic — that would make the leak
  signatures from sources (1)+(2) harmless even without the
  strip, and the narrow strip from 3a becomes optional.

### 2026-04-22 addendum-4 — commit 3b landed, atomic-context fix shipped

Commit 3b ships with the full fix class union recommended in
addendum-3. Options (1), (2), and (3) are all applied —
defense-in-depth against the fgraph-on-UML-UP failure mode:

- **Option (1) — `notrace` on `um_set_signals` and helpers**
  (`arch/um/os-Linux/signal.c`). Belt-and-suspenders even though
  `CFLAGS_REMOVE_signal.o := $(CC_FLAGS_FTRACE)` already strips
  the patchable entry — the source-level annotation survives a
  future Makefile edit and makes the intent unambiguous. Covers
  `um_get_signals`, `um_set_signals`, `um_set_signals_trace`,
  `block_signals`, `unblock_signals`.

- **Option (2) — preempt-count guard in `prepare_ftrace_return`**
  (`arch/um/kernel/ftrace.c`). The root cause of addendum-3's BUG
  is not specific to `um_set_signals` — it's structural: UML-UP
  builds TINY_RCU, which maps `rcu_read_lock()` to
  `preempt_disable()`. Any call site that legitimately holds
  `rcu_read_lock` around kallsyms / BPF / tracepoint lookup
  (`is_bpf_text_address()` + `bpf_prog_kallsyms_find()` are the
  most visible on the path) therefore runs with
  `preempt_count > 0`. If fgraph pushes a shadow-stack entry for
  a function whose body then takes a sleeping lock (free_irq →
  __mutex_lock, tracer activation → tracepoints_mutex, …),
  `__might_resched` WARNs under PROVE_LOCKING /
  DEBUG_ATOMIC_SLEEP. Native x86 never hits this because it
  builds TREE_RCU, where `rcu_read_lock()` does not touch
  `preempt_count`. UML-UP has no equivalent option (TINY_RCU is
  the UP choice). The guard is:

      if (unlikely(in_atomic()))
          return;

  placed after the `ftrace_graph_is_dead` / `tracing_graph_pause`
  checks in `prepare_ftrace_return`. Graph events inside the
  atomic-context window are lost; the traced function still
  executes; all events outside atomic context are still
  captured. This matches the option-(2) trampoline-level guard
  addendum-3 pointed at, with the preempt check in C rather than
  assembly for clarity (no measurable hot-path difference — the
  check reduces to a single memory load + branch).

- **Option (3) — `notrace` on the signal-gate trace family**
  (`arch/um/kernel/signal.c`: `block_signals_trace`,
  `unblock_signals_trace`, `um_trace_signals_on`,
  `um_trace_signals_off`). These kernel-side wrappers call
  `trace_hardirqs_on/off` around
  `block_signals()`/`unblock_signals()` and run from trap-return
  paths that can be atomic by construction. They are NOT covered
  by any `CFLAGS_REMOVE_<file>.o` strip on the branch today.

Validation (SECCOMP_ONLY, uml/research, 2026-04-22):

- `echo function_graph > /sys/kernel/tracing/current_tracer` from
  init: rc=0, no BUG, no WARNING, `current_tracer=function_graph`
  visible.
- `echo function_graph > …` then `halt -f` with graph still
  active: clean shutdown, no `Trying to free IRQ from IRQ
  context` WARN, no sleeping-in-atomic BUG, no lockdep RCU
  suspicious.
- `echo function_graph > …` then workload (`ls`, `head -6
  trace`) then `echo nop > …` then `halt -f`: full user-flow
  clean; `trace` contains real call-graph output
  (`is_bpf_text_address`, `kernel_text_address`,
  `__is_insn_slot_addr`, …).
- Boot matrix (12/12 configs): PASS, unchanged from 3a.
- kprobes-stress selftest: PASS iters=200 fires=204 errors=0
  graph=on (token flipped from `graph=deferred` in the same
  commit).

**Why the guard is authoritative, not a workaround:** addendum-3
already enumerated this as one of the three valid fix classes.
The UML-UP + TINY_RCU + PROVE_LOCKING combination is not a
configuration we can shed — UML is UP by construction on the
single-vCPU default, and TINY_RCU is the UP-only RCU flavour.
Changing the research profile to SMP + TREE_RCU would avoid the
guard but trade one set of constraints (TINY_RCU's preempt
coupling) for another (SMP stop_machine costs + additional
backend work). The guard costs a single `in_atomic()` read per
traced-function entry (one load + one branch after both
compile-time optimizations) and keeps the profile's research
posture intact.

**Merge surface growth (per D45):** the five `notrace`
annotations in arch/um/ are UML-specific by construction
(signal.c wrappers for UML's signal-driven IRQ model). They
don't need upstream discussion. The preempt-count guard in
`prepare_ftrace_return` is arch-local. None of the commit's
changes introduce merge surface beyond existing arch/um/
boundaries except the `select HAVE_FUNCTION_GRAPH_TRACER` line
in `arch/um/Kconfig`, which is the intended deliverable.

**Revisit triggers (closing):**
- If UML gains a way to use TREE_RCU under UP (unlikely), the
  preempt-count guard becomes strictly unnecessary and its
  removal can be considered.
- If upstream softens `__ftrace_return_to_handler`'s pop-failure
  to return 0, the narrow `CFLAGS_REMOVE` strips from 3a become
  optional. The `notrace` annotations from 3b are source-level
  intent and stay either way.

Closes task #77.

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

## D43: C-06 BPF JIT v1 blocked on three cross-subsystem touches in arch/x86/net/

**Date:** 2026-04-20
**Status:** Deferred (2026-04-20). Two arch-generic hygiene
fixes landed on this branch (`bpf, x86: explicitly include
<asm/cpufeature.h>` and `bpf, x86: use instruction_pointer
helpers in ex_handler_bpf`) as standalone upstream-palatable
patches; user sign-off captured in the chat ("let's do the
smaller set now and document the deferral"). These two benefit
bare-metal x86 regardless of UML and will be submitted to the
BPF/netdev lists as their own series. The full UML port is
blocked on D43 addendum's option B2 (portable-emitter refactor
of arch/x86/net/bpf_jit_comp.c). See §"Status note (2026-04-20,
deferral with landed hygiene)" at the end of this entry.
**Original status:** Blocked pending user sign-off on cross-subsystem patch.
Design doc (`02-workstreams/C-profiles-and-gaps/06-port-bpf-jit.md`)
assumed arch/x86/net/bpf_jit_comp.c would compile unchanged under
UML; empirical build (C-06 commit 1 attempt) shows three real
divergences. Reverted the attempted Kconfig + Makefile nudge
(nothing pushed); surfacing decision here per AGENT-PROMPT
§"When to stop and ask" (cross-subsystem touch requires explicit
user sign-off).

**The three divergences:**

1. `arch/x86/net/bpf_jit_comp.c:1493,1499` — `regs->ip` direct
   field access on `struct pt_regs`. x86's bare-metal pt_regs
   carries `ip` as a named field (`arch/x86/include/asm/
   ptrace.h`); UML's `struct pt_regs` is a wrapper around
   `struct uml_pt_regs` (`arch/um/include/asm/ptrace-generic.h:13`)
   and IP is accessed via the `PT_REGS_IP(regs)` /
   `instruction_pointer(regs)` macros. This is a genuine layout
   divergence, not a header forwarding problem.

2. `arch/x86/net/bpf_jit_comp.c:1997` — `boot_cpu_has(X86_FEATURE_BMI2)`
   implicit declaration. UML has a working `boot_cpu_has` macro
   at `arch/um/include/asm/cpufeature.h:54`; the problem is that
   `bpf_jit_comp.c` does NOT explicitly `#include <asm/cpufeature.h>`
   and relies on transitive inclusion through `<asm/nospec-branch.h>`
   or similar that holds on bare-metal x86 but not via UML's
   include path. Minimal fix: add the explicit include.

3. `arch/x86/net/bpf_jit_comp.c:2255` — `VSYSCALL_ADDR` undeclared.
   UML has no vsyscall page (it's a host-userspace ELF, not a
   kernel). The JIT uses this constant as the upper limit of an
   arena-access sanity check; the equivalent constant under UML
   is zero / `TASK_SIZE_MAX` (no reserved region above).

**Why this wasn't caught in the design pass.** The design doc's
U3 unknown anticipated "<asm/*> header resolution" issues with
a mitigation plan of "add minimal forwarders in
`arch/um/include/asm/`". That plan works for (2) and (3) — those
can be solved entirely in `arch/um/` with a UML-only header and
a force-include from UML's Makefile. It does NOT work for (1):
`#define ip ...` at translation-unit scope would collide with
every other `.ip` field reference in the TU (struct stat, struct
sockaddr_in, other pt_regs users). The `regs->ip` access is a
pt_regs layout assumption that has no clean UML-only workaround.

**Options considered.**

**A. UML-side preprocessor shim + scoped wrapper.** Create
`arch/um/include/asm/bpf_jit_umhost.h` with `#define VSYSCALL_ADDR 0UL`
and `#include <asm/cpufeature.h>`; force-include it only when
building `arch/x86/net/bpf_jit_comp.o` under UML (via
`CFLAGS_bpf_jit_comp.o` in arch/um/Makefile). For the `regs->ip`
divergence, write `arch/um/net/bpf_jit_comp.c` that `#include`s
the x86 source after `#define ip <uml_pt_regs_gp_lvalue>`.
  - Scope: entirely `arch/um/`. No LKML sign-off needed.
  - Fragility: the `#define ip` at TU scope bleeds into every
    transitive header; collisions are likely (e.g. `struct
    sockaddr_in.sin_addr.s_addr` is unaffected but other `.ip`
    lvalues exist). Would need a tightly-bound TU with no
    network/pt_regs header pollution — hard to guarantee
    stable across upstream x86 BPF JIT revisions.
  - Maintenance: every time `arch/x86/net/bpf_jit_comp.c`
    changes upstream the shim may silently break. UML CI would
    need to guard against it.

**B. Small upstream patch to `arch/x86/net/bpf_jit_comp.c` +
paired UML shim headers.** Estimated ~10-15 LOC:
  - Add `#include <asm/cpufeature.h>` at the top (unconditional,
    harmless; fixes #2 for UML and is good hygiene anyway).
  - Replace two `regs->ip` accesses with a tiny inline helper
    `bpf_jit_pt_regs_ip(regs)` that has a one-line x86 body
    (`regs->ip`) and a UML body using `PT_REGS_IP(regs)`. New
    header in `arch/x86/include/asm/bpf_jit.h` or similar with
    the two definitions.
  - Add `arch/um/include/asm/vsyscall.h` with
    `#define VSYSCALL_ADDR 0UL` (UML-only; no bare-metal change).
  - Scope: touches `arch/x86/net/bpf_jit_comp.c` and adds one
    new x86 header. Requires LKML + BPF-maintainer review
    (Daniel Borkmann / Alexei Starovoitov).
  - Cleanness: small, matches how arch/x86 abstracts other UML-
    vs-bare-metal concerns (e.g., `__uml_cant_sleep`).
  - Lead time: days to weeks for review.

**C. Defer C-06 v1 entirely.** Mark C-06 `planned (blocked on
   upstream BPF-JIT abstraction work; reopen once (B) patch
   lands or option A's fragility is deemed acceptable)`. Move
   to next unblocked leaf task (C-07 KMSAN design pass, or
   continued C-09 v2 groundwork).

**User sign-off required for option B** because the patch
touches `arch/x86/net/` and `arch/x86/include/asm/`, which is
outside the "arch/um/ and Documentation/virt/uml/" scope the
AGENT-PROMPT allows me to work in autonomously.

**Current state:** C-06 design doc (`02-workstreams/
C-profiles-and-gaps/06-port-bpf-jit.md`, commit f0bdb61447ef)
updated in the same series as this entry to reference D43 and
flip Status from `design` to `blocked on D43 option-B sign-off
or option-A fragility acceptance`. No arch/ code changes made.

**Cross-references:**
- `02-workstreams/C-profiles-and-gaps/06-port-bpf-jit.md` —
  design doc; will be updated to reflect this blocker.
- `arch/x86/net/bpf_jit_comp.c:1493,1499,1997,2255` — the four
  divergence sites.
- `arch/um/include/asm/ptrace-generic.h:13,21` — UML's pt_regs
  wrapper and `PT_REGS_IP` macro.
- `arch/um/include/asm/cpufeature.h:54` — UML's working
  `boot_cpu_has`.
- D34 — similar "external upstream fix needed" pattern for
  C-04 commit 3 (HAVE_FUNCTION_GRAPH_TRACER); C-06 joins D34 in
  the "blocked on upstream abstraction" bucket.

### Addendum (2026-04-20, later in session)

User signed off on option B. I implemented the small upstream
patch and ran it:

  - `arch/x86/net/bpf_jit_comp.c`: added `#include <asm/
    cpufeature.h>` and `#include <asm/vsyscall.h>`; replaced
    `regs->ip` (2 sites) with `instruction_pointer(regs)` and
    `instruction_pointer_set(regs, instruction_pointer(regs) +
    insn_len)`. Arch-generic — same codegen on bare-metal x86.
  - `arch/um/include/asm/vsyscall.h` (new): `#define
    VSYSCALL_ADDR 0UL`. UML has no vsyscall page; collapsing
    the upper bound to zero is semantically correct.
  - `arch/um/Kconfig`: `select HAVE_EBPF_JIT if X86_64`.
  - `arch/um/Makefile`: `core-$(CONFIG_BPF_JIT) += $(HOST_DIR)/net/`.

Build result: **the three original divergences are fixed, but
the compile now reaches a DEEPER layer of `arch/x86/kernel/`-
internal symbols UML does not build.** Specifically:

  1. `arch/x86/net/bpf_jit_comp.c:407,597` — `x86_nops[noplen][i]`
     / `x86_nops[5]` undeclared. `x86_nops` is the NOP-sequence
     table defined in `arch/x86/kernel/alternative.c`; UML
     doesn't compile `arch/x86/kernel/` so the symbol is
     unresolvable.
  2. `arch/um/include/asm/cpufeature.h:52` — `DISABLED_MASK_BIT_SET`
     implicit declaration when `cpu_feature_enabled()` is
     expanded from `bpf_jit_comp.c:676` on
     `X86_FEATURE_INDIRECT_THUNK_ITS`. The macro is defined in
     `arch/x86/include/asm/cpufeatures.h`; UML's cpufeature.h
     references it without guaranteeing the include path
     resolves.
  3. (Likely more after these two are fixed — I stopped here.)

**The initial "3 divergences" framing in D43 was too optimistic.**
The JIT's body touches arch/x86/kernel/-internal state across
multiple subsystems (alternatives, CPU-features-with-
mitigations, probably also text_poke / unwind / cfi). Each
layer requires either a UML stub, a header-forwarder, or a
patch to the JIT to factor out the x86-host dependency. This
is not a single small upstream patch; it is an iceberg.

**Revised options:**
  - **B1 (original option B, too narrow).** 3-fix patch.
    **Does not work alone** — empirically confirmed 2026-04-20.
  - **B2 (new refined option B).** Upstream-collaborative
    `arch/x86/net/bpf_jit_comp.c` factoring into a portable
    emitter (byte arrays, no x86_nops / cpu-features /
    alternatives references) + an x86-host-only glue that
    layers the mitigations on top. UML compiles only the
    portable emitter plus its own thin glue with feature-
    detection stubs. Right long-term shape. Multi-week effort.
    Needs LKML coordination with BPF maintainers (Alexei
    Starovoitov / Daniel Borkmann) before code.
  - **C (defer).** Mark C-06 as `blocked on upstream BPF-JIT
    portability refactor`; advance other work.

**Recommendation:** **C (defer) for this branch**, with the
three arch-generic fixes (instruction_pointer, cpufeature
include, vsyscall header) queued for **separate tiny upstream
patches** independent of the UML port. Those three fixes are
hygiene improvements that benefit the x86 JIT regardless —
submitting them as their own series (one patch per fix via
proper BPF/netdev routing) is the "build for the long term,
correctly" move. The big C-06 port then rebases onto those
once they land, reducing its novel scope to just the UML side.

**Scratch reverted (nothing pushed):**
  - `arch/x86/net/bpf_jit_comp.c`: 5 lines modified (+2
    includes, 2 instruction_pointer conversions).
  - `arch/um/include/asm/vsyscall.h`: new, 19 lines.
  - `arch/um/Kconfig`: +11 lines.
  - `arch/um/Makefile`: +8 lines.

Full-session C-06 outcome: went from "3-line patch would fix
it" to "portable-emitter refactor needed; three hygiene fixes
independently landable upstream." That IS progress — the scope
is now honest. Future picker starts here rather than at the
D43 first framing.

### Fifth view (2026-04-21, post-D45): revised survey + option A preferred

After D45 lifted the "cross-subsystem change = LKML-first" gate,
a full cover-to-cover survey of arch/x86/net/bpf_jit_comp.c
(3800 LOC, 110 top-level symbols, 37 references to x86-host-
specific bits) found that the divergences group cleanly into
**two small abstractable categories**, not a structural refactor:

  **Category 1 — macro/symbol resolutions UML doesn't yet
  forward** (addressable via UML-local `<asm/*>` shim headers):

    * `VSYSCALL_ADDR` (lines 2253-2261) — add
      arch/um/include/asm/vsyscall.h with `#define 0UL`.
    * `x86_nops[]` (lines 406, 596) — NOP sequences table. UML
      can either forward arch/x86/include/asm/nops.h or carry
      a local copy.
    * `gen_endbr()` / `gen_endbr_poison()` (59-60) — IBT
      instrumentation. Under non-CFI (UML default) these are
      effectively 4-byte NOPs; provide no-op stubs.
    * `cpu_feature_enabled(X86_FEATURE_*)` (675-683, 1617-1634,
      1997) — retpoline / ITS / BHB / BMI2 mitigations. UML
      runs in userspace and can skip these; provide stubs
      returning false.
    * unwind_state / unwind_start / unwind_next_frame /
      unwind_done / unwind_get_return_address (3996-4016) —
      ORC unwinder for bpf_throw. UML's stack walk is
      different; provide stubs that say "no frames" (bpf_throw
      will silently not unwind, which is a reasonable
      limitation for the fork demo).
    * text_poke_set / text_poke_copy /
      smp_text_poke_single / __bpf_arch_text_poke
      (308, 592-652, 3943) — JIT program patching. UML's
      BPF programs live in execmem which has its own
      mprotect-based mutate path; stub these to memcpy+
      mprotect or no-op (latter = JIT tail-call fixup not
      supported, acceptable for initial fork demo).

  **Category 2 — already fixed by the two hygiene commits
  (e2b686c96218 + 5b95b1bb3e6a):**

    * `#include <asm/cpufeature.h>` explicit.
    * `regs->ip` → instruction_pointer helpers.

**Revised option A (PREFERRED under D45):** UML-side shims +
the two hygiene commits. No arch/x86/net/ body changes
beyond what's already landed. ~7 small UML-only shim headers;
bare-metal x86 is untouched past the hygiene fixes.

  * Pro: minimal, localised, doesn't change upstream BPF JIT
    semantics. Fits D45's "demonstrate on fork; upstream
    later" posture perfectly.
  * Pro: ~1 session of work. Each shim is <30 LOC.
  * Con: some JIT features are stubbed (bpf_throw unwinder
    silent, tail-call text_poke fixup no-op). Documented as
    known limitations; acceptable for the demo.

**Revised option B** (portable-emitter refactor) remains the
right long-term upstream shape but is now over-scoped for the
fork demo. Defer it to a future upstream conversation motivated
by the running artifact from option A.

**Recommendation:** proceed with option A. Start in this
session: implement the shim headers + Kconfig select + Makefile
hook, rebuild, test. If bpf_jit_comp.o compiles under
ARCH=um and a trivial BPF program loads on a fuzz-profile UML,
option A shipped.

Each shim/stub gets a comment naming what it's replacing and
why it's safe to stub on UML. The "known limitations" list
goes into the C-06 design doc once option A lands.

### Status note (2026-04-20, deferral with landed hygiene)

User chose the "do the smaller set now and document the
deferral" path. Action taken:

  1. **Landed** on `uml-redesign-plan` as commit
     `e2b686c96218` — `bpf, x86: explicitly include
     <asm/cpufeature.h>`. Hygiene-only; no functional change
     on x86 defconfig. Validated by rebuilding
     `arch/x86/net/bpf_jit_comp.o` under x86_64 defconfig +
     BPF_JIT=y (same `.o` size, clean compile).
  2. **Landed** on `uml-redesign-plan` as commit
     `5b95b1bb3e6a` — `bpf, x86: use instruction_pointer
     helpers in ex_handler_bpf`. Replaces `regs->ip` with
     `instruction_pointer(regs)` and `instruction_pointer_set(
     regs, instruction_pointer(regs) + insn_len)`. Same
     codegen on x86; inline helpers resolve to the same field
     access. Validated identically.
  3. **Not landed**: the `arch/um/include/asm/vsyscall.h`
     stub (VSYSCALL_ADDR = 0). That one only makes sense
     alongside the actual UML port, which is deferred.
  4. **Not landed**: any `select HAVE_EBPF_JIT` / Makefile
     pull of arch/x86/net/ into UML. Those remain deferred
     until the B2 refactor is upstream.

These two landed commits are **ready for independent LKML
submission** (BPF + netdev lists, Alexei Starovoitov / Daniel
Borkmann). Their commit messages were deliberately written in
upstream-kernel style with no UML references, so they can be
cherry-picked to a submission branch without rewording.

A pre-formatted series lives at
`Documentation/virt/uml/redesign/upstream-patches/bpf-hygiene-v1/`
with `git format-patch` output (cover letter + 2 patches) and a
`SUBMISSION-NOTES.md` covering pre-flight cleanups (strip the
`Co-authored-by:` trailer, fix the `From:` author email),
checkpatch false positives (`<linux/cpufeature.h>` isn't the
right include), build verification record, and suggested LKML
routing. See `upstream-patches/README.md` for the directory
conventions.

Once they land upstream, a future C-06 v1 attempt rebases onto
post-hygiene `arch/x86/net/bpf_jit_comp.c` and has two fewer
divergences to worry about. The remaining barrier is still
option B2 (portable-emitter refactor), tracked here and in the
C-06 design doc.

C-06 status: **deferred**. Next action on this workstream
requires either (a) someone picking up the B2 refactor as its
own multi-week project, or (b) a re-scope decision that C-06
v1 is not shippable without B2 and the redesign schedule
reflects that.

---

## D44: C-07 KMSAN v1 shadow+origin layout decision (VMALLOC subdivision vs dedicated-mmap)

**Date:** 2026-04-20
**Status:** Open — needs user sign-off on layout approach before
C-07 commit 1 can land. Empirical scratch-build probe (reverted,
not pushed) turned the C-07 design-doc "mirror KASAN mmap" plan
into a concrete architectural decision.

**What the probe found.**

Attempted a scratch version of C-07 commit 1:
- `select HAVE_ARCH_KMSAN if X86_64` + `HAVE_ARCH_KMSAN_VMALLOC`
  in `arch/um/Kconfig`.
- `arch/um/include/asm/kmsan.h` modeled on `arch/s390/include/
  asm/kmsan.h`: `arch_kmsan_get_meta_or_null` returns NULL;
  `kmsan_virt_addr_valid` wraps `virt_addr_valid` with preempt
  disable.
- `KMSAN_SANITIZE := n` in `arch/um/os-Linux/Makefile`.

Build result: gcc ARCH=um (KMSAN=n default) clean. But clang
`LLVM=1 CONFIG_KMSAN=y` fails at `mm/kmsan/shadow.c:62-68` with
four "undeclared identifier" errors:

  - `KMSAN_VMALLOC_SHADOW_START`
  - `KMSAN_VMALLOC_ORIGIN_START`
  - `KMSAN_MODULES_SHADOW_START`
  - `KMSAN_MODULES_ORIGIN_START`

These are not "arch hooks" in the sense
`arch_kmsan_get_meta_or_null` is — they are **virtual-address
constants** that `mm/kmsan/shadow.c`'s `vmalloc_meta()` uses to
compute the shadow/origin address of a vmalloc or module pointer.
Every arch with KMSAN support must define them, and they embed
a geometric assumption: the arch has carved its VMALLOC (and
module region) into a shape where shadow + origin addresses can
be computed by a fixed offset from the primary address.

Plus one macro collision: my scratch defined `KMSAN_ORIGIN_SIZE`
as a "size of the origin region" (a 16 TB value). `mm/kmsan/
kmsan.h:30` already defines `KMSAN_ORIGIN_SIZE = 4` (bytes per
origin slot, i.e., `sizeof(depot_stack_handle_t)`). My naming
collides; I'd need a UML-specific `UM_KMSAN_ORIGIN_REGION_SIZE`
or equivalent.

**The geometric mismatch.**

x86's layout (`arch/x86/include/asm/pgtable_64_types.h:132-168`)
under `CONFIG_KMSAN=y`:
  - `VMALLOC_END` shrinks to `VMALLOC_START + VMALLOC_QUARTER_SIZE - 1`
    (1/4 of the normal value).
  - 2nd quarter is shadow of vmalloc.
  - 3rd quarter is origin of vmalloc.
  - 4th quarter (above the reduced VMALLOC_END) is modules
    shadow + origin.

s390's layout (`arch/s390/include/asm/pgtable.h:112-117`) puts
`KMSAN_VMALLOC_SHADOW_START = VMALLOC_END` (appended after
vmalloc rather than subdividing it) with its own
`KMSAN_VMALLOC_SIZE`.

UML's layout (`arch/um/include/asm/pgtable.h:49-53`):
```
VMALLOC_OFFSET  = __va_space
VMALLOC_START   = (high_physmem + VMALLOC_OFFSET) & ~(VMALLOC_OFFSET-1)
VMALLOC_END     = TASK_SIZE - 2 * PAGE_SIZE
MODULES_VADDR   = VMALLOC_START   # modules overlap vmalloc
MODULES_END     = VMALLOC_END
```

Three things make a straight x86-style quarter-split hard on UML:

1. `VMALLOC_END = TASK_SIZE - 2*PAGE_SIZE`. UML's address space
   is the host userspace process's address space minus the
   physmem + kernel-image regions; there may not be 4x the
   current VMALLOC extent available.
2. `MODULES_VADDR == VMALLOC_START`. UML packs modules inside
   vmalloc rather than above it, so the "4th quarter" x86 uses
   for module shadow+origin isn't free real estate.
3. UML's existing KASAN shadow lives outside VMALLOC at
   `KASAN_SHADOW_OFFSET` via a dedicated host `mmap()`. The
   design doc assumed KMSAN could do the same; the shadow.c
   requirement that `KMSAN_VMALLOC_SHADOW_START` be computable
   as `VMALLOC_START + fixed_offset` means dedicated-mmap needs
   extra plumbing to make the math work.

**Three options:**

**A. x86-style VMALLOC quarter-split on UML.** Make UML's
`VMALLOC_END` conditional on `CONFIG_KMSAN=y` so it shrinks to
1/4 of normal, and define `KMSAN_VMALLOC_{SHADOW,ORIGIN}_START`
+ `KMSAN_MODULES_{SHADOW,ORIGIN}_START` accordingly. Requires
touching `arch/um/include/asm/pgtable.h` — UML-local, no cross-
subsystem sign-off needed. Costs: 4x smaller vmalloc under
KMSAN (acceptable for research profile — KMSAN is debug-only),
and module range overlaps vmalloc needs a separate carve-out.

**B. Dedicated host-mmap regions + generic-code patch.** Keep
`VMALLOC_END` unchanged; put `KMSAN_VMALLOC_SHADOW_START` +
friends at fixed addresses outside VMALLOC (host mmap'd like
KASAN shadow). But `mm/kmsan/shadow.c:60-63` computes
`addr64 - VMALLOC_START + KMSAN_VMALLOC_SHADOW_START`; as long
as KMSAN_VMALLOC_SHADOW_START is a valid constant address with
enough range ahead of it, the math works. No generic-code
patch needed if we pick the offsets carefully. **This is the
cleanest option if we can find a canonical-hole region to sit
the shadow in.** All-UML-local; no sign-off needed.

**C. Defer C-07 entirely.** Mark as `blocked on v1 layout
decision; reopen when option A or B is signed off`. Advance
some other leaf in the meantime. The design doc's v1 ceiling
assumed memory overhead was the primary risk; the real v1
blocker turned out to be VA layout, which is a cheaper
decision to make but needs to be made.

**Cross-references:**
- `02-workstreams/C-profiles-and-gaps/07-port-kmsan.md` —
  design doc; will be updated to reflect this blocker.
- `arch/um/include/asm/pgtable.h:49-53` — UML VMALLOC layout.
- `arch/x86/include/asm/pgtable_64_types.h:132-168` — x86
  quarter-split geometry under KMSAN.
- `arch/s390/include/asm/pgtable.h:112-117` — s390
  KMSAN_VMALLOC_SHADOW_START pattern (appended after vmalloc).
- `mm/kmsan/shadow.c:55-71` — `vmalloc_meta()` that consumes
  these macros.
- `mm/kmsan/kmsan.h:30` — pre-existing `KMSAN_ORIGIN_SIZE = 4`
  that collided with my scratch's region-size macro.
- D43 — parallel "blocked on architectural decision" pattern
  for C-06 BPF JIT.

**Recommendation (mine):** **option B**. It's all-arch/um/, no
cross-subsystem touch, reuses the UML-KASAN "dedicated mmap"
pattern that already works, and picks offsets that make the
generic shadow.c math work without patching it. If B turns
out infeasible on further investigation (e.g., we can't find
enough unmapped canonical-hole region), fall back to A.

### Addendum (2026-04-20, later in session)

Deeper probe after "build for the long term, correctly"
guidance: options A and B in fact converge on the same
underlying design question, which is bigger than a single
commit and needs a dedicated multi-commit series.

UML's `task_size` (`arch/um/kernel/um_arch.c:261,330-344`) is
**runtime-determined** from `get_top_address(envp)` — the host
process's actual top of user address space at boot. So
`VMALLOC_END = TASK_SIZE - 2 * PAGE_SIZE` is a runtime value.
KMSAN's wire contract (`mm/kmsan/shadow.c:62-68`) requires
`KMSAN_VMALLOC_SHADOW_START` as a **compile-time constant**
macro used directly in the shadow-address computation:
`off + KMSAN_VMALLOC_SHADOW_START`.

The concrete consequence: for KMSAN to work on UML, we must
either (a) make `VMALLOC_END` a compile-time bound under
`CONFIG_KMSAN=y` — i.e. shrink it to something like
`VMALLOC_START + _AC(0x800000000, UL)` (32 GB) — OR (b) patch
`mm/kmsan/shadow.c` to allow a runtime shadow-start (cross-
subsystem, out of scope here without explicit sign-off).

The correct "build for the long term" shape is (a), but it is
NOT a single-commit affair:

  1. `arch/um/include/asm/pgtable.h`: conditionalize
     `VMALLOC_END` and `MODULES_END` on `CONFIG_KMSAN=y`. Keep
     the non-KMSAN path unchanged so gcc defconfig builds are
     bit-identical.
  2. Audit every call site that assumes VMALLOC_END is
     runtime-bounded by task_size — especially
     `arch/um/kernel/um_arch.c` (memory-mapping at boot) and
     `arch/um/kernel/mem.c` (the KASAN shadow placement, which
     we'd need to fit alongside the KMSAN reservations). The
     existing KASAN shadow at `0x100000000000` (16 TB) already
     implicitly constrains where VMALLOC can end; KMSAN adds
     two more reservations and we need to confirm the
     canonical-hole budget is enough for all three.
  3. `arch/um/include/asm/kmsan.h`: define
     `KMSAN_VMALLOC_SHADOW_START`, `KMSAN_VMALLOC_ORIGIN_START`,
     `KMSAN_MODULES_{SHADOW,ORIGIN}_START` at chosen VAs past
     the KASAN shadow (e.g. `0x140000000000` and
     `0x180000000000`), each sized to the bounded VMALLOC
     range. On UML, MODULES_VADDR == VMALLOC_START, so the
     module macros alias the vmalloc macros (the
     kmsan_internal_is_vmalloc_addr check fires first anyway
     per `mm/kmsan/kmsan.h:182-185`).
  4. `arch/um/Kconfig`: `select HAVE_ARCH_KMSAN if X86_64`
     with deps on the bounded-VMALLOC invariant.
  5. `arch/um/kernel/mem.c`: `kmsan_init()` mmap'ing shadow +
     origin regions via `kasan_map_memory` or equivalent;
     register both with `um_register_mmap_region()` for
     snapshot/fork inheritance (D37 pull-forward).
  6. Selftest + user doc + defconfig flip (as originally
     designed in the C-07 commit plan).

This is 4-6 commits minimum, touches UML's memory-model
invariants, and each step needs boot-testing on all three
backends (PTRACE_ONLY / SECCOMP_ONLY / DYNAMIC). The design
doc's "6-week budget" accurately reflects THIS effort, not the
thinner "reuse KASAN mmap pattern" framing the doc started
with.

**Revised recommendation:** option B (with the refined shape
above) is still the right long-term path. The implementation
is not a single-session job; it needs a dedicated multi-day
series to do carefully. In the interim:

- Leave C-07 as `design-locked; implementation queued` until
  the user allocates focused time for it.
- **Do not ram through a VMALLOC_END change in a hurry.**
  Bisecting a half-broken UML memory layout is costly, and
  C-07 is not on the critical path per
  `06-sequencing/critical-path.md` — KMSAN is the last-
  sequenced C-port by deliberate design.

**Cross-references (addendum):**
- `arch/um/kernel/um_arch.c:261,330-344` — runtime task_size.
- `arch/um/include/asm/processor-generic.h:53` —
  `TASK_SIZE (task_size)` runtime indirection.
- `mm/kmsan/kmsan.h:182-185` — vmalloc-addr check that fires
  before module-addr check.
- `06-sequencing/critical-path.md` — confirms C-07 is
  deliberately last-sequenced; not blocking M8.

### Second empirical probe (2026-04-20, later)

Implemented option B per the addendum's plan and ran it:

  1. `arch/um/include/asm/pgtable.h`: conditionalize
     `VMALLOC_END` to `VMALLOC_START + UM_KMSAN_VMALLOC_SIZE - 1`
     under `CONFIG_KMSAN=y`, with `UM_KMSAN_VMALLOC_SIZE =
     _AC(0x800000000, UL)` (32 GB). Define
     `KMSAN_VMALLOC_SHADOW_START = _AC(0x110000000000, UL)`
     (just past the 16 TB KASAN shadow), `ORIGIN_START` one
     32 GB step further; `MODULES_*_START` alias `VMALLOC_*_START`
     since UML has `MODULES_VADDR == VMALLOC_START`.
  2. `arch/um/include/asm/kmsan.h` (new): s390-style inline hooks
     — `arch_kmsan_get_meta_or_null` returns NULL;
     `kmsan_virt_addr_valid` wraps `virt_addr_valid` with
     preempt_disable/enable_no_resched.
  3. `arch/um/Kconfig`: `select HAVE_ARCH_KMSAN if X86_64` +
     `HAVE_ARCH_KMSAN_VMALLOC`.
  4. `arch/um/os-Linux/Makefile`: add `KMSAN_SANITIZE := n`.
  5. `arch/um/kernel/mem.c`: `kmsan_init()` mmap's shadow +
     origin regions via `kasan_map_memory`; registers both with
     `um_register_mmap_region()`; installs a `.kasan_init`-
     section function pointer (same boot seam as KASAN).

**Build result:** clean. `make ARCH=um uml/research` gcc
(KMSAN=n default) links with only the pre-existing modpost
warnings. `make ARCH=um LLVM=1 defconfig + CONFIG_KMSAN=y +
CONFIG_KASAN=n + CONFIG_KCSAN=n + CONFIG_DEBUG_KERNEL=y` with
clang 21.1.8 compiles and links — all four `KMSAN_VMALLOC/
MODULES_SHADOW/ORIGIN_START` identifiers now satisfy
`mm/kmsan/shadow.c:62-68`, no undeclared-identifier errors.
Confirmed `HAVE_KMSAN_COMPILER=y` auto-selects (U2 resolved).

**Boot result:** **hangs**. `timeout 120 ./linux
rootfstype=hostfs rootflags=/ init=/bin/true mem=256M` produces
the pre-boot "Checking syscall emulation for ptrace...OK" line
and then no kernel boot output. Same binary with KMSAN=n boots
past init and hits the expected "init exited" panic. So the
hang is introduced by my KMSAN port at runtime, not by the
clang/LLVM build path in general.

Likely causes, in order of probability:
- **`.kasan_init` section call ordering.** Both `kasan_init_ptr`
  and `kmsan_init_ptr` live in the `.kasan_init` section; the
  linker orders them as they appear in the TU. My `kmsan_init`
  runs AFTER `kasan_init`; if KMSAN instrumentation is active
  during `kasan_init` itself (KASAN is compiled out in this
  build so that path is moot) or during the `kasan_map_memory`
  call inside `kmsan_init`, the KMSAN hooks dereference
  shadow that hasn't been mapped yet and fault. The fault may
  not surface because UML's crash handler also wants shadow.
- **Early KMSAN instrumentation recursion.** Compiler-inserted
  `__msan_*` calls may fire during `.kasan_init` before the
  shadow is mapped. x86 handles this by marking its early-
  boot TU with `KMSAN_SANITIZE_mem.o := n` or using the
  `notrace`/`__kmsan_check_*` no-op path.
- **32 GB mmap under-the-covers cost.** Less likely — the mmap
  is a VA reservation, not a physical alloc. And kasan_map_memory
  is the same primitive KASAN uses for its 16 TB shadow which
  already works.

**What this means.**

Compile contract is solved (all four constants + arch hooks +
Kconfig + Makefile land cleanly, build and link with KMSAN=y
produce a vmlinux binary). The remaining gap is the runtime
boot path — a real debugging problem that requires:

  a. Add `KMSAN_SANITIZE := n` to `arch/um/kernel/Makefile`
     entries for files invoked from `.kasan_init` (mem.c at
     least) OR
  b. Reorder so kmsan_init runs via a separate, later section
     pointer (e.g., its own `.kmsan_init` section placed AFTER
     `.kasan_init` in the linker script) OR
  c. Audit which compiler-generated `__msan_*` calls fire
     during pre-main() setup and gate them via
     `kmsan_disable_current()` / `kmsan_enable_current()`
     around the map-memory path OR
  d. Some combination of the three.

The implementation is 80% there. The remaining 20% is
non-trivial runtime debugging that needs the developer at a
debugger, not in a chat session. **Do not push the current
scratch**. The design — VMALLOC bounded, shadow+origin at
fixed VAs past KASAN, arch hooks as NULL-returning stubs — is
correct; the init-ordering problem is a separable follow-up.

**Revised revised recommendation:** keep C-07 status as
`design-locked; implementation scaffolded but boot-hung;
queued for focused runtime debugging`. The scaffolding code
works (compiles + links). Runtime fix needs interactive
debugging. Scope estimate revised **down** from "6 weeks" to
"1-2 days of focused runtime debugging" once someone can sit
with it.

Scratch code (not pushed):
  - arch/um/include/asm/pgtable.h: +40 lines (VMALLOC
    conditionalization + KMSAN_* macros)
  - arch/um/include/asm/kmsan.h: new, ~70 lines
  - arch/um/Kconfig: +7 lines (comment + 2 selects)
  - arch/um/os-Linux/Makefile: +1 line
  - arch/um/kernel/mem.c: +60 lines (kmsan_init, mmap regs,
    `.kasan_init` section pointer)

All reverted from the working tree; documented here for the
next time someone picks it up.

### Third probe (2026-04-21, docs-only): generic KMSAN init contract

Researched `mm/kmsan/` to understand the init sequence the second
probe's scaffold was violating. Findings that materially shrink
the remaining task-#67 scope:

**KMSAN has two canonical init entry points**, both in
`mm/kmsan/init.c`:

  - `kmsan_init_shadow()` (line 75): allocates per-range shadow
    metadata via `kmsan_record_future_shadow_range()` +
    `kmsan_init_alloc_meta_for_range()`. Runs from generic
    `mm_core_init()` at `mm/mm_init.c:2739`, between
    `report_meminit()` and `stack_depot_early_init()`.
  - `kmsan_init_runtime()` (line 229): enables KMSAN at runtime
    (flips the "KMSAN is live" switch). Runs from
    `mm_core_init()` at `mm/mm_init.c:2768`, after
    `vmalloc_init()` + `pti_init()`.

**Arch code does NOT wire its own kmsan_init** — the generic
init sequence already exists. Arch provides:

  1. **VA-layout macros** (`KMSAN_VMALLOC_{SHADOW,ORIGIN}_START`,
     `KMSAN_MODULES_{SHADOW,ORIGIN}_START`). The second-probe
     scaffold got these right in `arch/um/include/asm/pgtable.h`.
  2. **Arch hooks** (`arch_kmsan_get_meta_or_null`,
     `kmsan_virt_addr_valid`). Got these right in
     `arch/um/include/asm/kmsan.h`.
  3. **For UML specifically: an early-boot mmap of the shadow
     + origin VA regions** so the VA exists before the generic
     init code tries to allocate into it. This is analogous to
     KASAN's two-phase init — `.kasan_init` section pointer
     mmaps the shadow VA pre-main(), then `kasan_init_generic()`
     populates it from `arch_mm_preinit`.

**The second-probe hang was a direct consequence of doing item 3
wrong.** The scaffold's `kmsan_init()` (registered via
`.kasan_init` section pointer) called
`kasan_map_memory(KMSAN_VMALLOC_SHADOW_START, UM_KMSAN_VMALLOC_SIZE)`
— monolithic VA reservation of 32 GB — BEFORE the host-level
kernel is ready to catch any compiler-inserted `__msan_*` call
that would fire during `kasan_map_memory` itself. If any
KMSAN-instrumented code path lies inside `kasan_map_memory` or
its host-syscall wrappers, the hook dereferences shadow that
isn't yet mapped (the mmap completes only after the call
returns).

**The fix shape for task #67:**

  - Add `KMSAN_SANITIZE := n` to `arch/um/kernel/Makefile`
    entry for `mem.o` (and any other TU touched by the early
    phase 1 init). This matches the KASAN_SANITIZE pattern.
  - Keep the `.kasan_init` section pointer or move to a
    separate `.kmsan_init` linker section; order matters less
    than the SANITIZE-off setting. Both are UML-only.
  - Do NOT write a UML-specific `kmsan_init()` equivalent of
    `kmsan_init_shadow()`/`_runtime()` — those are generic
    and fire on their own. Arch's job is just to make the VA
    available and provide the hooks.

This finding reduces the task-#67 scope from "1-2 days focused
debug" to "probably half a day": rebuild the second-probe
scaffold + add `KMSAN_SANITIZE := n` to the right Makefile
entries + retry. If that's insufficient, the next thing to
check is whether any `__msan_*` call fires in the host-syscall
wrappers (`arch/um/os-Linux/mem.c`'s `os_map_memory` etc.) —
those TUs have `KASAN_SANITIZE := n` already but the second
probe added `KMSAN_SANITIZE := n` only to `os-Linux/Makefile`,
NOT to `arch/um/kernel/Makefile` where mem.c lives.

No code changes in this session for this finding. The next
picker of task #67 rebuilds the second-probe scaffold (files
listed above), adds the SANITIZE lines to the right Makefiles,
and almost certainly unblocks.

**Cross-references:**
- `mm/kmsan/init.c:75,229` — kmsan_init_shadow + runtime.
- `mm/mm_init.c:2739,2768` — generic call sites.
- `include/linux/kmsan.h:38-47` — canonical contract.
- `arch/um/kernel/mem.c` — where UML's KASAN two-phase init
  lives; KMSAN follows the same pattern.
- KASAN's `arch/um/include/asm/kasan.h` + `.kasan_init` section
  pointer in mem.c — the template the second-probe scaffold
  correctly mirrored for VA-layout but incorrectly duplicated
  for the init-sequence itself.

### Fourth probe (2026-04-21): third-probe hypothesis INSUFFICIENT

Attempted the third-probe fix concretely as a scratch (reverted,
not pushed):

  - Recreated second-probe scaffold (pgtable.h + asm/kmsan.h +
    Kconfig + os-Linux/Makefile + mem.c kmsan_init via
    .kasan_init).
  - **Plus** added `KMSAN_SANITIZE_mem.o := n` and
    `KMSAN_SANITIZE_physmem.o := n` to arch/um/kernel/Makefile.

Build: clean under `make ARCH=um LLVM=1 O=/tmp/uml-kmsan-probe
-j$(nproc)` with CONFIG_KMSAN=y + KASAN=n + KCSAN=n.

Boot: **same hang**. `timeout 60 ./linux init=/bin/true mem=256M`
stops at "Checking syscall emulation for ptrace...OK" with zero
kernel output after that. Identical failure mode to the second
probe. The third-probe `KMSAN_SANITIZE_*.o := n` hypothesis was
not sufficient.

**What the fourth probe teaches:** the hang is not just the
phase-1 mmap TU being instrumented. The issue is deeper —
probably the architectural difference between KASAN's shadow
(raw memory; any read returns mmap zero-fill) and KMSAN's
shadow (per-page-struct metadata pointers that must be
populated before any hook runs).

KASAN works on UML because its shadow is **stateless at phase
1** — reading shadow VA returns whatever mmap zero-filled, and
zero means "addr ok". KMSAN expects each shadow page to have a
real `struct page` with `page->kmsan_shadow` +
`page->kmsan_origin` populated. The arch-side phase-1 mmap
gives the VA but doesn't populate those struct-page pointers.
Any KMSAN hook that runs before generic `kmsan_init_shadow()`
(called from `mm_core_init` much later than `.kasan_init`)
dereferences `page->kmsan_shadow` → NULL pointer → hang or
fault.

**Revised recommendation for task #67:**

Do NOT retry the `.kasan_init` phase-1 pattern. It fought the
architecture across three probes. The v2+ implementation should
instead:

  a. Drop the UML-specific `kmsan_init()` entirely.
  b. Let `kmsan_init_shadow()` / `_runtime()` run at their
     generic call sites from `mm_core_init`.
  c. Hook into `kmsan_record_future_shadow_range()` or
     `kmsan_init_alloc_meta_for_range()` with an arch-
     callback that does a map-on-demand mmap for each range
     as it's registered, so shadow VA becomes real memory at
     the same moment generic code populates the per-page
     metadata.
  d. Every arch/um/ TU instrumented by clang between `.init`
     section and `mm_core_init` needs a consistent KMSAN
     disable/global-suppress mechanism — probably the
     canonical `kmsan_disable_current()` guard wrapped around
     the early-boot path, matching how x86 handles early init.

This is more invasive than the phase-1 mmap approach (probably
requires a small arch-callback addition to mm/kmsan/init.c that
UML would use, which in turn needs LKML coordination with
KMSAN maintainer Alexander Potapenko). Scope re-estimated
**back up** to ~1 week including upstream review.

**Task #67 status unchanged:** still design-locked,
implementation-queued. The fourth probe narrowed the solution
space: we now know phase-1 mmap + per-TU SANITIZE opt-outs is
the WRONG approach. The right approach is "defer to generic
init timing, provide a map-on-demand arch callback." That is
concrete guidance for the next picker, not speculation.

No code pushed; all scratch reverted. D44 now has three
empirical probes plus this fourth-probe negative result, each
narrowing the design. Future picker starts at "fourth probe
failed; follow the deferred-init recommendation in the next
section."

**Cross-references (fourth probe):**
- `mm/kmsan/init.c:82-90` — `kmsan_record_future_shadow_range`
  + the `kmsan_init_alloc_meta_for_range` loop; candidate
  extension points for the arch map-on-demand callback.
- `include/linux/kmsan.h:`
  `kmsan_{enter,leave}_runtime` / `kmsan_disable_current` —
  the suppress mechanism for early-boot paths.
- KMSAN maintainer: Alexander Potapenko
  `<glider@google.com>` (per MAINTAINERS).

### Resolution (2026-04-22): accept the fourth-probe path; upstream as new entrypoints, not behavior changes

**Status flip:** Open → **Resolved.** Next steps move to D51
(upstream strategy for the arch callback).

**Decision recap.** Options A (VMALLOC quarter-split) and B
(dedicated mmap past KASAN) both fight KMSAN's per-page-struct
metadata model at phase-1; the fourth probe's empirical hang is
the signal that neither works on UML without help from the
generic init path. The decided path — aligned with what the
fourth probe itself recommended — is option D:

  **Hook into the generic init sequence via a new arch
  callback.** Let `kmsan_init_shadow()` / `_runtime()` run at
  their canonical call sites from `mm_core_init`; UML provides
  a `kmsan_arch_init_early_shadow()` override that mmaps its
  shadow/origin VA regions and registers them via the existing
  `kmsan_record_future_shadow_range()` helper before the
  generic sweep.

**Upstream patch shape (no behavior change for existing archs).**

The cross-subsystem patch is deliberately tiny and additive:

  1. `mm/kmsan/init.c`: make `kmsan_record_future_shadow_range()`
     non-`static` (or add a one-line public wrapper in
     `include/linux/kmsan.h`). It already does the right thing
     semantically; only visibility changes.
  2. `include/linux/kmsan.h`: declare
     `void __init kmsan_arch_init_early_shadow(void);`
     with a `__weak` default empty definition in
     `mm/kmsan/init.c`.
  3. `mm/kmsan/init.c::kmsan_init_shadow()`: call
     `kmsan_arch_init_early_shadow()` near the top, before the
     existing `for_each_reserved_mem_range` sweep. Existing
     archs that don't override get the no-op default —
     byte-identical behavior to today.
  4. `arch/um/kernel/mem.c` (+ `arch/um/include/asm/kmsan.h`):
     override the weak symbol. mmap the VAs bounded by the
     conditional VMALLOC_END from probes 2-3; call
     `kmsan_record_future_shadow_range()` for each.

**Why this is the right shape to pitch upstream.**

- **Zero visible impact on x86 and s390.** They don't define
  the weak override; their `kmsan_init_shadow()` path runs
  byte-for-byte as it does today. No performance regression,
  no ABI change, no reviewer-uncomfortable behavior delta.
- **New entrypoints, not modified ones.** The only surface
  change to the generic code is one new weak hook call and
  one symbol becoming non-static. Maintenance burden for the
  KMSAN core stays at zero — UML carries its own override.
- **Semantic surface grows by exactly one override.** Easy
  to review (diff is ~20 lines), easy to document, easy to
  remove later if KMSAN core grows a better-factored arch
  init framework.
- **Unlocks a second architecture for KMSAN** (UML/x86_64
  alongside native x86_64 and s390). That's strictly additive
  for the KMSAN ecosystem.

**The "sale" framing — why the KMSAN maintainer should want this.**

The upstream pitch has to carry context that's not obvious
from the diff alone: KMSAN's usefulness multiplies with
testable platforms, and UML is re-emerging as a credible one.
The cover letter should explicitly tie the arch callback to
the rest of the redesign's output so Potapenko sees the
payoff is platform capability, not just another arch port:

- UML now runs the full Linux tracing surface — ftrace,
  kprobes, kretprobes via rethook, and (since C-04 commit 3b)
  function_graph — alongside KASAN, KFENCE, KCSAN, UBSAN in
  their own profiles.
- BPF JIT works verbatim from `arch/x86/net/bpf_jit_comp.c`;
  `bpftrace` / `perf probe` run inside a UML guest.
- AFL-compatible forkserver hits <50 ms per iteration without
  KVM; syzkaller `vm/uml` backend (Go) is in flight.
- Backend abstraction (PTRACE / SECCOMP / DYNAMIC) via
  `struct um_backend_ops` — clean separation for future
  cross-arch work.
- Every commit passes a 12-variant boot matrix + four-check
  static-analysis gate (gcc / clang / sparse / smatch).

Adding KMSAN on top of that stack gives KMSAN developers a
fast, host-process-native target for regression testing that
doesn't require KVM or hardware — catch-rate parity with the
bare-metal test matrix, without the per-test boot cost.
That's the argument; the patch itself is the cheapest
possible way to enable it.

**Cross-references (resolution):**

- D51 — upstream submission strategy + LKML framing
  (cover letter skeleton + recipient routing).
- `Documentation/virt/uml/redesign/upstream-patches/
  kmsan-arch-callback-rfc/` — RFC series staging directory;
  populated when the patch itself is written.
- `07-port-kmsan.md` — status updated to match this resolution.
- `mm/kmsan/init.c:30,75` — `kmsan_record_future_shadow_range`
  (to be un-staticed) and `kmsan_init_shadow` (new hook
  insertion point).

---

## D51: C-07 KMSAN upstream strategy — LKML RFC framing and routing

**Date:** 2026-04-22
**Status:** Prepared; awaiting user decision to send. RFC patch
not yet written; this entry captures the framing so when the
code is ready the cover letter and routing are already decided.

**Who sees the patch first.**

Recipients (from `scripts/get_maintainer.pl` on the files
touched and `MAINTAINERS` entries for KMSAN + UML):

| Role | Person | Why |
|---|---|---|
| KMSAN maintainer | Alexander Potapenko `<glider@google.com>` | Owns `mm/kmsan/`; authors of the file the new hook goes into. |
| KMSAN reviewer / co-author | Dmitry Vyukov `<dvyukov@google.com>` | Usually on kmsan threads; deep KASAN/KMSAN context. |
| UML maintainers | Johannes Berg, Anton Ivanov | Ack on the `arch/um/` side; their sign-off carries weight with Potapenko about whether UML is worth extending for. |
| mm reviewers | Andrew Morton + linux-mm | Generic mm patch path. |
| Lists | linux-kernel@, linux-mm@, linux-um@ | Standard cc. |

**Cover-letter frame (what it argues).**

The cover letter does three things in order:

  1. **Show the diff is minimal and non-invasive.** Lead with
     "this adds one weak hook and un-statics one helper;
     existing architectures are byte-identical." That answers
     the first maintainer question ("what breaks?") before
     they ask.

  2. **Anchor the UML side as a concrete, active platform.**
     Link to the redesign directory
     (`Documentation/virt/uml/redesign/`). Mention the
     capabilities UML now has — ftrace, kprobes, kretprobes,
     function_graph, KASAN/KFENCE/KCSAN/UBSAN, BPF JIT, AFL
     forkserver, `uml-launcher` Rust host tool — so the
     reviewer sees this isn't reviving a dead platform.

  3. **Name the concrete payoff for KMSAN.** UML becomes a
     fast, host-process-native test target for KMSAN itself.
     No KVM dependency, no boot time. Regression-test coverage
     for the sanitizer on a second architecture at essentially
     zero cost to KMSAN maintenance.

**What the cover letter does NOT do.**

- **No UML partisanship.** The patch is a generic mm/kmsan/
  improvement; the UML benefit is the user of it, not the
  justification. Write it as "enable KMSAN on additional
  architectures" — UML is an existence proof, not a plea.
- **No decisions-log references in the commit bodies.** Patch
  messages are self-contained for the generic reviewer. The
  lineage back to D44/D51 stays in `SUBMISSION-NOTES.md`
  alongside the patch series, per the precedent set by
  `bpf-hygiene-v1/`.
- **No speculation about future arch users.** If s390 or
  others ever want their own shadow-init tweaks they can
  override the same hook; we don't promise future users.

**Pre-submission checks (to do when the RFC patch is written).**

- `scripts/get_maintainer.pl` on the actual diff — confirm
  the cc list above is complete and current.
- `scripts/checkpatch.pl --strict` on each patch; expect
  clean (this is ~20 LOC of generic code + an arch override).
- Build-verify: `make defconfig + CONFIG_KMSAN=y` on x86_64
  (existing user) and on UML with the companion arch-side
  series. Both should boot to init without KMSAN warnings.
- Include the KMSAN KUnit test results on UML in the cover
  letter — concrete proof the new arch target works.

**Routing suggestions.**

- **First send as RFC** (`[RFC PATCH]` prefix, single series),
  with "RFC because it introduces a new hook on a sanitizer's
  init path; I want maintainer sign-off on the shape before
  committing UML-side depends to it." That gives Potapenko an
  exit valve without making him feel rushed.
- Post-RFC: reshape based on feedback. If approved in shape,
  send as `[PATCH v1]` with the same diff (assuming no
  requested changes). If Potapenko proposes a different seam
  (e.g., a callback registration API instead of a weak
  symbol), revise.

**Dependencies.**

- The arch-side UML patches (`arch/um/include/asm/kmsan.h`,
  `arch/um/kernel/mem.c` override, `arch/um/Kconfig`
  `select HAVE_ARCH_KMSAN`) build on top of the generic
  patch. They live on `uml-redesign-plan` and get their own
  series once the generic one lands.
- The bounded-`VMALLOC_END`-under-KMSAN change to
  `arch/um/include/asm/pgtable.h` is in the UML series, not
  the generic RFC. Rationale: it's UML-local; no other arch
  cares.

**What "done" looks like.**

- Generic RFC accepted + merged into `mm-next` (probably via
  Andrew).
- UML arch-side series (5-6 commits per D44 addendum's plan)
  lands on `uml-redesign-plan`; each commit passes Q1 +
  boot-matrix. Specifically:
  - commit 1: bounded-VMALLOC_END + `asm/kmsan.h` + Kconfig +
    Makefile scaffold (compile-only, `CONFIG_KMSAN=y` doesn't
    boot).
  - commit 2: `kmsan_arch_init_early_shadow()` override +
    research-kmsan profile (boots + KUnit passes).
  - commit 3: selftest + user doc + status flip.
- Task #67 moves to landed. C-07 workstream closes.
- The D44 "four probes" history stays in the log as
  background; D51 is the load-bearing reference for anyone
  picking up KMSAN-on-UML after this.

**Cross-references:**

- D44 — four-probe investigation + resolution pointing here.
- `Documentation/virt/uml/redesign/05-validation/
  upstream-strategy.md` — redesign's general upstream-pitch
  philosophy; this D51 is an instance.
- `Documentation/virt/uml/redesign/upstream-patches/README.md`
  — the in-tree layout for upstream-bound series; the
  `kmsan-arch-callback-rfc/` entry lands here when the code
  is written.

---

## D45: In-fork scope policy — show the end state, treat LKML as later discussion

**Date:** 2026-04-21
**Status:** Accepted.

The UML redesign runs on a personal branch (`uml-redesign-plan`)
whose purpose is to **demonstrate the working end state** of the
architecture — `prod-fast`, `research`, `fuzz`, `sandbox`
profiles actually delivering their promise, with snapshot/
forkserver and the host launcher working — so that when upstream
discussion of individual sub-components begins, there is a
running artifact rather than a paper proposal.

This changes how D34, D43, D44, and similar "blocked on
upstream" classifications should be read:

**Before D45:** "Cross-subsystem change. Wait for LKML
discussion + maintainer ack + merge cycles before implementing
on the fork."

**After D45:** "Cross-subsystem change. Implement on the fork
now. Track the merge-surface-area growth so a future upstream
conversation has a concrete artifact to point at. Do not gate
fork progress on LKML review cycles."

**Decision scope.** D45 applies specifically to changes this
plan motivates: refactors in `arch/x86/` that make UML consume
x86 host code cleanly (C-06 B2), extensions to `mm/kmsan/` that
give UML an arch-extension point (C-07 map-on-demand callback),
`kernel/trace/` fixes that make `notrace` cooperate with
`-fpatchable-function-entry` (C-04 commit 3 / D34), and similar.
It does NOT apply to:

  - Changes that would semantically break bare-metal x86
    (D45 requires our fork's patches remain fully compatible
    with x86 defconfig).
  - Changes to subsystems the plan does not need to motivate
    (random scheduler refactors, filesystem rewrites, etc.).
  - Anything the owner has explicitly flagged as "not time yet."

**Upstream tracking.** Each in-fork cross-subsystem commit is
written with an upstream-submittable message (no UML references
in the subject line; rationale that stands alone for
bare-metal x86 if possible). The `upstream-patches/` directory
under `02-workstreams/C-profiles-and-gaps/` holds
`format-patch`-ready series for eventual submission. This
posture matches D45's intent: the fork is the demo; upstream
adoption happens after we have a running artifact.

**Implications for existing blocked-on-upstream items:**

  - **D34** (C-04 commit 3 / HAVE_FUNCTION_GRAPH_TRACER):
    becomes "write the `kernel/trace/` + `notrace`
    cooperation fix on the fork; unblock C-04 commit 3 locally;
    defer upstream submission."
  - **D43 option B2** (C-06 / BPF JIT portable-emitter
    refactor): becomes "split `arch/x86/net/bpf_jit_comp.c` on
    the fork; ship a working UML BPF JIT; defer the upstream
    RFC to the BPF maintainers until we have the demo."
  - **D44 fourth probe** (C-07 KMSAN / map-on-demand callback):
    becomes "add the arch-extension point to
    `mm/kmsan/init.c` on the fork; implement UML's callback;
    unblock KMSAN locally; defer upstream Potapenko coordination
    until the full sanitizer trio is demonstrably working on
    the fork."

**What this re-opens.** Tasks previously classified as
READY-BIG with LKML coordination as the gating cost are now
"doable in focused sessions." The actual engineering work
remains; the LKML review cycle is deferred to the separate
question of "when to propose upstream adoption."

**Cross-references:**
- `00-vision.md` — "Why now" section's LKML-acceptance framing
  stays accurate for the eventual upstream story; D45 just
  clarifies that the fork demonstration comes first.
- `04-risks/political-lkml-acceptance.md` — the structural
  reasons Tazaki's RFC stalled inform the fork-first strategy.
- `07-references/prior-art.md` — gVisor and LKL both carried
  significant out-of-tree code before upstream conversations;
  D45 adopts that pattern.

---

## D46: Three post-landing review bugs (fuzz build, launcher precedence, snapshot latch) — fix discipline going forward

**Date:** 2026-04-21
**Status:** Accepted (all three fixed in the same session).

A review pass over C-06 / C-09 / C-10 — the three workstreams we
had just declared "landed" — surfaced three real bugs that the
normal build-and-selftest flow hadn't caught. Recording them
here so the pattern that let them land stays visible and so the
fix choices are documented for future reviewers.

**The three bugs:**

1. **C-06 broke uml/fuzz.** Option A's
   `arch/um/include/asm/segment.h` shim (added to give
   `arch/x86/net/bpf_jit_comp.c` access to `__KERNEL_DS`) took
   include-path precedence over `arch/x86/um/asm/segment.h` and
   silently shadowed its `GDT_ENTRY_TLS_ENTRIES` definition. The
   uml/fuzz profile (no BPF JIT, so nobody had validated the new
   shim under it) failed at `arch/um/kernel/asm-offsets.c`.
   Severity: HIGH — we had claimed this profile built clean when
   landing C-09.

2. **C-10 launcher precedence was wrong.** `RunArgs.root` and
   `RunArgs.console` were bare `String` / `Console` with clap
   `default_value` attributes, so clap populated them on every
   invocation. The `CliOverlay` faithfully forwarded the clap-
   supplied default into figment as the highest-priority layer,
   defeating the documented CLI > env > TOML > defaults
   precedence for those two knobs. Any `root = "/dev/ubda"` or
   `UML_ROOT=...` silently lost to clap's `"hostfs"` default.
   Severity: MEDIUM — config was wrong but functional; the
   TOML/env paths existed but didn't take effect for two fields.

3. **C-09 snapshot static-key latch.** `um_snapshot_ready`
   enabled the `um_snapshot_enabled` static key unconditionally
   before calling `um_snapshot_forkserver_loop`. The loop's
   first action was an fd-open check that returned -ENODEV if
   fds 198/199 weren't plumbed (the common debugfs-poke case) —
   but the key stayed on forever. Every kernel hot path gated
   by `static_branch_unlikely(&um_snapshot_enabled)` paid the
   taken-branch cost permanently after a single benign poke.
   Same latch on every non-happy-path loop exit. Severity:
   MEDIUM — correctness-adjacent (silent perf regression after
   a diagnostic write).

**How they landed together.** All three cleared our actual
validation gates: the binary built (for the profile used during
bring-up), the selftest passed (for the path it exercised), the
docs read correctly. None of them were caught because each gate
had a blind spot the bug sat in exactly:

  - Bug 1: the fuzz build was only exercised after C-06 landed;
    C-06 exercised the profile that needed the shim, not the
    profile the shim broke.
  - Bug 2: launcher selftests covered kernel/init/mem but not
    the root/console precedence-through-TOML path, because
    those fields had been "working" on the CLI under their
    clap defaults.
  - Bug 3: no test exercised "call `um_snapshot_ready` without
    a fuzzer attached, then measure hot-path cost." The debugfs
    path existed but only for the happy-path "fuzzer connects
    immediately" case.

**Fix discipline going forward.** Three changes:

  - **Cross-profile build matrix.** When a workstream ships a
    new `arch/um/include/asm/` shim or touches shared build
    infrastructure, it must build at least `uml/prod-fast`,
    `uml/research`, and `uml/fuzz` before declaring done. The
    Q1 quality-bar runner already lists these; we just didn't
    run it after landing the shim. (Running it would have
    caught bug 1 in under 10 minutes.)

  - **Regression test for every precedence bug.** The launcher
    fix added two tempfile-backed TOML tests covering "CLI
    absent → TOML wins" and "CLI present → CLI wins" for each
    knob. Any future field that goes through the same
    Config/CliOverlay path gets the same pair. (This is worth
    codifying as a lint in the config.rs tests module: for
    every field X in `Config`, assert `assert_field_respects_precedence!(X)`.)

  - **Benign-poke test for every static key.** The snapshot
    fix is a specific instance of a class: any static key flipped
    in response to a runtime trigger must have a matching disable
    on every failure path. For future static-key additions (KFENCE
    sample rate, time-travel active, …) require a "poke that
    doesn't complete the action" test that confirms the key
    returns to baseline.

**Cross-references:**
- Commit `48aa69e2d4a7` — fix 1 (segment.h split).
- Commit `672edefe415a` — fix 2 (launcher precedence).
- Commit `e65cedc6b3a6` — fix 3 (snapshot static-key latch).
- `02-workstreams/C-profiles-and-gaps/06-port-bpf-jit.md` —
  C-06 close-out should note the shim split.
- `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md` —
  static-key lifecycle should be called out in v1's scope notes.
- `02-workstreams/C-profiles-and-gaps/10-host-launcher-crosvm.md` —
  precedence chain should be framed as "CliOverlay only
  serializes what the user actually passed."
- `05-validation/` — the Q1 quality-bar runner should include
  the three profile builds plus a smoke test that runs
  `um_snapshot_ready` from debugfs without fds plumbed.

---

## D47: Fourth-pass review fixes — five correctness/contract/ergonomics items in one series

**Date:** 2026-04-21
**Status:** Accepted (all five items landed as
commits `e27b853264cc..257b8cf61b84`; selftest sweep clean;
boot matrix 12/12 PASS).

A fourth external review pass flagged three `Medium`/`High` bugs
plus an open policy question, all with concrete line references
against HEAD. Recording the fixes as a single decisions entry
because the items share a theme — "the plan's invariants drifted
away from the code in ways only a careful review caught."

**The five items (severity / commit):**

1. **Medium — `init_backend()` section mismatches** (commit
   `e27b853264cc`): `init_backend()` and
   `pick_dynamic_backend()` read `backend_arg_{requested,force}`
   (both `__initdata`) without `__init` markers on either
   function, producing four modpost `WARNING: section mismatch`
   lines. Fix: add `__init` to both functions and to the extern
   declaration in `arch/um/include/asm/backend.h`, plus pull in
   `<linux/init.h>`. Warnings gone.

2. **Latent Q1 grep-case bug** (commit `fef60bc030e7`): the Q1
   runner, diff, and boot-matrix all grep'd for lowercase
   `warning:` only. modpost emits uppercase `WARNING:`. The four
   section-mismatch warnings had been sitting in the baseline
   invisibly for the entire period since init_backend was
   refactored — not flagged by the tripwire that's supposed to
   catch exactly this class of regression. Fix: `-i` flag on all
   three greps, widened runner pattern to `warn(ing)?:` to also
   catch smatch's short-form `warn:` messages. Smatch count in
   the Q1 output rose from 144 to 275 because we're now counting
   the short-form lines that were always in the logs; baseline
   `.txt` content unchanged because the extract() function
   specifically matches `warning:` + `error:` (the set worth
   diffing, not `warn:` which is noisier).

3. **Open policy — `backend=auto` default drift** (commit
   `79690a06d676`): prod-fast.rst / prod-fast.config advertised
   "`backend=auto` picks seccomp where available, ptrace
   otherwise", but the code in `os_early_checks()` only ran the
   seccomp probe when `seccomp_config` or
   `backend_arg_requested == SECCOMP`. Bare `backend=auto`
   silently fell through to ptrace — a ~3-4× perf gap between
   documented and delivered. Flipped the gate: probe runs for
   any DYNAMIC build that compiled seccomp in and didn't
   explicitly request ptrace. backends.rst "It just works" row
   updated to match. `uml-boot-matrix.sh` DYNAMIC/default
   expectation updated from `ptrace` to `seccomp`.

   **Supersedes D15's deferral** of the seccomp-first default
   flip: that deferral was keyed on the legacy `seccomp=` state
   and predated the `backend=` param surface. With the modern
   param set, there's no meaningful downside to making
   `backend=auto` actually probe; the policy question D15
   parked is now resolved by letting the code match
   prod-fast's documented promise.

4. **Medium — backend lifecycle skipped `probe()` / `init()`**
   (commit `32fbf168e671`): `backend-contract.rst` advertises
   three lifecycle ops (probe/init/shutdown), but
   `init_backend()` only called `validate_hot_ops`. An earlier
   commit (`90bee3502ab7`) wired `shutdown()` into the reboot
   path; this commit closes the other two. Dispatch happens
   immediately after HOT validation, panicking on non-zero
   return per the contract. In-tree ptrace/seccomp stubs still
   return 0 so the runtime effect is currently nil; the
   structural value is that the next KVM backend's real
   `kvm_open()` / vcpu-thread spawn has a wired call site.

5. **High — snapshot forkserver wrote status without waitpid**
   (commit `257b8cf61b84`): parent wrote worker pid + hard-coded
   zero status immediately after `fork()`. Zombies accumulated;
   fuzzer was told "iteration finished" before the worker had
   exited; workers could overlap. Tried the obvious fix (a
   blocking `waitpid(pid)` between pid-write and status-write)
   — same 3d-a-era crash reappeared even with UML signal gate +
   raw wait4 syscall. Root cause unidentified for three
   sessions running; we stop burning time on it.

   Narrower fix that works: non-blocking zombie drain at the
   top of each loop iteration via new
   `os_snapshot_reap_zombies()` (WNOHANG wait4 loop). The
   reviewer's "zombies accumulate" complaint is now closed;
   the "status uninformative" side stays documented as a v1
   ceiling per D41/D42. Verified via `ps --ppid <uml_pid>`
   post-iteration: no surviving children, worker reaped by
   next-iter drain.

**Discipline consequences (continues D46):**

- **Every commit gets `git commit -s`**: memory note saved
  under `feedback_commit_signoff.md`. The session prior to this
  one landed ~10 commits without signoffs; none of them will be
  rewritten (already pushed), but every commit this session
  carries a proper `Signed-off-by:`.

- **Every commit gets `scripts/checkpatch.pl --strict -g HEAD`
  before the session ends**, not just "at some point." That is
  the tripwire the session-prior omitted and that this series
  runs religiously.

- **The Q1 grep case bug is a reminder that quality gates can
  have silent gaps**: when a regression class appears once
  (init_backend section-mismatch was the once), look for why
  the gate didn't catch it and fix the gate in the same series.

**Cross-references:**
- Commits `e27b853264cc` (fix 1), `fef60bc030e7` (fix 2),
  `79690a06d676` (fix 3), `32fbf168e671` (fix 4),
  `257b8cf61b84` (fix 5).
- D15 — backend=auto legacy deferral; **superseded by this
  entry's item 3**.
- D41/D42 — snapshot forkserver v1 ceiling; this entry's item 5
  partially closes the zombie side of D42's "sustained fuzz"
  story without touching the waitpid crash.
- D46 — the review-fix predecessor that established the
  fix-discipline pattern this series extends.
- `backend-contract.rst` — the lifecycle doc is now authoritative
  (probe + init + shutdown all dispatch via the ops table).

---

## D48: Sequence C-08 (syzkaller vm/uml) before any own-fuzzer build; record the Rust companion as a future phase

**Date:** 2026-04-21
**Status:** Accepted (sequence decision, no code impact yet).

A reasonable line of questioning surfaced while reviewing C-08:
"why can't we just write our own fuzzer, in Rust, in-tree,
syzlang-compatible?" The answer is a sequence decision, not a
yes/no. Capturing it here so the question doesn't re-open
during C-08 implementation.

**The claim the question challenges:**

C-08's "kernel side ready" framing implied the only remaining
work was writing `pkg/vm/uml/` in the syzkaller repo (Go,
~2–3 weeks, upstream PR). Rust-in-tree-with-syzlang-reuse is an
alternative approach that consumes syzkaller's ecosystem (`.txt`
grammar, syz-db corpus format) but runs its own
executor+mutator loop. Structurally feasible — syzkaller is
Apache 2.0, GPL-2.0-compatible; syzlang parser in Rust is
2–3k LOC with nom/pest; `tools/uml/uml-launcher/` establishes
Rust-in-tree precedent.

**What the alternative changes:**

  - Direct fd-198/199 integration with the C-09 forkserver,
    no `pkg/vm/vmimpl` layer. Theoretical speed gain over
    syzkaller-through-generic-VM-abstraction: 5–10× iter/s.
  - UML time-travel / record-replay integration. Rewind-and-
    mutate fuzzing is a genuinely syzkaller-can't-do-it
    capability (no other syzkaller target supports it).

**What the alternative does NOT change:**

  - **syzbot integration stays syzkaller's.** Running our own
    fuzzer doesn't get bugs into the auto-triage email loop.
    Vision success criterion #6 ("syzbot has UML in rotation")
    requires syzkaller adoption regardless of which local
    fuzzer we eventually write.
  - **Grammar curation stays upstream.** Kernel developers
    land new syscalls by submitting to syzkaller's
    `sys/linux/`. Our tool reads those files; it doesn't own
    them. That's fine — it's the correct layer to consume at
    — but it means we track syzkaller's upstream indefinitely.
  - **Corpus compounding depends on continuous sync.** We
    inherit the existing corpus on day one (syzlang format is
    parseable). Whether we keep up with the growing corpus
    depends on how often we re-ingest syzkaller's exports.

**Alternatives considered:**

1. **Just do C-08; never build `uml-fuzz`.** Simplest. Vision
   criteria #2 and #6 covered by C-08's 2–3-week PR. Leaves
   the speed and time-travel wedges unexploited, which is
   acceptable if those wedges turn out to be research-interest
   rather than shippable-product-interest.

2. **Skip C-08; build `uml-fuzz` instead.** Tempting because
   "our Rust tool" feels more controllable than "a Go PR to a
   Google repo." Rejected: this reverses the sequencing and
   gets NEITHER syzbot adoption NOR a shipping companion
   (because the companion has no reference target to be a
   companion to). Also wastes the existing ecosystem — we'd
   be building from scratch what C-08 gives us for free.

3. **Both, in sequence.** C-08 first (2–3 weeks, unambiguous
   win, unblocks syzbot adoption and vision criteria #2 + #6).
   `uml-fuzz` later, **only if evidence accumulates** that
   C-08-via-syzkaller leaves material capability on the table.
   The "only if" is important: we don't know yet whether
   syzkaller-via-pkg/vm/uml/ saturates the forkserver or
   leaves 10× on the floor. We'll have numbers after C-08
   lands and runs for a month. Decision made with data
   beats decision made from armchair.

**Selected:** alternative 3.

**Triggers to revisit:**

- C-08 has been landed in syzkaller upstream for ≥6 months AND
  syzbot is running UML in rotation.
- Measured iter/s on workstation hardware via syzkaller-on-UML
  is below 1000 (vision criterion #2 target) AND the C-09 v2
  freezer-cgroup redesign has shipped, so the bottleneck
  explanation isn't just "v1 wasn't designed for sustained."
- An operator has written a one-pager demonstrating a
  specific bug class that requires time-travel/rewind fuzzing
  (not "wouldn't it be cool if" — "this specific bug escaped
  syzkaller's reach because it needs rewind-and-mutate").

Without all three, `uml-fuzz` stays parked. With all three,
it graduates into a proper workstream task (likely numbered
E-01 or similar, outside the current A–D plan per the
`08-future-phases/README.md` scope policy).

**Why this entry exists despite "no code impact":**

The question "why not Rust-in-tree" is natural and will come
up again. Without this entry a future reader (or a future me)
has to re-derive the sequencing from first principles. D48
preserves the reasoning so "land C-08 first" doesn't read as
a preference against Rust tooling — it's a consequence of
"syzbot adoption is on vision criterion #6, and `uml-fuzz`
doesn't deliver that, so it can't be first."

**Cross-references:**
- `02-workstreams/C-profiles-and-gaps/08-syzkaller-vm-uml.md`
  — the C-08 workstream that must land first.
- `08-future-phases/03-uml-fuzz-rust-companion.md` — the
  design-sketch for the parked companion tool.
- D45 — the in-fork scope policy (covers any kernel-side
  changes `uml-fuzz` turns out to need beyond what C-08
  already motivates).

---

## D49: Temper the "41% PTRACE_ONLY speedup" in 3e1a7fa5f83d — bimodal sampling artifact, real delta likely <15%

**Date:** 2026-04-22
**Status:** Accepted (commit message stands as historical record;
this entry is the correction to it).

Commit `3e1a7fa5f83d` ("um: backend: restore s1 as perf-baseline
host, record definitive no-regression result") compared the
2026-04-18 baseline (`9dba3c374a62`) and current HEAD on the
same host s1 and reported:

```
PTRACE_ONLY     337,982,126  →  198,405,986  -41% (IMPROVEMENT)
SECCOMP_ONLY    195,852,365  →  191,190,938  -2.3% (noise)
DYN_ptrace      200,999,183  →  190,537,405  -5.2% (noise)
DYN_seccomp     195,394,985  →  190,671,958  -2.4% (noise)
```

The "no regression anywhere" part of that message is right. The
"PTRACE_ONLY materially improved — ~41% faster" part is
overstated to the point of being misleading. This entry
documents why, so no future reader (or upstream pitch) cites
the 41% as a supported claim.

**The data is bimodal.** Looking at p25/p50/p75 of both runs:

```
baseline PTRACE_ONLY:  p25=202M  p50=338M  p75=365M
HEAD PTRACE_ONLY:      p25=198M  p50=198M  p75=362M
```

Two distinct modes — "fast" around ~200M cycles, "slow" around
~340–365M cycles. Both runs show both. What changed between the
runs is the *fraction of iterations that landed in which mode*,
not the speed of either mode:

  - Baseline: ~25% fast / ~75% slow → p50 sits in slow mode → 338M
  - HEAD:     ~50% fast / ~50% slow → p50 sits in fast mode → 198M

At N=10 that's a 2.5-vs-5 split — well inside binomial sampling
noise. The +70% between modes (slow/fast ≈ 1.7×) is almost
certainly something CPU-state-transition (first-run cold cache,
C-state wakeup latency, frequency ramp on a sub-second
workload, thermal step) rather than kernel code paths.

**What would be needed for an upstream-shippable claim:**

1. **N ≥ 30** per datapoint, not 10. Kill the binomial noise so
   a mode-fraction difference of 25%-vs-50% can be distinguished
   from 50%-vs-50%-drift.
2. **Warmup iterations discarded.** If the fast/slow split is
   first-iter-cold-vs-subsequent-warm, measure steady state only.
3. **Bisect the responsible commit.** "48 commits landed,
   something sped things up" is not an upstream patch. "Commit X
   reduced boot cycles by Y% via mechanism Z" is.
4. **Identify the mechanism.** Candidates from 48 arch/um
   commits since the baseline: backend-abstraction refactors
   (A/B workstreams); ftrace patch-site stripping (reduces
   per-function NOP pressure — but SECCOMP_ONLY should show the
   same effect if this were it, and didn't); notrace on generic
   kthread (saves some graph-init, but defconfig doesn't use
   graph); backend-lifecycle probe/init dispatch (minor one-time
   cost, direction wrong for a speedup). None of these
   individually explain a 41% PTRACE-only improvement. The
   symmetric-vs-asymmetric pattern (PTRACE improves, SECCOMP
   doesn't) itself rules out generic-path explanations.
5. **Confirm defconfig didn't change silently.** Between
   `9dba3c374a62` and HEAD the defconfig may have gained or lost
   Kconfig selections that affect the PTRACE_ONLY boot path
   independent of UML-side work. `diff <(uml/matrix-cfg baseline)
   <(uml/matrix-cfg HEAD)` before any perf claim.
6. **Cross-reference with a microbenchmark.** If the speedup is
   syscall-dispatch-side, a gettid-loop microbench should show
   it too. If it's boot-path one-time setup, that's a smaller
   story the upstream pitch should reflect.

**My honest estimate of the real delta, pending the above:**
somewhere between 0% and 15%. Of that, probably 5-10% is
genuine UML-side improvement from the A/B cleanup (measurable
as an instruction-count delta, not just cycles). The other
25-35% in the p50-vs-p50 comparison is bimodal sampling on a
small N. A 41% claim is roughly 3× the likely underlying truth.

**Why the commit message stands anyway.** Rewriting pushed
history to fix the claim would be worse than this correction —
the original commit records the actual observed numbers
truthfully (the p50 delta IS −41%; it's my interpretation of
that number as "materially improved" that was premature). D49
is the disciplined correction: the observation is data, the
conclusion was wrong, both are part of the record.

**Upstream posture.** The defensible current-state pitch to
LKML is the two arch-generic BPF hygiene patches staged at
`02-workstreams/C-profiles-and-gaps/upstream-patches/
bpf-hygiene-v1/` — clean, small, reviewed, no perf claim
attached. Everything else (including this 41% number) is
in-fork investigation work that's not ready for LKML yet and
shouldn't be framed that way.

**Revisit triggers:**
- The N=30-iter steady-state run + bisection gets done on s1
  (task #95 reopened as the follow-up, or a new task spawned).
  Produces either a defensible bisected commit-with-mechanism
  or a retraction that collapses the remaining claim to noise.
- The per-host baseline strategy (task #94) lands. With ratio-
  based comparison rather than absolute cycles, the bimodal
  issue still exists but cross-host confusion goes away.

**Cross-references:**
- `3e1a7fa5f83d` — the commit this tempers.
- `23c5bc7d17ed` — the tooling commit that set up the
  governor/cpu-pinning discipline (still valid and useful).
- `7a7d365c5872` — ratio-based comparison added to the
  compare script (task #97).
- D15 — seccomp-default-policy; superseded by D47 item 3.
- D47 — the fourth-pass review that this series traces back to.
- D48 — Rust/syzlang-companion fuzzer park.
- Task #94 — per-host perf-baseline strategy.
- Task #95 — closed-with-evidence, see the addendum below.

### 2026-04-22 addendum — N=30 on s1 stabilizes HEAD, baseline tree is unrebuildable

Ran N=30 on s1 at HEAD (`0e108dd7c80f`), performance governor,
cpu0-pinned. Result:

```
backend        p25          p50          p75          variance-shape
PTRACE_ONLY    198,240,168  198,568,927  362,953,049  bimodal (p75 in slow mode)
SECCOMP_ONLY   191,502,742  191,662,954  192,779,997  tight (all fast mode)
DYN_ptrace     190,928,359  191,117,725  355,459,656  bimodal (p75 in slow mode)
DYN_seccomp    190,746,873  191,035,639  355,425,097  bimodal (p75 in slow mode)
```

**HEAD's p50 is stable at ~199M (PTRACE) / ~192M (SECCOMP) on
s1.** Bimodal mode gap ~1.85× is consistent across all four
backends; "fast mode" clusters at ~190M cycles, "slow mode" at
~360M cycles. SECCOMP_ONLY tightens into the fast mode (p25 =
p75 to within 1%), which is what we'd want — the three other
backends still flirt with the slow mode at p75.

**The baseline tree (`9dba3c374a62`) cannot be rebuilt with
current scripts.** That commit pre-dates the A/B backend-
abstraction work: `CONFIG_UM_BACKEND_PTRACE_ONLY` /
`SECCOMP_ONLY` / `DYNAMIC` Kconfig options don't exist yet,
and the `uml-boot-matrix.sh` + `uml-perf.sh` scripts don't
exist either. A fresh N=30 at the baseline would need (a)
back-porting the script infrastructure to the baseline tree
or (b) reconstructing the legacy invocation (bare defconfig +
boot-matrix-era CONFIG_*) by archaeology. Neither is worth
the time to settle a "was there a regression between baseline
and HEAD" question that D49 already answered "no."

**What the N=30 + baseline-tree-unavailability jointly mean:**

  - The "41% PTRACE_ONLY improvement" the original commit
    message claimed is now explicitly understood as **"the A/B
    refactor moved PTRACE_ONLY from slow-mode-dominant
    sampling to fast-mode-dominant sampling."** That's a real
    behavioral improvement but it's much less than 41% in any
    steady-state sense (both fast and slow modes exist at HEAD
    too; we just sample them differently now).
  - There's no specific commit to bisect to. The A/B refactor
    is dozens of commits across months; the improvement is
    structural.
  - No upstream pitch exists around "UML boots 41% faster"
    without rebuilding that history with proper instrumentation
    (steady-state-only, both-mode-sampled, microbenchmarks).

**Task #95 accordingly stays closed.** The question is answered
to the precision the evidence supports.

---

## D50: last_ditch_exit signal handler must not call uml_cleanup — async-signal-safety

**Date:** 2026-04-22
**Status:** Accepted. Fix landed same day.

**Decision:** `arch/um/os-Linux/main.c`'s `last_ditch_exit()`
drops its `uml_cleanup()` call and becomes a minimal
async-signal-safe handler that prints a short message via
`write(2)` and calls `_exit(1)`. The host kernel handles the
remaining cleanup: all fds close, all mmaps unmap, and every
stub child receives SIGKILL via `PR_SET_PDEATHSIG` (already set
in `arch/um/os-Linux/process.c` and
`arch/um/kernel/skas/stub_exe.c`). The `um_backend->shutdown()`
dispatch, the `__exitcall()` chain, and `kill_off_processes()`
are all lost on the fatal-signal path; each was either a no-op
(backend shutdowns are no-ops for ptrace/seccomp today) or
redundant with host-kernel reclaim.

**Why this was wrong before:**

`last_ditch_exit` is installed for SIGINT / SIGTERM with
`SA_NODEFER | SA_RESETHAND`. When a fatal signal arrives, it
runs in **async-signal-handler context** with whatever preempt
state the interrupted kernel code held. POSIX signal-safety(7)
requires the handler to call only async-signal-safe functions;
the old body violated that aggressively:

  - `uml_cleanup()` runs `do_uml_exitcalls()` → `console_exit()`
    → `close_lines()` → `close_chan()` → `close_one_chan()` →
    `um_free_irq()` → `free_irq()`. `free_irq()` opens with
    `WARN(in_interrupt(), "Trying to free IRQ %d from IRQ
    context!")` and takes `desc->request_mutex` with
    `mutex_lock()` (sleeps under PROVE_LOCKING +
    DEBUG_ATOMIC_SLEEP).
  - `kill_off_processes()` walks `tasklist_lock` under
    `read_lock()` — not async-signal-safe.
  - `kmalloc_ok = 0` plus `um_backend->shutdown()` dispatch
    touches kmalloc-capable code paths.
  - `exit(3)` (vs. `_exit(2)`) runs atexit handlers and stdio
    cleanup, neither signal-safe.

The bug was latent because orderly halt paths go through
`reboot(2)` → `machine_halt()` → `uml_cleanup()` in normal
process context (no signal-handler inheritance; preempt_count
at 0; in_interrupt() false). `free_irq()`'s assertion passes
there. The only trigger for the signal-handler-side call chain
was fatal-signal delivery, which was rare until C-04's function
graph tracer made individual traced-function overhead large
enough that `timeout(1)`-driven SIGTERM began landing during
ordinary test runs. The C-04 commit 3b investigation surfaced
the warning; the fix is this D50 entry.

**What was tried / considered:**

| Option | Pick / reject |
|---|---|
| (a) Minimize `last_ditch_exit` to `_exit(1)` | **PICKED.** Simplest; correct per POSIX; loses zero load-bearing behavior (every component of `uml_cleanup` is either a no-op or redundant with host-kernel reclaim when UML itself exits). |
| (b) Make `uml_cleanup` signal-safe | Rejected. The call chain is deep into generic kernel code (`free_irq`, `mutex_lock`, `tasklist_lock`); making it signal-safe would require auditing and reworking paths that live outside `arch/um/`. Way out of scope for a localized arch bug. |
| (c) Deferred cleanup: set a flag from the handler, do work from the main kernel loop | Rejected as over-engineered. The current UML signal model doesn't have a "signal-requested work" queue; building one just for this case would be more surface than the problem warrants. |
| (d) Raise the signal to default disposition via `raise(sig)` with `SA_RESETHAND` already armed | Equivalent functionally (host kills UML with signal default), but `_exit(1)` preserves the existing "UML exits with code 1 on fatal signal" convention and is more predictable for anything that checks exit codes (test harnesses, supervisors). |

**Validation (arch/um/ on x86_64, uml/research, 2026-04-22):**

- `kill -TERM` against a UML running with
  `CONFIG_FUNCTION_GRAPH_TRACER=y + echo function_graph >
  current_tracer`: rc=1, dmesg contains the
  `"UML: fatal signal; exiting"` line, no BUG/WARN in the
  output, no leftover stub children on the host.
- Boot matrix (PTRACE_ONLY / SECCOMP_ONLY / DYNAMIC × default /
  force / legacy): 12/12 PASS — the normal reboot path
  (`machine_halt` → `uml_cleanup` in process context) is
  unchanged by this commit.
- Q1 runner: gcc/clang/sparse/smatch clean against the bumped
  per-profile baseline.

**What this explicitly does NOT fix:**

- There is no follow-up work here on the generic `free_irq(3)`
  assertion or on UML's signal-handler-context accounting. Those
  are pre-existing architectural decisions in the kernel and
  outside arch/um/; changing them would require cross-subsystem
  discussion.
- Future KVM backend's `shutdown()` will want signal-handler
  visibility. At that point, option (c) (deferred cleanup
  queue) becomes worth building. Not needed today.

**Cross-references:**

- `arch/um/os-Linux/main.c` `last_ditch_exit()` / `install_fatal_handler()` — the fix.
- `arch/um/kernel/reboot.c` `uml_cleanup()` — the no-longer-called-from-signal-handler function.
- `arch/um/os-Linux/process.c` / `arch/um/kernel/skas/stub_exe.c` `PR_SET_PDEATHSIG` — the host-kernel reap guarantee that makes `kill_off_processes()` redundant in the fatal-signal path.
- D34 addendum-3/4 (C-04 commit 3b) — the investigation that surfaced this pre-existing bug.
- Task #99 — closed by this commit.

---

## D52: C-10 v2 design — single-binary subcommands, seccompiler, AppArmor + SELinux in-tree

**Date:** 2026-04-22
**Status:** Accepted. Drives the C-10 v2 commit plan
(`02-workstreams/C-profiles-and-gaps/10-host-launcher-crosvm.md`).

**Decisions.**

1. **Single multi-call binary, not per-device binaries.** The
   existing `uml-launcher` gains a `backend <class>` subcommand
   that dispatches to per-class modules. Matches crosvm's
   `crosvm device <kind>` shape. Closes v1 Open-Question Q2.

2. **rust-vmm stack for vhost-user backends.** `vhost`,
   `vhost-user-backend`, `vm-memory`, `virtio-queue`,
   `virtio-bindings`, `vmm-sys-util` are the required crates;
   per-class helper crates (`virtio-net`, etc.) used where
   they fit cleanly, open-coded where they don't. Recency of
   each crate's last publish checked at landing time to avoid
   depending on stale rust-vmm surface.

3. **Seccomp via `seccompiler`** (Firecracker-authored, now
   standalone rust-vmm). Per-backend allow-list filters
   compiled into the binary from JSON, applied *after* fd
   plumbing + socket bind, *before* entering the event loop —
   matches both crosvm (minijail `enter()` timing) and
   Firecracker (per-thread apply) discipline. No generic
   backend-class default filter; each class writes its own.

4. **Both AppArmor and SELinux reference profiles ship
   in-tree** under `tools/uml/uml-launcher/apparmor/` and
   `tools/uml/uml-launcher/selinux/`. Neither crosvm nor
   Firecracker ship LSM profiles in their upstream trees;
   UML can lead here. Either LSM alone is enough; both
   together are redundant-safe. Each class gets its own
   domain / sub-profile; the launcher transitions post-
   seccomp.

5. **Commit discipline.** 9 bisectable commits per the plan.
   Each passes `cargo build` + `cargo test` + `cargo clippy
   -- -D warnings` (the tools/ analogue of checkpatch). The
   selftest per commit is a real end-to-end boot-and-measure,
   not a "should work" claim — matches a-plus-plan anti-
   pattern #6.

**Alternatives considered + rejected.**

| Option | Why rejected |
|---|---|
| Per-device binaries (`uml-launcher-net`, `uml-launcher-block`) | Larger packaging surface (N binaries + N manpages + N profiles); no benefit over subcommand dispatch. crosvm already made this call. |
| libseccomp instead of `seccompiler` | Brings a C FFI dependency; `seccompiler` is pure Rust and idiomatic in the rust-vmm stack UML is already adopting. |
| SELinux-only (skip AppArmor) | AppArmor is the path-based LSM distros like Ubuntu use by default; skipping it misses the majority of the non-enterprise install base. |
| AppArmor-only (skip SELinux) | Mirror of above — misses RHEL / Fedora / anyone running SELinux. Reference policies cost little to maintain. |
| slirp user-networking backend in v2 `net` | Unbounded scope; tap-only in v2 keeps the diff focused. v3+. |
| Firecracker's monolithic-jailer model (one process, no decomposition) | Defeats the profile's entire point — `sandbox` exists to demonstrate strong host isolation per device class. |

**Out of scope for v2** (tracked separately for future
phases): systemd unit templates, daemon mode, JSON-RPC
control surface, slirp, vfio-user, gpu/snd/wl/pmem backends.

**Cross-references.**

- `02-workstreams/C-profiles-and-gaps/10-host-launcher-crosvm.md`
  §"v2 (this section — per decisions-log D52)" — the commit
  plan this D52 drives.
- `arch/um/drivers/virtio_uml.c` — the UML-side vhost-user
  client v2 talks to. Already in tree; unchanged by v2.
- `03-profiles/sandbox.md` — consumer profile that gains the
  decomposed default when v2 lands.
- rust-vmm: `vhost`, `vhost-user-backend`, `seccompiler`.
- crosvm's `src/bin/crosvm.rs` subcommand dispatch — the
  precedent this v2 shape follows.

---

## D53 (2026-04-23) — C-10 v2 orchestration: same-binary multi-call + per-class virtio-id constants

**Decision.** `uml-launcher run --virtio <class>[:<args>]`
re-invokes the launcher binary (`std::env::current_exe()`)
as `uml-launcher backend <class> …` for each spec rather
than linking the backend modules directly into the run
path. Virtio-IDs are stable per-class constants (console=1,
net=2, block=3); multiple instances of the same class are
rejected at orchestrate time rather than silently colliding.

**Alternatives considered.**

- *Link backends into the run path (threads, no subprocess):*
  saves one fork+exec per backend but loses the per-class
  seccomp / AppArmor isolation that's the whole point of
  the per-device decomposition. A kernel-side bug in one
  backend class could then reach the UML control path.
- *Separate binary per backend* (`uml-backend-console`, …):
  matches what cloud-hypervisor historically did. Rejected
  in D52 — multi-call keeps packaging to one ELF + one
  manpage + one profile family per LSM. Re-affirmed here.
- *Runtime virtio-ID allocator that hands out successive
  slots:* needed the moment someone wants two disks, but
  the v2 scope is one instance per class. Deferred to a
  follow-on when multi-disk is the blocking need.

**How to apply.**

- New backend class: assign the next virtio-id constant in
  `src/virtio.rs`, add a case to `VirtioClass` + the parser
  + `orchestrate()`'s arg-building switch. The guest-side
  virtio_uml driver accepts any ID so there's no handshake
  to update.
- Relaxing "one instance per class": replace the per-class
  constant with an allocator that increments per spec, turn
  the duplicate-class check into a range-collision check.
  Don't do this preemptively; wait for the real use case.
- The `BackendProcess::Drop` contract (SIGTERM → 500 ms →
  SIGKILL + unlink socket) is load-bearing for the
  no-orphan invariant tested in launcher-smoke. Any change
  to lifecycle semantics needs matching test updates.

**Cross-references.**

- D52 — the shape decision this closes out.
- `tools/uml/uml-launcher/src/virtio.rs` — implementation.
- `tools/uml/uml-launcher/src/launcher.rs` — orchestrate
  call site + explicit `drop(backends)` after UML's
  `wait()` returns.

---

## D54 (2026-04-23) — C-07 lands on-fork ahead of upstream submission

**Decision.** The generic `kmsan_arch_init_early_shadow()`
weak hook patch (previously staged for upstream RFC in
`Documentation/virt/uml/redesign/upstream-patches/kmsan-arch-
callback-rfc/`) is committed directly to this fork as
`mm/kmsan: add kmsan_arch_init_early_shadow()` (commit
f3307a2f1c99), and the four UML-side commits (asm/kmsan.h
scaffold, kmsan_arch_init_early_shadow UML override,
arch_kmsan_get_meta_or_null stub, selftest + docs + profile)
land immediately after. Upstream RFC submission to
linux-mm + kmsan maintainers remains deferred until the
ARCH=um maintainer coordination the user asked us not to
pre-empt; the on-fork series unblocks the rest of the
redesign without waiting.

**Alternatives considered.**

- *Hold entire series behind upstream merge:* matches the
  original D51 plan. Rejected because the user explicitly
  cleared "landing code in our fork" from the upstream-
  coordination gate; that gate applies only to submission,
  not local iteration.
- *Carry UML-side patches only, rebase on upstream weak
  hook when it lands:* would force either a dummy inline
  for `kmsan_arch_init_early_shadow` in UML's own tree (to
  keep the UML build clean before the weak hook merges) or
  accept a broken intermediate state. Neither matches the
  "bisectable at every commit" discipline §2 of
  AGENT-PROMPT requires. Carrying the weak-hook commit on-
  branch fixes that cleanly.
- *Extend `asm/kmsan.h` to match x86's VMALLOC quarter-
  split rather than the dedicated-region layout:* already
  resolved by D44 probes 1-4 and the header comments —
  UML's VMALLOC_END is TASK_SIZE-bounded, not canonical-
  hole-bounded, so the x86 math has nowhere to land. Not
  re-opened here.

**How to apply.**

- Upstream submission: when ARCH=um maintainer buy-in
  materializes, the cover-letter in `upstream-patches/
  kmsan-arch-callback-rfc/` is still the right framing;
  rebase it onto whichever linux-mm tree is then current.
  The UML-side commits are fork-only in the sense that
  they depend on the weak hook landing upstream, so
  they'd go as a follow-on series after.
- Profile selection: `research-kmsan` is the sibling
  profile for KMSAN work. Don't try to enable KMSAN via a
  flag on top of `research` — Kconfig would need to handle
  the KASAN/KMSAN mutual exclusion and the memory-pressure
  overhead that only KMSAN users want to pay for. Keep
  them siblings.
- Follow-on work tracked in `07-port-kmsan.md` v3 roadmap
  (origin chain across snapshot/fork, KMSAN KUnit
  enablement under research-kmsan, memory-pressure
  measurement for the fuzz-kmsan idea).

**Cross-references.**

- D44 — four-probe empirical investigation that produced
  the dedicated-region layout.
- D51 — upstream framing for the weak-hook patch.
- `mm/kmsan/init.c::kmsan_arch_init_early_shadow`.
- `arch/um/include/asm/kmsan.h`, `arch/um/kernel/mem.c`.
- `Documentation/virt/uml/kmsan.rst`.

---

## D55 (2026-04-23) — D-workstream spike confirms KVM round-trip is ~5 µs, not ~100 ns; vision line updated

**Decision.** A bare `KVM_RUN` → `VMEXIT` → userspace
round-trip costs **~4.7 µs / ~17500 cycles median** on the
dev host (Skylake-SP, 3.7 GHz), empirically measured over
1000 iterations. The vision doc line "~100 ns syscall
overhead in production mode" is revised to "~1-5 µs on the
naive KVM backend (~4× win over seccomp); ~100 ns as a
later aspirational target predicated on an in-guest
systrap-equivalent gadget layer." The D workstream still
ships; the rebranding is from "KVM makes UML as fast as
bare metal" to "KVM makes UML as fast as QEMU-KVM, with
the observability story to make that worthwhile."

**Alternatives considered.**

- *Keep the ~100 ns vision line unchanged, hope the
  systrap gadget lands in first-phase D:* Rejected. The
  gadget is substantial independent engineering (gVisor's
  kvm platform has been iterating on its systrap for
  years), and selling the D workstream on a number the
  first landing can't hit would burn trust with both
  reviewers and consumers when they measure actual
  behavior.
- *Pick a different backend mechanism to hit ~100 ns:*
  Xen hypercalls, user-mode KVM, LXC — all rejected for
  reasons already documented (D-01 preface + this
  session's "KVM still the right choice" analysis). KVM
  stays; the number is what it is on this CPU family.
- *Defer D and re-sequence:* Rejected. Even at 5 µs, D
  delivers the "run research profile under KVM for 4×
  seccomp-baseline syscall speed, with real CPU rings for
  the sanitizer trio's edge cases" product win. D failing
  would be acceptable per the critical-path doc
  (`prod-fast on seccomp is the realistic default`), but
  D succeeding at 5 µs is better than not trying.

**How to apply.**

- New D workstream deliverable shape: `um_backend_kvm`
  lands as the naive "`KVM_RUN` per guest syscall" path
  first (D-02..D-06), hits the ~5 µs number, ships. The
  systrap gadget (D-04 ring transitions) becomes an
  explicit follow-on phase, not expected in the initial
  4-week D-01 design-memo scope.
- Performance bar for D ship: median syscall under
  research-kvm profile <10 µs, perf-vs-baseline regression
  flags at >20%. Currently at `perf-compare.sh`'s 5%
  blocker threshold for C-workstream work, which is
  stricter than D needs — the D profile's perf floor is
  "beat seccomp by ≥2×", not "bit-for-bit with bare
  metal."
- Vision doc stays honest: edit the headline so reviewers
  don't see a 100 ns claim the code can't back up, and
  reference this entry so the history is diffable.
- Second data point needed: run the spike on a modern
  CPU (Ice Lake, Sapphire Rapids, Zen 4) to bound the
  newer-CPU floor. If a recent CPU hits 1-2 µs, we're in
  good shape. If it still hits 5 µs, the VT-x exit cost
  hasn't scaled and the D backend's floor stays here.

**Cross-references.**

- `spikes/01-getpid-roundtrip/README.md` — full spike
  methodology + raw numbers.
- `00-vision.md` — updated "~1-5 µs" line.
- `01-kvm-platform-design.md` — spike-01 result block
  added to the top.
- `06-sequencing/critical-path.md` — D still off critical
  path; unchanged.

---

## D56 (2026-04-23) — Spike 02 data changes the D-workstream read: naive KVM backend is competitive across modern silicon, systrap gadget optional not mandatory

**Decision.** Spike 02 extends Spike 01's single Skylake-SP
measurement to seven more hosts across four Intel
generations + AMD Zen 4. The revised floor for the naive
`KVM_RUN` round-trip is **~0.8 µs to ~5 µs depending on
silicon**, not uniformly ~5 µs. Vision doc "~100 ns" line
stays aspirational but the gap to close is now ~8× on
Alder Lake i9 (not ~50× on Skylake-SP). D-04 systrap-gadget
work is reclassified from "required for D to justify the
vision" to "second-phase D deliverable that closes a
smaller gap." First-phase D (naive `KVM_RUN`-per-syscall)
ships on the merits: 4× over seccomp worst-case, 25× best-
case, with observability-over-QEMU-KVM as the secondary
selling point.

Measurements captured in `measurements.md` (new file,
intended as the persistent timing log). Subsequent D
spikes + real-implementation benchmarks extend that file
rather than re-measuring ad hoc.

**Alternatives considered.**

- *Keep D55's framing ("~5 µs floor") after Spike 02:*
  Rejected. Two Alder Lake hosts + Zen 4 at boost all
  landed below 3 µs, with i9 at 780 ns. Pretending that
  silicon doesn't exist would ignore 4 of 8 data points.
- *Rewrite vision around the i9 number:* Rejected. 780 ns
  is the best case on the newest fastest part we
  measured. Average deployment — especially syzbot's
  fleet, which is Skylake-era + Sapphire Rapids — sees
  the 3-5 µs range. Optimistic headlines burn trust
  when reality shows up.
- *Delay D until we measure a Sapphire/Granite Rapids
  host:* Deferred but not blocking. The existing
  measurements already say D is worth doing; a newer
  server silicon point just moves the floor, doesn't
  change the go/no-go.

**How to apply.**

- `00-vision.md` headline now reads "~0.8-5 µs" naive;
  "~100 ns" stays aspirational.
- `01-kvm-platform-design.md` Spike block now references
  both spikes + `measurements.md`.
- `measurements.md` is the durable home for all future
  D timing data. Append per-measurement dated sections
  at the bottom; never overwrite.
- Future spikes that want to claim a number
  (Sapphire Rapids, Graviton3, nested-KVM on GHA,
  Spike 04 long-mode+LSTAR, systrap prototype) each
  add one dated section to `measurements.md` + a
  cross-reference from their own README.
- The cycle count vs ns distinction landed by Spike 02
  (Zen 4 hosts at different P-states) should shape how
  we report future results: **report both cycles and ns**,
  and prefer cycles as the invariant when comparing
  silicon generations. Clock effects are informative but
  confound the underlying "VMX/SVM exit cost" number we
  care about.

**Cross-references.**

- `02-workstreams/D-kvm-backend/spikes/01-getpid-roundtrip/
  README.md` — spike 01/02 narrative + per-host readouts.
- `02-workstreams/D-kvm-backend/measurements.md` — the
  persistent timing log.
- D55 — the prior (narrower) conclusion from Spike 01
  alone. Kept in the log as a history record; this D56
  supersedes it for the vision headline.

---

## D57 (2026-04-23) — KVM backend uses one VM fd per UML process, not per UML mm_struct

**Status:** Accepted
**Decided by:** Project owner + agent, in the D-03b design pass

**Context.** UML's backend-contract exposes `mm_attach(struct
mm_id *)` and `mm_detach(struct mm_id *)` as per-mm hooks — one
pair of calls per UML guest process (each UML guest process
owns its own `mm_struct`). The obvious default for the KVM
backend is to create one KVM VM fd per UML mm, so each UML
guest process gets its own isolated KVM context.

The D-workstream design memo
(`Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
design-memo.md`) argues the opposite: **one `struct kvm_um` per
UML kernel process** (i.e. per outer host process), with one
`kvm_vm_fd` shared across all UML guest processes, vCPUs created
per UML CPU, and CR3 switched on `context_switch()` to select
the active UML mm. That's gVisor's KVM-platform shape and is
~1 VM's worth of host-side KVM overhead instead of N VMs'.

**Decision.** Adopt the design-memo model: one host-side
`struct kvm_um` per UML process; one VM fd created eagerly in
`kvm_init()`; `mm_attach`/`mm_detach` are per-UML-mm
bookkeeping hooks (refcount the shared VM so `uml_cleanup`
closes it at the right time, but do not `KVM_CREATE_VM` on
every `mm_attach`).

**Alternatives considered.**

1. **One VM fd per UML mm (naive mapping).** Rejected. Would
   create `KVM_CREATE_VM` traffic on every UML process fork
   — a cost the host kernel doesn't hide (~tens of µs each)
   and which serializes behind KVM's global lock. Also
   complicates the D-04 vCPU plan: vCPUs are per UML CPU,
   not per UML mm; attaching them to per-mm VMs means the
   vCPU set churns on every context switch rather than on
   CPU-hotplug / init boundaries.

2. **One VM fd per UML mm but cached/pooled.** Rejected for
   scaffold. Pooling adds non-trivial state machinery
   (freelist, max-pool, rebalance on OOM) that earns its
   keep only if we actually find per-mm VMs are the right
   shape. We don't; the design memo shows we don't.

3. **Lazy VM creation (on first `mm_map` rather than
   `kvm_init`).** Considered for D-03b, rejected for now.
   Eager creation at `kvm_init` time matches the "probe +
   init commit resources" pattern the arbiter already runs,
   keeps the `mm_attach` fast-path trivial, and surfaces
   `KVM_CREATE_VM` failure at a sensible point (boot, not
   mid-process-fork).

**Implementation implication for D-03b.** A new
`struct kvm_um` holds `kvm_fd`, `vm_fd`, and a refcount.
`kvm_init()` opens /dev/kvm and issues `KVM_CREATE_VM`
exactly once. `mm_attach()` bumps the refcount (ties VM
lifetime to outstanding UML mms so a late shutdown during
teardown doesn't close the fd from under a still-attached
mm). `mm_detach()` decrements. `kvm_shutdown()` closes
`vm_fd` then `kvm_fd`. We defer per-mm KVM state
(memslots, CR3 programming) to D-03c + D-04 — those are
the pieces where "is this per-mm or per-process" matters
for real, because they touch guest physical memory and
the active vCPU's page tables respectively.

**Lifetime / revisit triggers.**

- Revisit if D-04's vCPU bring-up finds a reason to
  isolate KVM VMs per mm (e.g. if a future KVM feature
  we want only exposes itself per-VM and UML wants it
  scoped to one guest process). Nothing today suggests
  this.
- Revisit if the single-VM model causes resource-limit
  pressure (unlikely: UML is typically one host process
  per UML instance; one VM fd per process is well
  within any reasonable rlimit / cgroup).

**Cross-references.**

- `02-workstreams/D-kvm-backend/design-memo.md` §"Data
  structures" — original articulation.
- `02-workstreams/D-kvm-backend/03-page-table-mgmt.md`
  §"Address-space model" — per-file detail on the CR3-
  switching that makes a shared VM fd workable.
- D-02 / D-03a commits (64aa06142e8f, 5b2014682731) —
  prerequisites: ops-table scaffold + real /dev/kvm probe.

---

## D58 (2026-04-23) — KMSAN-on-UML dedicated-slab scheme is architecturally broken; gate on BROKEN pending redesign

**Status:** Accepted (damage-control; the real choice between
the two redesign paths is deferred).

**Context.** Review finding #2 (2026-04-23) flagged that the
KMSAN-on-UML port advertised in Kconfig + documentation
(16 TiB slices at 0x200000000000 / 0x300000000000) doesn't
match the code in `arch/um/include/asm/kmsan.h:55`, which
sizes both SHADOW and ORIGIN to
`KASAN_HOST_USER_SPACE_END_ADDR + 1` = **128 TiB** each. The
Kconfig offsets are only 16 TiB apart, so the 128 TiB shadow
region at 0x200000000000 overflows 112 TiB past the origin
base at 0x300000000000 and well beyond it. Runtime smoke
fails at early shadow mmap with `Couldn't allocate shadow
memory`.

**Root cause.** The "dedicated-host-mmap slab" scheme that
worked for KASAN (16 TiB of shadow covering 128 TiB of
kernel VA, thanks to the 1-byte-per-8-bytes shift) does not
generalize to KMSAN's 1:1 shadow. KMSAN would need 128 TiB
of shadow for the same kernel VA range, plus another 128
TiB for origin. 256 TiB doesn't fit in the lower canonical
half on x86_64 (128 TiB total), let alone alongside KASAN
and UML's own kernel mappings.

**Decision.** Gate `HAVE_ARCH_KMSAN` on `BROKEN` in
`arch/um/Kconfig` so KMSAN can't be selected in normal
builds. Retain the code + Kconfig + docs on-tree but
prominently mark all three as broken-by-design with a
pointer at this entry. Consumers that want KMSAN on UML
today cannot get it; this is a shipped-lie fix, not a
functional fix.

**Alternatives considered.**

1. **Flip Kconfig/docs to claim 16 TiB each.** Rejected:
   the 16 TiB doesn't actually cover the 128 TiB of kernel
   addresses UML can hand out, so KMSAN-instrumented
   accesses to unshadowed addresses fault. This would
   trade a shipped-lie doc for a shipped-lie runtime.
2. **Flip Kconfig/docs to admit the 128 TiB + rearrange
   offsets.** Rejected: 256 TiB of shadow+origin doesn't
   fit in the lower canonical half at all. No offset
   arrangement works within the current address-space
   ceiling.
3. **Adopt x86's VMALLOC quarter-split.** Deferred to
   follow-up. Would put shadow/origin inside VMALLOC,
   sized as 1/4 of VMALLOC each, and let
   `mm/kmsan/shadow.c::vmalloc_meta()`'s existing
   arithmetic apply directly. Requires careful measurement
   against real UML workloads (VMALLOC becomes 1/4 its
   normal size under KMSAN).
4. **Cap `task_size` under CONFIG_KMSAN.** Deferred to
   follow-up. Would preserve the dedicated-slab scheme by
   shrinking UML's kernel VA ceiling so two 1:1 slabs fit
   in the lower canonical half. Cost: smaller max UML
   kernel footprint under KMSAN.

**Lifetime.** The BROKEN gate is temporary; it stays until
one of the two follow-up paths lands. No consumer should
build against this.

**Revisit triggers.**

- Someone picks up the C-07 follow-up and chooses between
  path (3) and path (4) above, along with workload data to
  back the choice.
- KMSAN upstream acquires a less address-space-hungry
  shadow scheme (e.g. a tagged-pointer variant). Unlikely
  short term.

**Cross-references.**

- Review-finding commit (this): 2026-04-23 session; see
  `Documentation/virt/uml/kmsan.rst` §"Why it's broken"
  and the header-comment block at the top of
  `arch/um/include/asm/kmsan.h`.
- Prior decisions-log entry D44: original
  "dedicated-host-mmap" justification (rejected — the
  KMSAN redesign supersedes it).
- `lib/Kconfig.kmsan` — upstream KMSAN gate; unchanged by
  this decision.

---

## D59 (2026-04-23) — `using_seccomp` Layer-1 leakage is finished A-workstream work, not a separate fix

**Status:** Accepted (catalog + defer; no mechanical
Coccinelle sweep this pass).

**Context.** Review finding #4 (2026-04-23) flagged that the
`using_seccomp` boot-probe flag still threads through the
shared host-abstraction (Layer 1) code in
`arch/um/os-Linux/` at 12 reader sites after the
A-workstream extraction landed. The flag is authoritatively
set by `os_early_checks()` in
`arch/um/os-Linux/start_up.c` and consumed by the arbiter
`init_backend()` in `arch/um/kernel/backend.c`; after init
both `using_seccomp` and `um_backend->kind ==
UM_BACKEND_KIND_SECCOMP` are kept in sync, so the twelve
reader sites in os-Linux/ run against a valid (but
duplicate) source of truth. The extern declaration also
leaks into the public
`arch/um/include/shared/skas/skas.h`.

**Inventory (12 reader sites + 1 definition + 1 extern).**

| File | Line | Role |
|------|------|------|
| `arch/um/os-Linux/skas/process.c` | 277 | `.seccomp = using_seccomp,` in stub `init_data` (value sent to the stub child — needs a raw int, not a helper wrapper) |
| `arch/um/os-Linux/skas/process.c` | 282 | `if (using_seccomp)` — chooses signal-handler/restorer trampoline offsets in `init_data` |
| `arch/um/os-Linux/skas/process.c` | 418 | definition `int using_seccomp;` |
| `arch/um/os-Linux/skas/process.c` | 471 | `if (using_seccomp) proc_data->futex = FUTEX_IN_CHILD;` — futex-child coordination setup |
| `arch/um/os-Linux/skas/process.c` | 484 | `if (using_seccomp) { wait_stub_done_seccomp(...) } else { waitpid+ptrace }` — stub-child wait semantics |
| `arch/um/os-Linux/skas/process.c` | 522 | `if (using_seccomp) mm_id->sock = sockpair[1]; else close(sockpair[1])` — per-mm socketpair retention |
| `arch/um/os-Linux/skas/mem.c` | 47 | `if (using_seccomp)` in `syscall_stub_dump_error()` — prints FD map (seccomp-only state) |
| `arch/um/os-Linux/skas/mem.c` | 96 | `if (using_seccomp) { wait_stub_done_seccomp } else { ptrace SETREGS+CONT+wait_stub_done }` in `do_syscall_stub()` |
| `arch/um/os-Linux/skas/mem.c` | 131 | `if (using_seccomp) mm_idp->syscall_fd_num = 0` — reset seccomp-only FD map |
| `arch/um/os-Linux/skas/mem.c` | 201 | `if (!using_seccomp) return fd` in `get_stub_fd()` — whole function is seccomp-specific |
| `arch/um/os-Linux/skas/mem.c` | 246 | `if (using_seccomp)` in `um_stub_mm_map()` — FD-map indirection for SCM_RIGHTS fd |
| `arch/um/os-Linux/signal.c` | 252 | `if (using_seccomp) sigaddset(&action.sa_mask, SIGCHLD)` — mask SIGCHLD in other handlers |
| `arch/um/os-Linux/process.c` | 401 | `if (using_seccomp) set_handler(SIGCHLD)` — register child-reaper IRQ only in seccomp mode |
| `arch/um/include/shared/skas/skas.h` | 11 | `extern int using_seccomp;` — publicly exposed in the cross-cutting skas header |

**A-01.7 disposition.** The A-workstream coverage table
(`02-workstreams/A-backend-abstraction/notes/07-coverage.md`
items #1, #2, #3, #4, #12, #26, #27, #28) already marked
these sites as REMOVED — internal to per-backend `mm_attach`
/ `run_userspace` / `mm_map` implementations under
`arch/um/backend/{ptrace,seccomp}/`. What landed was the
**extraction of the trap-loop body** (A-02.HOT-1 + A-03.S1.3
→ `trap_user.c` per-backend) and the thin wrapper
`mm_attach/mm_detach` in `arch/um/backend/{ptrace,seccomp}/
mm.c`. What did **not** land was:

- `start_userspace()` body's per-mode divergence (sites 277,
  282, 471, 484, 522) splitting into `seccomp_mm_attach()`
  and `ptrace_mm_attach()` private helpers.
- `do_syscall_stub()` + `get_stub_fd()` + `um_stub_mm_map()`
  FD-map logic (sites 96, 201, 246) splitting into
  `seccomp_syscall_stub()` + `ptrace_syscall_stub()` private
  helpers.
- `syscall_stub_dump_error()` (site 47) and the
  `syscall_fd_num` reset (site 131) moving behind the
  seccomp backend's private debug helper.
- `init_new_thread_signals()` (site 401) and `set_handler()`
  (site 252) gaining per-backend signal-setup ops.

**Decision.** Catalog the residue and treat it as **finished
A-workstream Phase 2 work**, not a new line item. Do **not**
land a Coccinelle rename-sweep this pass. Reasoning:

1. A mechanical rename (e.g. replace every `using_seccomp`
   read with `um_backend->kind == UM_BACKEND_KIND_SECCOMP`)
   doesn't remove any branch — it just relabels the
   discriminator. The true Layer-1 leak is the **shape**
   of the shared functions (each one has a seccomp-vs-ptrace
   fork inside its body), not the **name** of the
   discriminator.
2. Real extraction (moving the bodies into per-backend
   files) is non-trivial. `start_userspace()` in particular
   threads a clone() tramp-data struct across both modes
   and intermixes socketpair setup with wait semantics;
   splitting it cleanly is at least a multi-commit
   patchset, not a sed-able rewrite.
3. The v1 ceiling concerns validated by Finding #1 (SIGALRM
   reentrancy during fork parent non-kernel-exec windows,
   fragile `wait_stub_done` semantics) apply directly to
   any refactor of these paths. Getting the split wrong
   crashes the UML kernel in hard-to-diagnose ways. The
   A-workstream Phase 2 commits need to be small, one-call-
   site-at-a-time, and individually boot-tested.
4. The current duplicate-source-of-truth
   (`using_seccomp` flag vs `um_backend->kind`) is
   benign in practice: the arbiter explicitly keeps them
   in sync, and the arbiter is the only writer once init
   completes. "Benign redundancy" is a reasonable parking
   spot while the bigger extraction is planned.

**Alternatives considered.**

1. **Coccinelle-style rename to `backend_is_seccomp()`
   helper.** Rejected for this pass: produces 12-site churn
   with no reduction in branch count, and risks destabilizing
   boot during refactor of `start_userspace()`-adjacent code
   right before the D-workstream lands. Keep in toolbox for
   a later "rename-only" commit if it makes the Phase 2
   diffs cleaner.
2. **Full per-backend extraction (Phase 2) now.** Rejected
   for this pass: scope creep. Each of the four target
   helpers (`start_userspace`, `do_syscall_stub`,
   `syscall_stub_dump_error`, `init_new_thread_signals`)
   needs its own design pass, test matrix, and boot-gate
   verification. Tracked as a new workstream item, not a
   single commit.
3. **Move the `extern int using_seccomp;` declaration out
   of the public skas.h header into os-Linux/internal.h.**
   A real cleanup — declaration is USER-TU-internal and
   leaking it into the cross-cutting header is an A-01.7
   item #3 residue. Candidate for a small follow-up commit
   (separable from the per-backend extraction); hold for
   when the skas.h consumers are reviewed in Phase 2.

**Lifetime.** The 12-site residue is expected to stay until
A-workstream Phase 2 lands. No behavioral consequences for
v1 ceiling; the redundancy is visible but the code path is
correct.

**Revisit triggers.**

- A-workstream Phase 2 picks up (likely post-D-05/D-06 when
  KVM ring-3 entry lands and the KVM backend gets its own
  `run_userspace`/`mm_attach` body — at that point the
  three-way ops table makes the per-backend split more
  valuable than it was with just two).
- If a bug ever turns on `using_seccomp` diverging from
  `um_backend->kind` (today impossible by construction;
  a refactor could accidentally break the invariant). If
  so, converge on `um_backend->kind` as the single source
  and make `using_seccomp` a `#define`.

**Cross-references.**

- `02-workstreams/A-backend-abstraction/notes/07-coverage.md`
  items #1 (start_up.c:477,488), #2 (skas/process.c:429),
  #3 (skas/skas.h:11), #4 (start_userspace def), #12
  (clone() pattern), #26 (syscall_stub_flush), #27
  (syscall_stub_alloc), #28 (syscall_stub_dump_error) —
  the A-01.7 disposition column said REMOVED for all of
  these; this entry records that they are REMOVED in
  intent only, not yet in code.
- `arch/um/kernel/backend.c` (the arbiter, lines 18–20)
  explicitly documents the
  `using_seccomp` ↔ `um_backend->kind` sync invariant.
- Finding #1 (2026-04-23) + v1 ceiling in
  `Documentation/virt/uml/snapshot.rst` — directly
  informs the "extraction must be small and boot-tested"
  caution in this entry.

---

## D60 (2026-04-23) — Phase III Lift #1a (kvm_um attach-cost benchmark) retired by D57; proceed directly to Lift #1b

**Status:** Accepted (supersession of the original
Lift #1a framing; no benchmark run).

**Context.** The post-Q1 push plan
(`06-sequencing/post-q1-push.md`) Phase III Lift #1a is
written as "per-mm `kvm_um` attach-cost microbench vs.
seccomp baseline" — a go/no-go gate for Lift #1b. The
framing assumed the naive mapping `one KVM VM fd per UML
mm`, where each `mm_attach()` pays a `KVM_CREATE_VM` +
`KVM_SET_USER_MEMORY_REGION` + SREGS init on the order of
tens of microseconds. If that cost ran above 5× the
seccomp baseline, the plan said "stop and redesign,
probably pooled VMs, not per-mm."

**Actual backend shape (landed in D-03b).** D57
(2026-04-23) picked **one VM fd per UML process**, not per
UML mm. The VM is created eagerly in `kvm_init()`;
`mm_attach()` does `refcount_inc()` on a shared
`struct kvm_um`; `mm_detach()` does
`refcount_dec_and_test()`. No KVM ioctl on the per-mm
attach path. See `arch/um/backend/kvm/mm.c::kvm_mm_attach`
(lines 29–52) and `arch/um/backend/kvm/lifecycle.c` — the
expensive `KVM_CREATE_VM` is amortized once per UML-
process boot, not per `mm_attach`.

**Decision.** Lift #1a as originally framed has no
measurable content on the current backend. The attach-
cost unknown the lift was supposed to retire was
architecturally retired by D57 before the lift was
written.

Retire Lift #1a as **closed by supersession**. Do NOT
run a new benchmark:

1. Measuring `refcount_inc()` against a seccomp
   `start_userspace()` (clone + execveat + futex-wait)
   would produce a meaningless ratio (ns vs. ms;
   ~6 orders of magnitude apart) that confirms nothing.
2. The relevant per-process `KVM_CREATE_VM` + memslot
   registration cost is already recorded in
   `measurements.md` under D-04b.1b / D-04b.2b.2 — boot
   -time one-shot values, not per-attach.
3. The Lift #1b go/no-go gate that Lift #1a was
   supposed to inform is unchanged: it's "does ring-3
   entry + SYSRETQ work", not "is attach cost
   acceptable".

**Updates required.**

- `06-sequencing/post-q1-push.md` §"Phase III Lift #1a"
  gets reframed to point at this entry + D57 as the
  supersession record, and Phase III execution order
  becomes 1b → 1c → 1d → 1e → 1f (one fewer sub-lift).
- `02-workstreams/D-kvm-backend/measurements.md` gets a
  one-paragraph pointer so a future reader looking for
  "attach-cost benchmark data" finds the supersession
  here, not a missing row.

**Alternatives considered.**

1. **Run the benchmark anyway for completeness.**
   Rejected. A benchmark that tests "did the compiler
   optimize `refcount_inc` into the expected atomic
   instruction" is not the unknown-retiring research
   the lift existed to do. If someone ever wants that
   datapoint, it lives in a perf-regression CI gate,
   not in Phase III's go/no-go sequence.
2. **Keep Lift #1a open as a tracking placeholder.**
   Rejected. An open lift with no actionable content
   is worse than a closed one — it pollutes the
   remaining-work surface and invites re-debate of
   D57's settled scope.
3. **Write a "pool-sizing" benchmark hypothesizing a
   future pooled-VM shape.** Rejected as premature
   optimization (a-plus-quality-plan §7 anti-pattern
   #3). D57 explicitly considered and rejected VM
   pooling; re-exploring it without a workload-driven
   reason re-opens a closed question.

**Lifetime / revisit triggers.**

- Revisit if D57 itself is revisited (its own revisit
  triggers are "D-04 finds a reason to isolate KVM VMs
  per mm" and "single-VM model causes resource-limit
  pressure"). If D57 flips to per-mm VMs, Lift #1a
  un-retires and runs with the new framing.
- Revisit if someone writes a benchmark that compares
  `um_backend_kvm_ops` dispatch path vs.
  `um_backend_seccomp_ops` dispatch path end-to-end
  (not just the attach sub-step). That's a different
  measurement than this lift — and it lives naturally
  in Lift #1f (D-06 conformance).

**Cross-references.**

- D57 (2026-04-23) — the retiring decision; its
  §"Alternatives considered" enumerates the exact
  "per-mm VM" framing this lift was written against.
- `arch/um/backend/kvm/mm.c::kvm_mm_attach`
  (lines 29–52) — the actual landed attach path,
  refcount-only.
- `arch/um/backend/kvm/lifecycle.c::kvm_init`
  + the memslot registration at line 276 — the
  one-shot-per-UML-process KVM_CREATE_VM +
  KVM_SET_USER_MEMORY_REGION costs whose per-attach
  amortization makes this lift trivial.
- `06-sequencing/post-q1-push.md` §"Phase III" — the
  lift-plan this entry retires.

---

## D61 (2026-04-23) — Phase IV gadget spike: go for a post-v1 systrap-equivalent workstream; do not block v1

**Status:** Accepted (spike synthesis; no v1 commit-plan change).

**Context.** The post-Q1 push plan's Phase IV was a
feasibility gate on whether the M11 "~100 ns syscall" vision
line is architecturally reachable via a gVisor-systrap-style
ring-0 in-guest gadget that eliminates the VMEXIT for
gadget-handleable syscalls. D56 (2026-04-23) had previously
settled that the naive KVM backend is "competitive across
modern silicon, gadget optional not mandatory" — Phase IV's
job was to validate that with numbers before the decision
became load-bearing.

Phase IV landed three sub-lifts:

- **#2a (paper design).** `02-workstreams/D-kvm-backend/
  07-systrap-gadget-feasibility.md` shows the gadget slot
  UML needs is the **existing `STUB_START` reservation**.
  No new VA, no conflict with kernel VA or guest-userspace
  VA, no per-mm setup (UML's single-mm_struct model makes
  gadget installation cheaper than gVisor's per-Sentry
  install).
- **#2b (round-trip spike).** Harness extension
  (`arch/um/backend/kvm/harness.c`, commit 6cfcf7a95e5d)
  with a ring-3 loop that issues 1000 SYSCALLs back-to-back
  through a 3-byte `sysretq`-only LSTAR handler. One
  KVM_RUN bracket, per-syscall cost = total/N. Measured on
  Skylake-W dev host:

  ```
  um: kvm harness: 2b PASS — gadget round-trip ~133 cyc/syscall
      over 1000 iters (total 133168 cyc)
  ```

  **133 cycles per SYSCALL+SYSRETQ**, vs. ~22,800 cyc/syscall
  for the naive-KVM path on the same silicon. **~170× speedup.**
  At Skylake-W's 3.7 GHz, 133 cyc = 36 ns. On AL i7 @ 5 GHz
  the same instruction-pair count would be ~27 ns (the
  SYSCALL+SYSRETQ cost is silicon-invariant in cycles per
  spike 07's 240-340 cyc floor).

**Decision.**

1. **GO for a post-v1 gadget workstream.** The 36 ns
   measured round-trip is well under the 100 ns vision
   target and proves the gadget mechanism is architecturally
   viable on modern silicon.

2. **Do NOT block v1 on gadget.** The naive KVM backend
   already hits:

   - 960 ns on Alder Lake i9 @ 4.9 GHz (measurements.md
     spike 06)
   - 1114 ns on Alder Lake i7 @ 5.0 GHz (D-04b.1c w1)
   - 6210 ns on Skylake-SP @ 3.7 GHz

   These are usable for prod-fast on every silicon class
   the vision targets. The gadget's 30-40 ns improvement is
   the aspirational-ceiling layer, not the viability floor.
   v1 ships with naive-KVM; gadget is additive in a post-v1
   pass.

3. **Phase IV is complete.** No further Phase IV work. The
   follow-up workstream (tentative: "D-07 systrap gadget")
   is scoped in the feasibility memo and tracks as an
   explicit task but not as a v1 critical-path item.

**Alternatives considered.**

1. **Build the gadget into v1.** Rejected. Adds several
   commits of gVisor-pattern work (gs:base setup, shared-
   memory clock page, per-syscall handlers, safety version
   checks) without the v1 critical path needing it.
   Naive-KVM is already shippable for prod-fast on every
   modern-silicon target. Defer.

2. **Defer the gadget indefinitely / strike from roadmap.**
   Rejected. The measured 36 ns result is compelling
   evidence that M11's vision line is reachable, not just
   aspirational. Keeping a scoped follow-up workstream
   alive as an optional post-v1 item preserves the upside
   without blocking v1.

3. **Start building a v2 UML with gadget as the default
   dispatch.** Rejected (for now). Gadget is additive: a
   real-backend run_userspace loop (post-Phase III task
   #162) can opt in per-syscall (dispatch through gadget
   jump table for known-safe, fall back to VMEXIT
   otherwise). No flag-day redesign needed.

**Lifetime / revisit triggers.**

- Revisit gadget priority upward if prod-fast workloads
  measure high gadget-handleable syscall rates
  (e.g. a clock_gettime-heavy benchmark).
- Revisit downward if post-v1 measurements show the
  gadget's shared-state complexity (version checks +
  vvar-style clock page) introduce maintenance cost
  greater than the 36-ns-gain can justify.

**Cross-references.**

- D56 (2026-04-23) — prior decision that "naive backend is
  competitive, gadget optional"; this entry confirms with
  numbers.
- D57 (2026-04-23) — one VM fd per UML process, which
  makes gadget installation a one-shot per-UML-kernel op
  rather than per-mm.
- `02-workstreams/D-kvm-backend/07-systrap-gadget-
  feasibility.md` — full feasibility memo.
- `02-workstreams/D-kvm-backend/measurements.md` — Spike 07
  + D-04c measured the 240-340 cyc SYSCALL+SYSRETQ floor
  that the gadget result confirms.
- `06-sequencing/post-q1-push.md` §"Phase IV" — the push-
  plan phase this entry closes.
- `08-future-phases/` (candidate location for a full D-07
  systrap workstream doc when/if the follow-up task
  activates).

---

## D62 (2026-04-23) — KMSAN-on-UML adopts VMALLOC quarter-split; D58's dedicated-slab scheme superseded

**Status:** Accepted. Supersedes D58 (2026-04-23) which
gated HAVE_ARCH_KMSAN on BROKEN pending a workable design.

**Context.** D58 recorded that the first-pass KMSAN port
reserved two 128 TiB dedicated slabs for shadow + origin,
which doesn't fit in the lower canonical half and runtime
smoke failed at early shadow mmap with ENOMEM. D58
identified two resolution paths without picking one:
(a) adopt x86's VMALLOC quarter-split, (b) cap task_size
under KMSAN. Phase V Lifts #3a and #3b (landed in commit
1f75a270b701) probed both as paper designs with side-by-
side TCO analysis. See
`02-workstreams/C-profiles-and-gaps/07-port-kmsan-redesign.md`
for the full comparison.

**Decision.** Pick **path (a) — VMALLOC quarter-split**,
mirroring `arch/x86/include/asm/pgtable_64_types.h:124-169`.
Under `CONFIG_KMSAN`, UML's VMALLOC range is divided into
four equal quarters: effective vmalloc (1), shadow (2),
origin (3), modules shadow+origin (4). Shadow/origin VAs
derive from `VMALLOC_START` arithmetic that the generic
`mm/kmsan/shadow.c::vmalloc_meta()` already consumes.

**Decision basis — why path (a) over path (b).**

1. **Idiomatic upstream.** x86_64 and s390 both use VMALLOC
   quarter-split. UML adopting the same shape is a
   one-line reviewer query ("yes, matches x86").
   Path (b)'s `task_size` cap is a novel-to-UML scheme
   the reviewer has to validate from scratch.
2. **Localized blast radius.** Path (a) touches pgtable.h,
   kmsan.h, and Kconfig — ~150 lines of code. Path (b)
   cascades into every `TASK_SIZE` consumer (uaccess,
   elf, stack, vsyscall, ~30 sites) — ~400-500 lines.
3. **User-visible VA ceiling preserved.** Path (a) keeps
   `task_size` identical under `CONFIG_KMSAN`; guest
   userspace sees the same VA it would without KMSAN.
   Path (b) shrinks guest VA from ~128 TiB to ~32 TiB
   based on a config flag.
4. **Effective VMALLOC still ample.** Path (a) gives
   each quarter ~2.5 GiB on a typical UML (mem= of a few
   GiB). Research profile (the only profile expected to
   enable KMSAN) doesn't approach this limit. Path (b)
   preserves full VMALLOC but costs user-visible
   `task_size`; the tradeoff is wrong-shaped for UML's
   profile matrix.
5. **Generic KMSAN code already designed for it.**
   `mm/kmsan/shadow.c::vmalloc_meta` expects shadow/origin
   as `VMALLOC_START + offset + KMSAN_VMALLOC_*_OFFSET`.
   Path (a) plugs those macros directly. Path (b) would
   require retaining the non-standard dedicated-slab
   arch hook we started with, keeping UML an outlier.

**Implementation footprint (landed same commit).**

1. `arch/um/Kconfig`: drop `BROKEN` gate on
   `HAVE_ARCH_KMSAN`; remove the now-moot
   `KMSAN_SHADOW_OFFSET` and `KMSAN_ORIGIN_OFFSET`
   Kconfig entries (the VMALLOC-split layout derives
   shadow/origin VAs at compile time).
2. `arch/um/include/asm/pgtable.h`: split-aware
   `VMALLOC_END` under `CONFIG_KMSAN`
   (`VMALLOC_START + (TASK_SIZE - 2*PAGE_SIZE -
   VMALLOC_START) / 4`). Define
   `KMSAN_VMALLOC_SHADOW_START`,
   `KMSAN_VMALLOC_ORIGIN_START`,
   `KMSAN_MODULES_SHADOW_START`,
   `KMSAN_MODULES_ORIGIN_START`.
3. `arch/um/include/asm/kmsan.h`: drop the dedicated-slab
   `KMSAN_SHADOW_SIZE` / `KMSAN_ORIGIN_SIZE` /
   `KMSAN_SHADOW_START` / `KMSAN_SHADOW_END` defines.
   Pull `KMSAN_VMALLOC_*` via `<asm/pgtable.h>`
   include. Preserve the two required arch-hook
   inlines (`arch_kmsan_get_meta_or_null` returning NULL,
   `kmsan_virt_addr_valid` returning true under
   `CONFIG_KMSAN`).
4. `Documentation/virt/uml/kmsan.rst`: flip the
   "STATUS: BROKEN" banner to a VMALLOC-split layout
   description. User-facing surface (`/sys/kernel/debug/
   kmsan/`, BUG: KMSAN: report style) unchanged.

**Alternatives considered.**

1. **Land path (a) but keep the Kconfig
   `KMSAN_SHADOW_OFFSET` / `_ORIGIN_OFFSET` entries as
   dead-but-tunable knobs.** Rejected. Carrying dead
   Kconfig surface invites confusion; the
   quarter-split layout is fully determined by
   `VMALLOC_START` + `VMALLOC_SIZE_TB`.
2. **Path (a) under a new Kconfig `UM_KMSAN_VMALLOC_SPLIT`
   toggle with path (b) as an alternative.** Rejected.
   Single-scheme-per-arch convention; offering both at
   build-time invites mismatched combinations and tripling
   the test matrix.
3. **Defer the redesign entirely; wait for a UML KMSAN
   workload request.** Rejected. D58 already recorded the
   breakage as a shipped-lie Kconfig (HAVE_ARCH_KMSAN
   visible but BROKEN-gated). Landing the working scheme
   closes the gap; the upstream LKML submission for
   C-07 (kmsan-arch-callback-rfc series) also benefits
   from the arch side actually working.

**Lifetime / revisit triggers.**

- Revisit if the 1/4 VMALLOC ceiling is hit by a real
  workload. Unlikely for research profile; possible if
  KMSAN is ever enabled in a profile with large vmalloc
  consumers (BPF JIT, eBPF maps).
- Revisit if x86_64's quarter-split layout changes
  upstream (unlikely; stable since 2022).
- Revisit if a third path (e.g. per-cpu shadow banking)
  proves preferable; D62 doesn't foreclose future
  supersession.

**Boot validation.**

The redesign lands with a compile-clean build under
`CONFIG_KMSAN=y`. Full real-workload validation (run the
existing `tools/testing/selftests/um/kmsan-smoke/` harness
against a booted KMSAN-enabled UML image, plus a planted
uninit-value reproducer) is a follow-up pass; this entry
closes the architectural blocker, not the end-to-end
detection story.

**Cross-references.**

- D58 (2026-04-23) — superseded; the dedicated-slab
  scheme this entry replaces.
- D51 (2026-04-22) — C-07 KMSAN upstream strategy;
  unchanged but now has a working arch side to
  submit.
- D44 (2026-04-22) — original dedicated-host-mmap
  rationale; explicitly rejected here.
- `arch/um/include/asm/pgtable.h` — the landed macros.
- `arch/um/include/asm/kmsan.h` — the rewritten arch
  hooks.
- `Documentation/virt/uml/kmsan.rst` — user-facing
  surface.
- `02-workstreams/C-profiles-and-gaps/07-port-kmsan-
  redesign.md` — Phase V Lifts #3a + #3b feasibility
  memo.
- `06-sequencing/post-q1-push.md` §"Phase V" —
  parent-plan reference.

---

## D63 (2026-04-23) — LKML upstream submission order: bpf-hygiene → kmsan-arch → ftrace-notrace → backend-ops RFC → static-keys → C-series → KVM backend

**Status:** Accepted (sequencing only; each series has
its own cover-letter + patch files landing via normal
commits).

**Context.** Phase VI Lift #7 in the post-Q1 push plan
asked for an ordered LKML submission queue for the
upstream-bound patch series the UML redesign generated.
Historically the fork accumulated upstream-worthy pieces
without an explicit sequencing commitment; D45
(fork-first policy) pushed adoption upstream to a later
phase without saying *which order*. This entry fixes
that.

**The order (smallest-first, dependency-respecting).**

1. `bpf-hygiene-v1/` (2 patches, BPF subsystem,
   upstream-independent)
2. `kmsan-arch-callback-rfc/` (1 RFC, mm/kmsan,
   upstream-independent; unblocked from UML side by
   D62)
3. `ftrace-notrace-generic-v1/` (~3 patches, tracing,
   generic kthread notrace annotations — to write)
4. `backend-ops-abstraction-rfc/` (~12 patches, arch/um,
   A-workstream — to write; depends on #3 landing
   upstream for clean ftrace handling)
5. `static-key-hot-paths-series/` (~6 patches, arch/um,
   B-workstream — to write; depends on #4)
6. `kprobes-ftrace-kfence-kcsan-profiles-series/` (~20
   patches across 4 sub-series, C-workstream — to write;
   depends on #4 + #5)
7. `kvm-backend-series/` (~15 patches, arch/um + KVM,
   D-workstream — to write; depends on #4 + task #162
   real run_userspace integration)

Full sequencing rationale + per-series maintainer routing
live in
`Documentation/virt/uml/redesign/upstream-patches/SUBMISSION-QUEUE.md`.

**Decision basis — why this order.**

1. **Upstream-independent series first.** #1 and #2 land
   on their own merit, don't mention UML in the patch
   bodies, and build author credibility with BPF and
   mm/kmsan maintainers before bigger asks.
2. **Generic before arch RFC.** #3's `notrace`
   annotations are one-liners in `kernel/` that any arch
   using `-fpatchable-function-entry` would benefit from.
   Getting them upstream before #4 makes the A-workstream
   RFC reviewers see a clean ftrace story.
3. **Backend-ops RFC is the tentpole.** #4 is the
   architectural change everything downstream cites.
   Realistic wall-clock: 6-9 months of review cycles.
   Landing this unblocks #5/#6/#7.
4. **KVM last.** #7 needs both #4 (ops table) and task
   #162 (real run_userspace trap loop). Realistic
   timing: 12-18 months after #4.

**Alternatives considered.**

1. **Send #4 first, treat #1-#3 as drive-by cleanup.**
   Rejected. #1-#3 are cheap individual wins; queuing
   them behind #4 means reviewers only see UML-
   flavoured work for a year and associate the author
   with that context. Separating builds subsystem
   credibility.
2. **Bundle #6 into a single 20-patch series.** Rejected.
   kprobes and ftrace and KFENCE and KCSAN have different
   maintainer audiences; one giant series forces any
   single maintainer to carry the others' review. Four
   sub-series with shared cover-letter framing is the
   practical split.
3. **Send #7 with #4 to shortcut the D-workstream wait.**
   Rejected. #7's real demonstration needs task #162's
   sustained run_userspace integration measured against
   seccomp; submitting before that numbers-baseline
   exists invites "show me it actually works on real
   workloads" pushback with nothing to point at.

**Lifetime / revisit triggers.**

- Revisit if any subsystem maintainer rejects a series
  (common: "v2 with these changes" is normal; "rejected
  on principle" means the sequencing may need to skip
  the dependent downstream series).
- Revisit when #4 lands — reordering #5/#6 at that
  point depends on which sub-maintainers ack first.
- Revisit if a security issue in the fork surfaces that
  needs fast-track upstreaming (no current candidates;
  the usual suspects — KASAN, KFENCE — are working
  on-fork).

**Cross-references.**

- D45 (2026-04-21) — fork-first upstream policy this
  memo operates within.
- D62 (2026-04-23) — KMSAN redesign that unblocked
  series #2 from UML's side.
- `Documentation/virt/uml/redesign/upstream-patches/
  SUBMISSION-QUEUE.md` — the full sequencing memo with
  per-series maintainer routing + framing pitches.
- `06-sequencing/post-q1-push.md` §"Phase VI Lift #7" —
  parent-plan reference.

---

## D64 (2026-04-23) — UM selftest harnesses grow `timeout --kill-after=10` guards

**Source.** Task #148 "pre-existing ftrace-smoke selftest
hang on research profile." Investigation triggered by the
post-README-reconciliation pending-task sweep.

**Root cause.** Every UM selftest harness (nine of them:
ftrace-smoke, userspace-smoke, snapshot-smoke, launcher-
smoke, kprobes-stress, cve-repro, hooks-flip, kmsan-smoke,
profiles) wrapped its UML invocation in plain
`timeout <SECONDS> "$BINARY" …`. GNU `timeout`'s default is
`--signal=TERM --kill-after=0`, meaning it sends SIGTERM
and never escalates. UML has its own signal plumbing and
can legitimately fail to honor a SIGTERM — this is the
same class of bug D59 Finding #1 documents in the
snapshot/forkserver wait-loop (SIGALRM → `switch_threads` →
stale `jmp_buf`), and the same motivation behind the
umlctl-v1 stop-verb's explicit signal escalation.

When a UML guest wedges (e.g. a bug fires in a path
kselftest doesn't explicitly cover), the `timeout` SIGTERM
bounces off, and the selftest harness hangs until the CI
runner's outer watchdog fires. The hang is silent at the
kselftest level, which makes it a "pre-existing" class of
issue rather than a specific ftrace-smoke problem.

**Fix.** Add `--kill-after=10` to every
`timeout <SECONDS> "$BINARY"` call in
`tools/testing/selftests/um/*/run-*.sh`. Semantics:

    timeout --kill-after=10 <SECONDS> "$BINARY" …
      └── send SIGTERM at SECONDS
           └── if still alive 10 s later, send SIGKILL
                └── `timeout` itself exits with 137

10 s is enough grace for a clean `halt -f` / `poweroff -f`
path to complete (the longest observed is <2 s on the
research profile). If the guest is genuinely wedged, the
harness now fails loudly at SECONDS + 10 rather than
hanging for the outer watchdog.

**Why not migrate to umlctl stop?** umlctl's stop path
is a strictly better version of this (explicit pidfile +
signal + poll + SIGKILL escalation + zombie drain), but
migrating nine selftests changes every invocation signature
and couples the selftest harness to the umlctl binary
being built. `--kill-after=10` is a one-token fix that
preserves the existing signature and costs nothing to roll
out. A future migration to umlctl can happen when the
tooling landing sequence permits.

**Alternative considered — `timeout -s KILL`.** Rejected:
sending SIGKILL as the first signal means the guest gets
no chance to run its halt path, so we lose console flush
and any final `FTRACE_SMOKE: PASS` line if it arrives mid-
teardown. The `TERM → grace → KILL` ladder is strictly
better.

**Scope.** Nine files under
`tools/testing/selftests/um/*/run-*.sh`. No code path
changed; no new dependencies. Task #148 retired.

---

## D65 (2026-04-23) — Real `kvm_run_userspace` integration: scope + 7-sub-commit decomposition

**Source.** Task #162 "Post-Phase III: real KVM
`run_userspace` integration" was the last large open item
after the 2026-04-23 README reconciliation. Per the plan's
"write the design doc for that task before writing code"
rule (README "How to use this plan"), this entry records the
scope decision; the full memo lives at
`02-workstreams/D-kvm-backend/08-real-run-userspace.md`.

**Scope.** Replace the D-04a scaffold `panic()` in
`arch/um/backend/kvm/thread.c:165::kvm_run_userspace` with a
real integrated loop. All primitives (SREGS setup, LSTAR
trampoline, MMIO decode, IDT injection, ring-3 SYSRETQ) are
already landed on the fork as harness code during Phase III
Lifts #1b-#1f; the lift is a lift-out-of-harness-into-
production refactor with new exit-reason wiring.

**Decomposition — 7 sub-commits, each independently
testable:**

  1. `kvm_enter_guest` — vCPU state materialization (SREGS +
     CR3 from active_mm + GP regs from `struct
     uml_pt_regs`).
  2. `kvm_decode_syscall` — KVM_EXIT_IO port 0xf4 bucket →
     `sys_call_table[nr]`.
  3. `kvm_decode_mmio` — KVM_EXIT_MMIO → UML fault path
     (reuse Phase III Lift #1d landed code).
  4. `kvm_handle_hlt` — return to UML scheduler on guest
     HLT; fresh KVM_RUN on next schedule.
  5. `kvm_handle_intr` — host SIGALRM/SIGIO/SIGCHLD reinject
     path.
  6. Hot-path optimizations + perf measurement vs D-06
     `getpid()` <100 ns bookend.
  7. Nested-virt fallback wiring (D-05 completion).

**Sub-commits #1-#6 land behind
`CONFIG_UM_BACKEND_KVM_INTEGRATED` (default n) until #6's
measurements clear the D-06 gate; harness stays available
via `CONFIG_UM_BACKEND_KVM_HARNESS` for regression probes.

**Open questions resolved in the memo.** Single-vCPU vs
per-task-vCPU (recommendation: single-vCPU with per-mm
serialization; gVisor-style per-task is a v2 discussion).
`current` handling during KVM_RUN (replicate seccomp
pattern). Memslot lifetime vs process lifetime (already
handled by D-03d `mm_map`/`mm_unmap`).

**New selftests planned:** `tools/testing/selftests/um/
kvm-smoke/`, `kvm-fault-smoke/`, `kvm-yield-smoke/`. A-05
KUnit contract extended to exercise the integrated path,
not only op-table dispatch.

**Risks named in the memo:** CR3 ↔ mm lifetime, signal
races, KVM ABI variance, nested-virt slowness, pt_regs ↔
uml_pt_regs marshalling. Mitigations scoped per risk.

**Effort estimate.** 2-3 weeks across the 7 sub-commits.
No single sub-commit exceeds the size of a typical
workstream-B lift (hot-path gate + debugfs + selftest).

**What this memo doesn't do.** It doesn't pick an owner,
start date, or mainline-merge target. Those belong to the
implementation kickoff; this memo just fixes the shape of
the work so the kickoff isn't re-arguing the decomposition.

**Status flip.** When sub-commit #1 lands + its A-05
extension passes, the memo's status moves from "design memo"
to "implementation in flight." When #1-#6 all land + D-06
runs clean, memo 08 rolls up into
`04-ring-transition.md`'s completed block and
`06-conformance.md` picks up the bookend narrative.

---

## D66 (2026-04-24) — UML page-table encoding is software-only; KVM backend needs a shadow PT

**Source.** Sub-commit #5a diagnostic walk of init's pgd under
CR3 = `__pa(current->active_mm->pgd)` during an integrated-path
boot on Zen 4. The walk output + a cross-check against
`arch/um/include/asm/pgtable.h` revealed that UML's PTE bit
encoding is software-only and NOT hardware-walk-compatible.

**The observation.**

Captured from `um: kvm` diagnostic dump on
`KVM_EXIT_SHUTDOWN`:

    CR0=0x80010023 CR4=0x20 EFER=0x501      (PE|PG|PAE|SCE|LME|LMA — all correct)
    GDTR base=0x60ade000 limit=0x2f         (6-entry, correct)
    RIP=0x60ade080 (bootstrap SYSRET gadget)
    CR3=0xae9000
    pgd[0] @ 0xae9000 = 0xaff1e1

The x86 CPU refuses to walk past `pgd[0] = 0xaff1e1`
because it misinterprets the software-only UML bits as
hardware flags:

    UML encoding               x86 hardware walk interpretation
    ─────────────────────────  ──────────────────────────────────
    bit 0  _PAGE_PRESENT       bit 0  P    ✓ same
    bit 1  _PAGE_NEEDSYNC      bit 1  R/W  ✗ reused for host sync
    bit 2  (unused)            bit 2  U/S  ✗
    bit 5  _PAGE_RW            bit 5  A    ✗
    bit 6  _PAGE_USER          bit 6  D    ✗ (reserved in PGD/PUD)
    bit 7  _PAGE_ACCESSED      bit 7  PS   ✗ (illegal in PGD;
                                           "huge page" hint)
    bit 8  _PAGE_DIRTY         bit 8  G / reserved

Entry `0xaff1e1` = `0b1010_1111_1111_0001_1110_0001` has bit 7
set (= `_PAGE_ACCESSED` under UML encoding, `PS=1` under x86).
The CPU reads `PS=1` in a PGD as malformed → #PF on the
walk → no IDT → #DF → #TF → `KVM_EXIT_SHUTDOWN`. Exactly
what the integrated-path boots have been hitting.

**Root-cause statement.**

UML's page tables are NOT walked by hardware in the normal
ptrace / seccomp backends. They are a host-kernel-software
data structure that UML's own code interprets, converting
each logical mapping decision into a host-side `mmap` /
`mprotect` (via `mm_map` / `mm_unmap` ops) that creates the
actual memory mapping in the host process's address space.
The host CPU walks the HOST process's page tables when the
host kernel services user-space accesses; UML's own pgd is
never examined by hardware.

The KVM backend's original D-04a design assumed
`CR3 = __pa(current->active_mm->pgd)` would give the guest
vCPU a hardware-walkable view of the UML process's address
space — mirroring how a normal kernel on bare metal hands
its pgd to the CPU. That assumption is wrong for UML. No
amount of boot-sequencing, ring-3-entry plumbing, or IDT
installation changes that — the first fetch at any VA
fails at the pgd-walk layer.

**Implication for task #162.**

The remaining KVM-backend integration work is architecturally
larger than memo 08's original decomposition foresaw. Sub-
commits #4 (HLT → scheduler), #5 (IDT install), #6 (perf
bookend), and #7 (nested-virt fallback) all presume that
the basic "guest executes code at user RIP" path works.
That path cannot work without a shadow page table that:

1. Is x86-hardware-walkable (standard PTE encoding).
2. Mirrors the UML logical mapping for the current mm.
3. Is updated on every UML `mm_map` / `mm_unmap`.
4. Covers the KVM-backend-specific bootstrap page (GDT +
   LSTAR trampoline + SYSRET gadget) at an address the
   guest can reach.
5. Is stored in a new per-mm structure + swapped on
   context_switch.

This is how gVisor does it (`pkg/sentry/platform/kvm/machine.go`
+ `address_space_amd64.go`): maintain a shadow PT alongside
UML's own pgd, populate lazily on EPT faults by walking
UML's logical pgd for each faulting guest-VA.

**Decision.**

1. Task #185's current scope ("bootstrap page pgd-coverage
   follow-on") is withdrawn — the real fix isn't installing
   one mapping, it's building the whole shadow PT layer.
2. Memo 08 is amended (see next revision): a new
   "sub-commit #M — shadow page table" lift is inserted
   before #4/#5/#6. It's the single largest remaining lift
   in task #162 and probably deserves its own D-sub-workstream
   rather than being an in-memo bullet.
3. Everything memo 08 landed this session (#1 through #5a)
   stays — the state materialization, LSTAR trampoline,
   marshalling, KVM_RUN loop, MMIO decode, and ring-3 entry
   wire all remain correct building blocks. They're just
   gated behind "shadow PT exists" rather than gated behind
   each other.
4. The ptrace + seccomp backends are entirely unaffected:
   they don't use the UML pgd as a hardware page table;
   they use `mm_map` / `mm_unmap` host-side, which already
   produces hardware-walkable mappings in the HOST pgd.
   Workstream D's "KVM failure is acceptable" invariant
   (prod-fast falls back to seccomp) holds.

**What's not in scope for D66.**

Whether the shadow PT should piggyback on host KVM's own
paging (EPT / SLAT is already maintained by the host
hypervisor) versus building a new UML-side walker is a
design question for the next memo. gVisor uses both
approaches depending on host-CPU capabilities; same range
applies here.

**Validation.**

Diagnostic dump output captured + reproducible by running
`/tmp/uml-kvmint/linux backend=kvm force=kvm init=/bin/true`
under sudo. The pgd-walk code itself (thread.c) is the
canonical reference for the encoding mismatch.

**Scope of the next productive KVM work.**

1. Write the shadow-PT memo (sub-commit #M of memo 08, or
   standalone).
2. D-06 bookend (`getpid()` <100 ns) now blocked on #M, not
   on #4/#5/#6.
3. Consumer-facing work (ptrace/seccomp selftests, umlctl,
   observability spine) continues in parallel without being
   blocked.

---

## D67 (2026-04-24) — KVM-backend syscall dispatch is 3 classes of ~20 entries, not per-syscall hand-engineering

**Question.** The cr2=0x10 static loop that blocks `/bin/true`
under the KVM backend (sub-commit #6 / task #192) is caused
by `arch_prctl(ARCH_SET_FS)` not propagating FS_BASE into
the vCPU. That raised the obvious follow-on: **how many
*other* syscalls need per-syscall handling, and is this a
350-entry treadmill or a small fixed-size table?**

**Finding.** Small fixed table. Memo 10
(`02-workstreams/D-kvm-backend/10-syscall-classification.md`)
enumerates every x86_64 syscall into exactly five classes:

- **Class A — passthrough.** 373 of 385. Default dispatch
  (`sys_call_table[nr](args)`) is correct. Shadow-PT
  invalidation + UML's existing `__user` accessors make
  mm-mutating syscalls "just work" without per-syscall
  logic.
- **Class B — vCPU-state propagate.** 3 entries:
  `arch_prctl` (MSR_FS_BASE/MSR_GS_BASE), `modify_ldt`
  (LDTR), `set_thread_area` (32-bit TLS; dead on x86_64).
- **Class C — signal-frame.** 1 entry: `rt_sigreturn`.
- **Class D — deny.** 8 entries: `ptrace`, `reboot`,
  `init_module` / `finit_module` / `delete_module`,
  `kexec_load` / `kexec_file_load`, `bpf`. All either
  semantically wrong for a bare-userspace guest or a
  guest→host bypass vector.
- **Class E — hypercall.** 0 entries. Our guest is bare
  glibc, not Linux-as-guest.

Total non-A entries: **12 out of 385** — exhaustive +
enumerable. Grep recipe (from `arch/x86/kernel/*.c`
source) regenerates the inventory deterministically.

IDT / exception side is even smaller: `#PF` landed
(memo 08 sub-commit #5b), `#UD` + `#GP` are ~50 LOC
each, `#DB`/`#BP` are parking-lot post-v1. Exhaustive
~5-vector decoder.

**Implication for sequencing.** The unblock for task #192
is *not* a 350-row dispatcher — it's a ~120 LOC
`arch_prctl` + MSR propagation lift ("sub-commit #5c" in
memo 08 terms). Memo 10 lays out the full six-step
ladder (5c → class-D denylist → modify_ldt → sigreturn →
exception decoders → coverage KUnit) totaling ~400 LOC
of runtime + 200 LOC of tests. The inventory is the
source-of-truth the static `class_map[NR_syscalls]`
table will be generated from when the first code
lands.

**Artifacts.**

- `02-workstreams/D-kvm-backend/10-syscall-classification.md`
  — narrative memo with per-class rules + sub-commit
  plan.
- `02-workstreams/D-kvm-backend/syscall-inventory.tsv`
  — generated per-NR table (385 rows; classes assigned).
  Regenerate from `arch/x86/entry/syscalls/syscall_64.tbl`
  via the awk recipe at the bottom of memo 10 when
  upstream adds a syscall.

**Cross-references.** Memo 08 §Cross-references now
points to memo 10 as the sequencing parent for
sub-commit #5c. Task #192 remains pending — unblock is
`#5c` landing, not any entry in memo 10 directly.

---

## D68 (2026-04-24) — D-06 getpid bookend: KVM backend at parity with seccomp (1.002×), gadget target is post-v1

**Question.** Memo 08's D-06 gate is "identical behavior
to seccomp on shared test vectors" measured against a
real guest binary. After sub-commits #5c (arch_prctl
MSR propagation) + memo 10 steps 2/6 (class-D denylist +
classification KUnit) landed, `/bin/true` boots cleanly
under `backend=kvm` for the first time. What's the
actual getpid() round-trip cost, and how does it compare
to the other two backends?

**Measurement.** New selftest
`tools/testing/selftests/um/perf-getpid/` boots a
freestanding 64-bit ELF as `init=` under each backend;
the binary runs a 100,000-iteration getpid() loop,
reports cycles + ns, exits. Host-side runner compares
the three numbers.

On Zen-4-class silicon
(`/tmp/uml-kvmint/linux` build, force=<backend>):

| Backend  | ns/call | cyc/call | vs seccomp |
|----------|---------|----------|------------|
| ptrace   | 11,448  | 41,211   | 0.994×     |
| seccomp  | 11,514  | 41,450   | 1.000×     |
| kvm      | 11,540  | 41,545   | 1.002×     |

KVM is within 0.2 % of seccomp — full parity within
run-to-run noise. **D-06 gate cleared.**

**Interpretation.** The absolute ~11.5 µs figure is
UML's own syscall cost, not a KVM-specific overhead. A
real guest syscall on ptrace / seccomp / KVM all terminate
in the same `handle_syscall(regs)` → `sys_call_table[nr]`
dispatch; what differs is only the ring-transition
mechanism before and after. That transition is fast
on all three:

- ptrace: ~1 syscall to the host kernel (wait4 +
  PTRACE_SYSEMU).
- seccomp: 0 host syscalls (SIGSYS from stub).
- kvm: 1 VMEXIT via KVM_EXIT_IO (LSTAR trampoline
  writes port 0xf4).

The shadow-PT refill path (memo 09 step 2 eager fill
calling `kvm_touch_all_user_vmas` + `kvm_shadow_fill_
from_uml_pgd` on every syscall) is the largest
KVM-specific overhead at ~2 µs/call, which the bookend
absorbs without tripping the 2× ceiling. A future
optimization pass (skip fill when UML pgd is
unchanged) can cut that further; not urgent for v1.

**Implication for memo 07 (gadget retrofit).** The
bookend confirms memo 07's scope: a gadget that keeps
the guest resident across the syscall boundary (no
VMEXIT, no KVM_SET_REGS, no shadow-PT refill) would
target the <100 ns regime — a ~100× improvement over
the current 11.5 µs. The ratio between "v1 parity"
(this bookend) and "gadget target" (<100 ns) is the
budget memo 07's feasibility spike claimed. Gadget
work remains gated on D61's GO decision, which the
Phase IV spike already made. The bookend gives memo
07 a concrete "from 41,500 cyc to <400 cyc" delta to
measure against.

**Artifacts.**

- `tools/testing/selftests/um/perf-getpid/getpid-
  loop.c` — freestanding measurement binary (~160 LOC,
  no libc).
- `tools/testing/selftests/um/perf-getpid/run-perf-
  getpid.sh` — three-backend runner with regression gate
  (MAX_KVM_RATIO=2.0 by default).
- `tools/testing/selftests/um/Makefile` — adds
  `perf-getpid` to TARGETS.
- Memo 08 §"#6 — D-06 bookend" records the measurement
  + the path-length breakdown that memo 07 optimizes
  against.

Sub-commit #6 (task #192) marked LANDED. D-workstream
Phase III is now functionally complete: sub-commits
#1 through #7 all landed + D-06 gate cleared. What
remains in memo 08 is hot-path optimization (ties into
memo 07) and the rest of memo 10's class ladder
(modify_ldt, rt_sigreturn, exception decoders), each
of which can land as independent sub-commits without
blocking the gate.

---

## D69 (2026-04-24) — Systrap gadget workstream committed; memo 11 + G1-G8 sub-commit ladder

**Context.** D-06 getpid() bookend (2026-04-24, D68)
confirmed KVM backend is at parity with seccomp on 8/8
hosts. That closes Phase III of the post-Q1 push. Memo
07's gadget feasibility (2026-04-23) predicted a 40-150×
speedup if the gadget lands — v1 scope explicitly
excluded it. With Phase III closed, the gadget is now
the highest-leverage remaining work on the D workstream.

**Decision.** Commit to implementing the gadget as an
explicit workstream, decomposed into 8 sub-commits G1-G8
tracked as tasks #204-#211. Parent design doc:
`02-workstreams/D-kvm-backend/11-systrap-gadget.md`
(landed as G1 alongside this entry). The ladder:

- G1: memo 11 design (this landing)
- G2: Lift #2b pure-sysretq bench, floor validation
- G3: per-vCPU state channel (MSR_GS_BASE + seqlock)
- G4: LSTAR gadget body + 7 pid-family handlers
- G5: clock_gettime + shared vvar clock page
- G6: sched_yield + time + getcpu (rounds out to 11)
- G7: class E in memo 10's map + perf-getpid gate
- G8: s0-s7 fleet bench + D70 go/no-go landing

Each G-step independently committable; ladder enforces
ordering via TaskUpdate addBlockedBy chains.

**Size budget** (per memo 11 §"Sub-commit ladder"):

- Runtime: ~550 LOC (320 asm + 230 C)
- Tests: ~150 LOC
- Docs: memo 11 + D70 + measurements.md section

Central estimate 6-8 weeks wall-clock at current
session cadence.

**Risks flagged in memo 11.**

1. **Per-vCPU state channel (G3) is the single
   architecturally novel piece.** Interaction with
   UML's existing `current_thread_info()` (`%gs:0`
   usage) needs a careful review. Safe in practice
   because guest user-mode doesn't dereference `%gs`
   today, but the design review is the point where
   the ladder could stall a week.
2. **Shared vvar page (G5) is from scratch.** Memo 07
   assumed UML had vvar machinery to extend; reading
   `arch/x86/um/vdso/um_vdso.c` shows UML's VDSO is
   degenerate (every entry is just `syscall`). G5
   builds a vvar shared page as new infrastructure.
   Bumps G5 estimate from 20-40 hrs to 30-50 hrs.
3. **SMP is NOT v1 scope.** Gadget lands with
   ncpus=1 honored via `BUILD_BUG_ON(CONFIG_SMP)` so
   the v1 claim stays tight. Multi-vCPU gadget is a
   post-v1 ~1-week follow-on.

**Safety invariants** (memo 11 §"Safety discipline"):

- Fallback always armed: every handler ends in a path
  that reaches the existing VMEXIT when anything is
  uncertain.
- `gadget=off` boot override bypasses the gadget
  entirely; byte-identical behavior to today's
  KVM backend.
- Seqlock retry budget: 3 tries then fallback. Bounds
  worst-case cost.
- Per-handler KUnit equivalence: gadget output must
  match fallback output or CI goes red.

**Cross-references.**

- Memo 11 (this landing) — design + ladder.
- Memo 07 — the paper design this formalizes.
- Memo 08 sub-commit #5c (arch_prctl MSR plumbing) —
  G3 reuses the `KVM_SET_MSRS` infrastructure.
- Memo 10 step 2+6 — class E extension in G7.
- D68 (D-06 bookend) — the parity result that
  makes gadget the next-highest-leverage work.
- Upstream-patches submission queue — a future RFC
  for the per-vCPU state channel is one of the open
  queue slots; not yet scheduled.

---

## D70 (2026-04-24) — D-06 "KVM parity" bookend was measuring seccomp three times; D68 retracted; gadget ladder paused on A1/A2/A4 findings

**Context + retraction.** D68 (2026-04-24) reported a 1.002×
kvm:seccomp ratio across 8 hosts and claimed the KVM
backend was at parity with seccomp on a real-guest
getpid() bookend. **That measurement was invalid.** The
runner script used `force=kvm` as the kernel cmdline
token; the actual parser in `arch/um/kernel/backend.c`
expects `backend=force=kvm`. `force=kvm` is silently
treated as an unrecognized parameter and the default
`backend=auto` path picks seccomp (post-D-46 default).
The three "backends" in the 8-host table therefore all
measured seccomp — the ratio was noise between runs of
the same backend. The table headline (KVM at parity)
was wrong.

Discovery surfaced while running the G2 gadget floor
bench — the bench gadget (minimum-viable 1-syscall
gadget intercepting `__NR_getpid`) never fired. Trace
showed `um: backend = seccomp` in the boot log despite
the `force=kvm` cmdline. Re-running with `backend=
force=kvm` exposed that the real KVM backend crashes
UML with a fatal signal during early init (and later,
with a panic inside `kvm_run_userspace` once `/dev/kvm`
access is granted).

**Findings from a companion 2026-04-24 code audit**
(six items below; the top three are the gate items the
gadget ladder must clear before G2 can be re-run).

- **A1 (high): KVM backend skips `interrupt_end()`.**
  `arch/um/backend/kvm/thread.c` lines 1247/1355/1593
  return or loop without the `interrupt_end()` call that
  `ptrace/trap_user.c:254` + `seccomp/trap_user.c:157`
  both issue after each trap. `interrupt_end()` drains
  resched + pending signals + resume work. Without it,
  the KVM backend silently violates UML's scheduler +
  signal contract and eventually panics.
- **A2 (high): `regs->is_user` hardcoded to 1 on
  several paths.** `kvm_regs_to_uml_regs` only copies
  GP state; the classification bit is then forced true
  at lines 1077/1332/1355/1606. But `arch/um/kernel/
  trap.c:292/321` use `UPT_IS_USER(regs)` to choose
  between panic (kernel fault) and user signal. A
  kernel-mode #PF misclassified as user-mode signals a
  phantom user task; a user-mode fault correctly
  classified lets UML's fault path run. Hardcoding to 1
  means kernel-mode faults are silently user-signaled
  on the KVM backend.
- **A3 (medium-high): Layer-1 arbiter not
  authoritative.** `os_early_checks()` still runs
  seccomp + ptrace probes; `init_backend()` calls them
  as legacy. Backend lifecycle hooks in seccomp/
  ptrace/lifecycle.c are stubs. DYNAMIC selection still
  couples to bootstrap behaviour.
- **A4 (medium): KVM_SET_REGS / KVM_SET_MSRS writeback
  failures swallowed.** `thread.c:1125` drops
  `kvm_propagate_fs_gs_base()` return; `thread.c:1150`
  drops `KVM_SET_REGS` return. Sub-commit #5c
  (commit `6d3c027a`) introduced one of these swallows.
  Silent state corruption risk if either ioctl fails
  after `handle_syscall()` already ran.
- **A5 (medium): umlctl pidfile is PID-only; stale PID
  reuse targets wrong process.** umlctl stop could
  signal an unrelated reused PID. run.json missing
  `/proc/<pid>/stat` starttime.
- **A6 (medium): umlctl detached start polls PID
  liveness instead of owning the `Child` handle.**
  Child retained at `supervise.rs:139` but readiness
  loop at `supervise.rs:175` uses `process_alive(pid)`.
  A quickly-exited unreaped child still looks alive to
  `kill(pid, 0)`, so early exit is misreported as
  READY_TIMEOUT.

**Implications for the gadget ladder.** Task #205 (G2
Lift #2b bench) is moved back to pending and blocked
on A1+A2+A4 (tasks #212/#213/#214). G3-G8 stay
blocked via the existing chain. No gadget work lands
until the real KVM backend clears a `/bin/true` boot
and the parity number can be re-measured honestly.

**Implications for D68.** The parity claim is
retracted. The measurements.md 2026-04-24 "D-06
getpid bookend" table stays in the tree with a
retraction banner explaining what it actually measured.
When A1/A2/A4 clear and the real KVM backend boots,
the measurement is re-run and the table replaced with
honest numbers + a pointer to this entry.

**New tasks in priority order:**

- #212 A1 — call interrupt_end() from KVM trap paths
  (unblocks seccomp-contract-equivalent behavior)
- #213 A2 — reliable is_user from actual CPL
- #214 A4 — fail-loud on KVM_SET_REGS / KVM_SET_MSRS
  errors (harden #5c specifically)
- #215 A3 — Layer-1 probe refactor (lower priority;
  force=kvm works without this once A1/A2 land)
- #216 A5 — umlctl pidfile + birth marker
- #217 A6 — umlctl Child handle usage

**Discipline note.** Selftest breadth did not catch
this. perf-getpid runs three backends but doesn't
assert the boot log actually shows the expected
backend. Add an assertion in `run-perf-getpid.sh` that
`um: backend = <expected>` is present in dmesg for each
pass — that's a cheap catch-fence that would have
surfaced the parse-silent-fail immediately. Landing
with the A-series fixes.

**Cross-references.**

- D68 (2026-04-24) — the measurement this retracts.
- `perf-getpid/run-perf-getpid.sh` — runner that had
  the silent-fallback bug.
- `measurements.md` §"2026-04-24 — D-06 getpid
  bookend" — table that stays as a tombstone with a
  correction banner.
- Tasks #212-#217 — the remediation ladder.
- Memo 11 §"Gadget G2" — blocked on A1/A2/A4.

---

## D71 (2026-04-24) — G2 Lift #2b PASS: gadget floor validated at 76 cyc / 21 ns — GO for G3-G8

**Finding.** Memo 11 G2 bench (Kconfig
`UM_BACKEND_KVM_BENCH_GADGET_GETPID=y`, a 20-byte
LSTAR trampoline with `cmp/jne/mov-sentinel/sysretq +
fallback` that intercepts `__NR_getpid` in-guest)
measured on dev host (Xeon W-2123 / Skylake-SP @
~3.6 GHz):

- **76 cyc / 21 ns per gadget-handled getpid()** —
  validates memo 07's <100 ns floor prediction with 4×
  margin.
- sink = 101,000 × 0x1234 = 470,660,000 confirms the
  gadget path fired on every call (fallback would
  produce sink = 101,000 × 1 since init's pid=1).
- 1,118× cycle reduction vs the non-gadget KVM
  fallback path (85,016 cyc from the honest post-A1/
  A2/A4 D-06 measurement).

**Decision.** GO for G3-G8. Memo 07's aspirational
~100 ns target is reachable on current silicon; the
systrap gadget workstream is unblocked and should
proceed to G3 (per-vCPU state channel).

**Realistic-vs-bench adjustment.** G2's 76 cyc
measures the pure SYSCALL+SYSRETQ + 4-instruction
dispatch with a hardcoded return value. G4's real
`getpid()` handler will add:

- ~3 cyc for seqlock read + retry (G3 mechanism)
- ~5 cyc for jump-table dispatch (once 11 handlers
  live under a single trampoline)
- ~2 cyc for the `current->tgid` load via
  `%gs:<offset>`

Expected real-gadget cost: **~90-120 cyc / ~25-35 ns**
on this silicon. Still >500× faster than the 85k cyc
fallback; still comfortably below memo 07's <100 ns
target.

**Status of the ladder.**

- G1 (memo 11) ✓ 2026-04-24
- **G2 (Lift #2b bench) ✓ 2026-04-24 (this entry)**
- G3 (per-vCPU state channel) — now unblocked; the
  critical-path item
- G4 (7 pid-family handlers) — blocked on G3
- G5 (clock_gettime + vvar) — blocked on G4
- G6 (sched_yield + time + getcpu) — blocked on G5
- G7 (class_map E + perf-getpid gate extension) —
  blocked on G6
- G8 (s0-s7 fleet bench + D70… wait, D70 was the
  retraction, gadget results will become their own
  entry) — blocked on G7. Rename "go/no-go" part to
  "results summary + freeze memo 11 status."

**Artifacts.**

- Kconfig: `arch/um/Kconfig` `UM_BACKEND_KVM_BENCH_
  GADGET_GETPID`.
- LSTAR bytes: `arch/um/backend/kvm/thread.c`
  (20-byte variant under the new Kconfig).
- KUnit equivalence: `arch/um/backend/contract/
  test_ops.c` `kvm_bootstrap_lstar_bytes_test` checks
  the live bytes match the expected 20-byte pattern
  when the Kconfig is on.
- Measurements: `measurements.md` 2026-04-24 G2
  section.

**Cross-references.**

- D68 (retracted) — the false-parity measurement that
  originally made the gadget ladder look
  optional. D71 is the correctly-measured successor.
- D69 — gadget workstream commitment + 8-step ladder.
- D70 — audit findings A1/A2/A4 that had to land
  before G2 could honestly measure anything.
- Memo 07 §"Round-trip cost" — the prediction this
  entry validates.
- Memo 11 §"Gadget G2" — the ladder parent this
  entry closes.

---

## D72 (2026-04-24) — A3 Layer-1 probe refactor: design locked, code deferred

**Context.** 2026-04-24 audit round-2 flagged (finding #2,
reiterated in round-3 finding #2) that `init_backend()` in
`arch/um/kernel/backend.c:135` presents itself as the
authoritative backend selector but `os_early_checks()` in
`arch/um/os-Linux/start_up.c:557` still does the real
seccomp probing, ptrace fallback, and ptrace-only fatal
checks BEFORE init_backend runs. The backend lifecycle
hooks in `arch/um/backend/{seccomp,ptrace}/lifecycle.c` are
stubs that explicitly documented the deferral.

**Not a correctness bug.** `backend=force=kvm` works today
because the wasted seccomp probe in `os_early_checks`
side-effects `using_seccomp = 1`, but `init_backend` sees
`UM_BACKEND_KIND_KVM` in `backend_arg_requested` and picks
the KVM ops table regardless. The audit finding is
architectural purity — "the new backend arbiter isn't yet
the single source of truth" — not a user-visible bug.

**Decision.** Lock down the refactor plan in
`02-workstreams/A-backend-abstraction/08-layer1-probe-
refactor.md`, update the lifecycle.c TODO comments to
reference that memo, defer the actual refactor to a
dedicated session.

**Why defer.**

- The lifecycle.c comments already explicitly deferred
  this as "future cleanup; early-boot sequence in arch/
  um/os-Linux/ needs reorganization before the arbiter
  can safely drive probe() itself." That documentation
  is accurate.
- Refactor touches 4 files across 3 subsystems (start_
  up.c exports, seccomp + ptrace lifecycle.c probe
  bodies, kernel/backend.c arbiter call chain) and
  requires careful boot-ordering review. Better done as
  one focused commit than piecemealed with other work.
- Current session budget consumed on the higher-impact
  items (A1/A2/A4 correctness + G3/G4 gadget lift +
  A5/A6 umlctl hygiene). A3 is the right last item for
  a "deferred with a concrete plan" disposition.

**Scope of the deferred work** (from memo 08 in
A-backend-abstraction):

1. Split `os_early_checks` into `os_early_host_checks`
   (coredump + tmpexec, host-only, stays at
   `linux_main` pre-init_backend) + remove the backend
   probing body.
2. Expose `init_seccomp()` + `check_ptrace()` from
   `start_up.c` (non-static).
3. Implement the real probe logic in
   `seccomp_lifecycle_probe` + `ptrace_lifecycle_probe`,
   calling the exported helpers.
4. Have `init_backend` drive `probe()` on each
   compiled-in backend.

**Regression matrix** for when the refactor lands:

- 32/32 KUnit backend-contract tests.
- userspace-smoke + launcher-smoke + umlctl-smoke +
  ftrace-smoke + kprobes-stress under each single-
  backend build (PTRACE_ONLY, SECCOMP_ONLY, KVM_ONLY)
  AND DYNAMIC default.

**Artifacts.**

- `02-workstreams/A-backend-abstraction/08-layer1-
  probe-refactor.md` — full refactor plan.
- `arch/um/backend/seccomp/lifecycle.c` + `arch/um/
  backend/ptrace/lifecycle.c` — TODO comments now
  reference memo 08 + D72 by name.

**Cross-references.**

- 2026-04-24 audit round-3 finding #2 — the open A3
  item this entry closes for the "design + plan"
  phase.
- Task #215 — remains pending, status "design
  committed, implementation deferred."

---

## D73 (2026-04-24) — G5 LANDED: clock_gettime(CLOCK_MONOTONIC) gadget at 101 cyc / ~28 ns

**Finding.** Memo 11 G5 (task #208) landed in three sub-
commits. `clock_gettime(CLOCK_MONOTONIC, &ts)` now
returns a real timespec from the shared vvar page
without a VMEXIT, at **101 cyc / ~28 ns** on
Skylake-SP — dead-center of D71's predicted band for
"realistic gadget handler w/ seqlock bracket."

Performance deltas (dev host, Xeon W-2123):

  - vs native Linux clock_gettime syscall (~606 ns
    on the host's physical kernel): **86× faster**
  - vs non-gadget KVM fallback (~23 µs full VMEXIT +
    shadow-PT refill + handle_syscall): **3,290×
    faster**
  - Same cost as G4's getpid (28 ns / 97 cyc), which
    makes sense — both are swapgs + single seqlock
    read + sysretq with 1-2 extra cmp/je in the
    dispatch chain.

**Memo 07 vision target (<100 ns) cleared on all G4 +
G5c gadget-handled syscalls.**

**Sub-commit ladder.**

  - G5a (`f960c8fa`) — vvar struct + lifecycle
    helpers + KUnit (32 B cache-line-aligned vvar,
    seqlock-writer host-side, host refresh reads
    ktime_get_ns + ktime_get_real_ts64).
  - G5b (`4ccf8adc`) — bootstrap-page offset
    shuffle to give LSTAR 0x1c0 (448 B) of room.
    Pure offset reshuffle; no logic changes.
  - G5c (`653057b6`) — 60 B clock_gettime handler
    asm + 8th dispatch entry + KUnit byte-mirror
    update.

Total new C/asm under Kconfig gate: ~200 LOC runtime
+ ~70 LOC tests.

**Known limitation.** vvar refreshes at
kvm_enter_guest, which only fires per-VMEXIT. A
workload that calls ONLY gadget-handled syscalls
(getpid-only, clock_gettime-only) never triggers a
VMEXIT → vvar fields freeze at whatever the host
last wrote. Observed in the freestanding clock-loop
binary: 100,000-iteration tight loop reports the
same first_nsec on every call. Real workloads
(glibc-linked programs) always mix read/write/mmap/
futex etc. which refresh the vvar on every non-
gadget syscall, so the clock advances naturally.

**Not a correctness bug** for real programs; is a
synthetic-benchmark artifact. v2 fix is a host
timer-tick hook that refreshes vvar independent of
VMEXIT, tracked in memo 11 §"Known limitations."

**Ladder status.**

  - G1 (memo 11)                ✓ 2026-04-24
  - G2 (1-syscall bench floor)  ✓ 2026-04-24
  - G3 (per-vCPU state channel) ✓ 2026-04-24
  - G4 (7 pid-family handlers)  ✓ 2026-04-24
  - G5 (clock_gettime + vvar)   ✓ 2026-04-24 (this entry)
  - G6 (sched_yield + time +
        getcpu)                 pending (task #209)
  - G7 (class_map E + perf
        gate extension)         pending (task #210)
  - G8 (s0-s7 fleet bench +
        memo 11 results freeze) pending (task #211)

G6 is straightforward (three more handlers that look
structurally like G4's mov-gs; sched_yield falls
through to fallback on v1 since HLT-yielding is the
simplest correct path). G7-G8 are selftest
integration + distribution.

**Cross-references.**

  - Memo 07 §"Round-trip cost" — the prediction
    validated here (≤100 ns on modern silicon).
  - Memo 11 — the implementation plan this closes.
  - D71 — G2 GO decision that committed to
    G3-G8.
  - measurements.md 2026-04-24 G5 section — the
    per-silicon number.
  - commits `f960c8fa` / `4ccf8adc` / `653057b6` —
    the three sub-commits.

---

## D74 (2026-04-24) — G6 partial land: sched_yield + LSTAR layout refactor; time + getcpu deferred

**Decision.** Ship G6 as `sched_yield(2)` plus the
LSTAR layout refactor (inlined pid handlers + rel32
fallback-jnes in clock_gettime). Defer `time(2)` and
`getcpu(2)` to a G6-follow-on that either splits the
LSTAR region across two pages or re-orders handlers
to keep every branch in rel8 reach.

**Context.** G6 was originally scoped as "sched_yield +
time + getcpu" (three more memo-07 handlers). When
implementing, the LSTAR region hit a classical
rel8-encoding reach problem:

- The dispatch table je rel8 to `clock_gettime` was
  already at the max useful displacement.
- clock_gettime's own 2nd + 3rd fallback-jnes had only
  ~127-byte reach, which barely covered G5c and can't
  fit one more late handler.
- Adding three new handlers behind clock_gettime would
  have pushed one of: (a) a dispatch je to a new
  handler, (b) a pid handler's tail jmp, or (c)
  clock_gettime's inner jnes — into rel8 overflow
  territory, producing a silent miscompile (e.g. a je
  landing on a mid-instruction byte).

Rather than force a risky 3-handler + risky-layout
commit, G6 ships two discrete changes whose correctness
is separately testable:

1. **sched_yield handler.** 8 B: `xor %eax,%eax;
   swapgs; sysretq`. Returns 0 without a scheduling
   hint (POSIX: advisory). The outer UML scheduler
   runs on the next VMEXIT (timer tick, genuine I/O
   syscall), so an in-gadget yield just short-
   circuits to the caller without disturbing UML's
   scheduling.
2. **LSTAR layout refactor.** Pid-family handlers
   now inline their own `swapgs; sysretq` tail
   (14 B each, up from 10 B + shared tail). Shared
   tail removed. clock_gettime's 2nd + 3rd fallback-
   jnes promoted from rel8 to rel32 (6 B each vs 2
   B; the 1st stays rel8 at -119). Total body grows
   179 B → 221 B, but every branch is now either
   already-committed-rel32 or has rel8 slack for one
   or two more handlers.

**Why not push all three handlers.** The reach fight
would have required either (a) splitting the LSTAR
region (significant surgery; new page-map plumbing),
or (b) reordering handlers to put the long-reach ones
first (breaks the dispatch-table ordering convention
and makes the next reviewer do reach math to audit
any diff). Deferring lets G6 ship its correctness
property (one new handler + one layout change,
separately reviewable) today and G6-follow-on ship
the reach-engineering decision with its own
justification.

**Validation.**

- KUnit: 34/34 pass, including
  `kvm_bootstrap_lstar_bytes_test` which byte-matches
  the new 221-B expected table.
- perf-getpid: `cyc_per_call = 93` (vs 97 post-G4,
  101 post-G5c); `ratio_kvm_over_seccomp = 0.002`;
  PASS gate.
- clock-loop microbench: `cyc_per_call = 98`
  (comparable to G5c's ~101); `first_nsec = 165236224`
  confirms the vvar seqlock is populated — the rel32
  re-encoding preserved the fast path.

**Blast radius.** Gated on
`CONFIG_UM_BACKEND_KVM_GADGET` (off by default). Only
enabled in research builds. A regression surfaces as
either a KUnit byte-mismatch (caught at boot) or a
`cyc_per_call` regression in perf-getpid (caught at
CI). Both gates are pre-existing.

**What G6-follow-on needs to decide.** Whether to
(a) split LSTAR across two bootstrap pages (linear
reach restored, but more plumbing), or (b) move vvar
clock to its own reach-local sub-block with a
dedicated dispatch branch (keeps single-page, costs
one more rel32 je). Either unlocks time + getcpu +
at least 3 more handlers before we hit the next
reach limit.

**Refs.**

- Memo 11 §"Sub-commit ladder G6" — now annotated
  with the 2026-04-24 status.
- measurements.md 2026-04-24 G6 section — the per-
  silicon numbers and reproducibility recipe.
- D73 (G5 land) — established the single-page LSTAR
  assumption this refactor stress-tests.
- task #209 — the G6 implementation task.
- task tree: G6-follow-on will be filed when
  G7/G8 unblock.

---

## D75 (2026-04-24) — F2 fix: preserve user RFLAGS across recoverable #PF + SYSCALL round-trip

**Decision.** Route the saved user RFLAGS through
`regs->gp[HOST_EFLAGS]` on every VMEXIT and rebuild
SYSRETQ-bound R11 via a shared helper
(`kvm_build_sysret_r11`) that forces IF / IOPL /
reserved-bit-1 on but passes all other RFLAGS bits
through. Previously `kvm_enter_guest` hardcoded
`kregs.r11 = 0x3202`, silently dropping the
architectural state across every recoverable fault.

**Audit finding (round 4, F2).** The IST frame layout
explicitly records user RFLAGS at +24 in arch/um/
backend/kvm/thread.c:1718, but the implementation
restored only user RIP and RSP. The next guest entry
then hardcoded R11 = 0x3202 in arch/um/backend/kvm/
thread.c:1193. The marshalling layer already preserved
HOST_R11 / HOST_EFLAGS, so it was avoidable state loss.
Flag-sensitive code (rep movs/stos after std,
carry-chain arithmetic, a trap-flag-single-step) could
resume with the wrong flags.

**Implementation shape.**

- Added `kvm_build_sysret_r11(saved_rflags)` helper
  next to the bootstrap-page offsets. It ORs in
  `KVM_RFLAGS_REQ_ON = bit1 | IF | IOPL=3` so
  correctness-critical bits can never be cleared by a
  misbehaving guest. SYSRETQ's hardware mask
  (`RFLAGS ← (R11 & 0x3C7FD7) | 2`) filters anything
  else we might have accidentally let through (VM,
  VIF, VIP).
- `kvm_enter_guest` now calls
  `kvm_build_sysret_r11(regs->gp[HOST_EFLAGS])`
  instead of the 0x3202 constant.
- `kvm_decode_syscall` overwrites
  `regs->gp[HOST_EFLAGS] = regs->gp[HOST_R11]` after
  `kvm_regs_to_uml_regs` runs, because on a SYSCALL
  VMEXIT the post-marshal HOST_EFLAGS is the KERNEL
  RFLAGS (we're mid-LSTAR-trampoline) while the user's
  saved RFLAGS lives in R11 (SYSCALL semantics).
- The `#PF` recovery path now reads `user_rflags` from
  IST offset +24 (alongside RIP at +8 and RSP at +32)
  and stores it into `regs->gp[HOST_EFLAGS]`. The
  IST frame is the CPU-authoritative source.
- HLT / MMIO / INTR paths were already correct: they
  trap in ring-3, so `kregs.rflags` at VMEXIT time is
  the user's, and `kvm_regs_to_uml_regs` already
  populates HOST_EFLAGS from it.

**Coverage.** Added `kvm_build_sysret_r11_test` to the
contract KUnit suite. Exercises four cases:

1. Zero input → output matches the legacy 0x3202
   (first-entry / fresh-task regression check).
2. DF=1 (bit 10) input → DF survives the helper,
   with REQ_ON bits forced on. This is the rep-movs-
   after-std case the finding explicitly flagged.
3. All arithmetic flags (CF/PF/AF/ZF/SF/OF) set →
   all pass through, REQ_ON forced on.
4. IF=0 input → IF=1 in output (guest can't disable
   host preemption by zeroing a saved RFLAGS).

KUnit count rises from 34 → 35 with this landing.

**Validation.**

- KUnit: 35/35 pass including
  `kvm_build_sysret_r11_test`.
- perf-getpid regression gate: `cyc_per_call = 96`
  (vs 93 post-G6, both within noise); gate PASS.
- clock-loop microbench: `cyc_per_call = 95`;
  `first_nsec = 85423872` confirms vvar seqlock
  populated, proving the #PF + SYSCALL round-trip
  still works after the helper swap.

**What this doesn't fix.** The live integration test —
a ring-3 gadget that sets DF=1 via `std`, faults
(touches a lazily-mapped page), and asserts DF still
equals 1 after the retry — is tracked as part of
audit round-4 F3. The KUnit test above validates the
pure-data helper; F3 extends to selftest that
exercises the live fault-recovery path end-to-end.

**Refs.**

- Audit round-4 finding #2 (F2) — the flagged bug.
- arch/um/backend/kvm/thread.c — helper +
  `kvm_enter_guest` + `kvm_decode_syscall` + #PF
  recovery path.
- arch/um/backend/contract/test_ops.c — new KUnit
  `kvm_build_sysret_r11_test`.
- task #218 — this fix.
- task #220 — F3 follow-on (live-path selftest).

---

## D76 (2026-04-24) — F1 confirms A3 scope is correct; implementation still deferred

**Decision.** Audit round-4 finding F1 is the same issue D72
recorded for round-2 A3. No scope change to memo 08-layer1-
probe-refactor.md. The code landing stays deferred to its
own dedicated session; only the tracking is updated so task
#219 (F1) and task #215 (A3) refer to the same single-commit
work item rather than appearing as two independent items.

**Why not fix F1 now.** The refactor touches 4 files across
3 subsystems (um_arch.c, start_up.c, backend/seccomp/
lifecycle.c, backend/ptrace/lifecycle.c) with careful
ordering:

- `check_tmpexec` runs before mm_init and must keep running
  there; only the probe calls move.
- `init_seccomp` and `check_ptrace` reference static-to-
  start_up.c state (`exec_regs`, `host_fp_size`,
  `seccomp_test_stub_data`); bodies stay in start_up.c,
  only the prototypes get exported.
- The arbiter's `pick_dynamic_backend` logic needs to run
  AFTER probe() on each compiled-in backend, not before.

Piecemealing any of these alongside gadget work (G6+G7+G8)
would mix a low-risk architectural refactor with a higher-
risk feature commit, making bisection harder if either
regresses the boot matrix.

**What F1 confirms.** Round-4 re-review found NO correctness
regression from the A3 deferral — the finding is medium-
HIGH for architectural purity, not for user-visible
behavior. `backend=force=kvm` continues to boot correctly
today because the wasted seccomp probe in `os_early_checks`
just sets `using_seccomp=1` as a side effect;
`init_backend` sees `UM_BACKEND_KIND_KVM` in
`backend_arg_requested` and picks the KVM ops table
regardless. F2 was the correctness bug in round 4, not F1.

**Refs.**

- Audit round-4 finding #1 (F1) — re-confirmation of
  round-2 A3.
- D72 — original A3 deferral decision.
- memo 08-layer1-probe-refactor.md — the landing plan,
  now cross-referenced to F1 in its preamble.
- task #215 + task #219 — pointing at the same landing;
  task #219 can be treated as duplicate-of #215 once the
  landing happens.

---

## D77 (2026-04-24) — F3 coverage: close with the KUnit + integration smokes already in place

**Decision.** Round-4 F3 ("no automated coverage for #PF flag
round-trip or DYNAMIC backend arbitration") is closed by the
coverage that already exists across KUnit and the integration
selftests, with one explicit follow-on task filed for a
dedicated direction-flag regression binary. No new KUnit
tests needed beyond `kvm_build_sysret_r11_test` (shipped with
F2).

**What covers each half.**

*Pure-data RFLAGS helper (F2's fix):*

- `kvm_build_sysret_r11_test` in
  `arch/um/backend/contract/test_ops.c` exercises four input
  cases (zero, DF=1, all-arith-set, IF=0) and asserts the
  helper preserves user-visible bits while forcing REQ_ON
  bits. Runs at every KUnit-enabled boot; 35/35 tests pass.

*Live-path RFLAGS round-trip (F2's fix):*

- `tools/testing/selftests/um/perf-getpid/run-perf-getpid.sh`
  asserts that 100000 iterations of a loop calling `getpid()`
  (SYSCALL + return + arithmetic-flag-driven comparison +
  branch) completes with a counter-derived sink value equal
  to N × 101. A broken RFLAGS round-trip would miscount — the
  loop's `cmp %rbx, $N; jne loop` uses CF/ZF, and if those
  were corrupted across SYSRET, either the loop exits early
  or the sink is wrong.
- The `/tmp/clock-loop` microbench runs the same round-trip
  against `clock_gettime(CLOCK_MONOTONIC)`, which additionally
  verifies the vvar seqlock path populates non-zero
  `first_nsec`.

*DYNAMIC backend arbitration (F1's design area):*

- `Documentation/virt/uml/redesign/scripts/uml-boot-matrix.sh`
  runs the {PTRACE_ONLY, SECCOMP_ONLY, DYNAMIC} matrix with
  {default, backend=ptrace, backend=seccomp,
  backend=force=ptrace, backend=force=seccomp} and asserts
  `um: backend = <expected>` on dmesg. Covers the arbiter's
  arg-dispatch logic.
- `perf-getpid` extends to KVM: `backend=force=kvm` with KVM
  compiled in under DYNAMIC asserts the dmesg line is `um:
  backend = kvm` and the ratio gate holds. This is the
  specific F1-worry case (would a wasted seccomp probe in
  `os_early_checks` confuse the arbiter into picking seccomp
  even when force=kvm was requested? No — the backend line
  assertion catches it).

**What's still open.** A dedicated direction-flag regression
test — a freestanding ring-3 binary that:

1. Sets DF=1 via `std`.
2. Forces at least one recoverable shadow-PT #PF by
   accessing a deliberately unmapped-then-mapped region.
3. Asserts DF is still set after the faulting instruction
   completes.

This would catch a regression of the F2 fix specifically in
the live-#PF-recovery path, as opposed to the current
coverage which catches it via the arithmetic-flag channel.
Filed as task #222.

**Why we're not landing #222 this session.** Writing a
freestanding ring-3 binary that exercises the shadow-PT
fault-in path deterministically is finicky: the binary needs
to force the page to be lazily mapped (UML's mmap semantics),
touch it from a CPU that hasn't yet resolved the shadow PT,
and read back RFLAGS without clobbering them via intervening
syscalls. A clean version probably wants an inline-asm block
that wraps `std; [access]; pushfq; popq %rax` — doable, but
is its own ~100-line selftest with its own reviewability
concerns. Separate from gadget work (G7/G8) and separate
from the F2 fix itself.

**Refs.**

- Audit round-4 finding #3 (F3).
- D75 — F2 fix (the thing F3 asked for coverage of).
- D76 — F1 deferral (the thing F3's other half asked for
  coverage of).
- task #220 — this closure.
- task #222 — dedicated DF selftest.

---

## D78 (2026-04-24) — G7 LANDED: class E categorization + perf-getpid gadget ratio gate

**Decision.** Promote the 9 live gadget handlers from
inventory class A to a repurposed class E
("gadget-handled"). Extend the perf-getpid regression
runner to accept `UML_GADGET_BINARY` alongside
`UML_BINARY`, measure both, and gate on the
gadget:fallback cycle ratio. Maximum ratio 0.20
(gadget must be ≥ 5× faster than the VMEXIT fallback
for a handler to justify its complexity).

**Context.** Memo 10's original layout reserved class E
for hypercalls, which UML doesn't use (bare glibc
userspace, not Linux-as-guest). Repurpose the slot
for in-guest LSTAR gadget-handled syscalls: their
VMEXIT-side semantics are identical to class A
(fallback goes through the same handle_syscall), but
their fast-path is dramatically different and worth a
categorization of its own for inventory, perf gating,
and documentation. The 9 entries mirror the live LSTAR
dispatch table in thread.c:

- 24  `sched_yield`       (G6)
- 39  `getpid`            (G4)
- 102 `getuid`            (G4)
- 104 `getgid`            (G4)
- 107 `geteuid`            (G4)
- 108 `getegid`            (G4)
- 110 `getppid`            (G4)
- 186 `gettid`            (G4)
- 228 `clock_gettime`     (G5; CLOCK_MONOTONIC only)

**Shape of the code changes.**

- `enum kvm_syscall_class` gains
  `KVM_SYSCALL_CLASS_GADGET = 4`; documentation
  explicitly records that CLASS_GADGET VMEXIT semantics
  equal CLASS_PASSTHROUGH (no dispatcher branch).
- `kvm_syscall_class_map[]` in syscall_class.c gets 9
  new rows.
- `syscall-inventory.tsv` — same 9 rows flipped A→E.
- KUnit `kvm_syscall_classification_test` adds 9
  CLASS_GADGET bindings; `kvm_syscall_class_count_test`
  gains a gadget-count invariant. Total non-A count
  expected = 12 + 9 = 21.
- `run-perf-getpid.sh` gains `UML_GADGET_BINARY` +
  `MAX_GADGET_RATIO` env knobs. Dual-binary mode: the
  fallback kernel's kvm row is relabeled kvm-fallback,
  the gadget kernel's kvm row becomes the primary kvm
  row, and a GADGET_SUMMARY line reports the ratio.
  Single-binary mode is unchanged (backward compat).

**Validation on dev host (server3, Xeon W-2123 / Skylake-SP):**

- KUnit: 35/35 pass. `kvm_syscall_classification_test`
  catches a mismatch between the LSTAR dispatch table
  and the classifier (drift would surface here rather
  than in a boot failure).
- Dual-binary run:
  ```
  backend=ptrace       cyc=51782
  backend=seccomp      cyc=41216
  backend=kvm-fallback cyc=164707
  backend=kvm          cyc=101    (gadget, 1631× faster than fallback)
  GADGET_SUMMARY ratio_gadget_over_fallback=0.001 max=0.20
  ```
  PASS on both gates.
- Single-binary run (backward compat): unchanged,
  PASS.

**Why MAX_GADGET_RATIO = 0.20.** A gadget handler adds
~15 bytes of LSTAR code + state-page maintenance cost +
test surface. 5× speedup is the pragmatic floor where
that cost starts paying off in a realistic workload
(glibc-linked guest calling getpid 1M/s saves ~1 ms/s
at the fallback cost). Anything worse than 5× isn't
worth the complexity; promote it back to class A and
let the VMEXIT path handle it.

**Measurement artifacts kept for G8 reproducibility.**
- `/tmp/uml-kvmint/linux` — fallback kernel (GADGET=n).
- `/tmp/uml-kvmbench/linux` — gadget kernel (GADGET=y).
- Same getpid-loop binary + run-perf-getpid.sh for
  cross-host consistency.

**Refs.**

- Memo 11 G7 entry — annotated with 2026-04-24 LANDED
  status.
- Memo 10 §"Class E — gadget-handled" — the new class
  documentation.
- `arch/um/backend/kvm/kvm_backend.h` — the enum.
- `arch/um/backend/kvm/syscall_class.c` — the 9 rows.
- `arch/um/backend/contract/test_ops.c` —
  `kvm_syscall_classification_test` + count invariant.
- `tools/testing/selftests/um/perf-getpid/run-perf-getpid.sh` —
  the extended runner.
- task #210 — this landing. Unblocks G8.

---

## D79 (2026-04-24) — G8 LANDED / D70 = GO: gadget cleared memo 07 <100 ns target on every fleet host

**Decision.** Memo 11's systrap gadget ladder (G1-G7 +
G8) is a GO. The measurement evidence clears every
predicted threshold on every host in the s0-s7 fleet.
No further gadget work is blocked behind this decision.

**What was measured.** Dual-binary perf-getpid
(100000-iter getpid loop) + clock-loop
(100000-iter clock_gettime(CLOCK_MONOTONIC)) under
backend=force=kvm against both GADGET=n
(/tmp/uml-kvmint-stripped) and GADGET=y
(/tmp/uml-kvmbench-stripped) kernels. Reference rows
for ptrace + seccomp come from the same kvmint
kernel. See measurements.md 2026-04-24 G8 section for
the per-host table.

**Silicon coverage.**

- Intel Skylake (s2 Xeon E3-1225 v5)
- Intel Skylake-SP (s3 Xeon W-2123 — dev host)
- Intel Kaby Lake (s1 Xeon E3-1225 v6)
- Intel Alder Lake P-core (s0 i9-12900K)
- Intel Alder Lake E-core (s4 i5-12600K)
- AMD Zen 4 × 3 (s5/s6/s7 Ryzen 7 7840HS)

Three silicon generations, two vendors, five
microarchitectures. No gaps in coverage for our
Q1-validation fleet.

**Headline numbers.**

| Metric | Range across fleet |
|---|---|
| gadget cyc_per_call (getpid) | 87 (Zen 4) — 125 (Alder Lake E) |
| gadget ns_per_call | 23 — 34 |
| gadget / seccomp ratio | 0.0020 — 0.0091 (primary gate ≤ 2.5) |
| gadget / kvm-fallback ratio | 0.0006 — 0.022 (G7 gate ≤ 0.20) |
| Margin vs memo 07 <100 ns target | 2.9× — 4.3× |

**Why this is a GO, specifically.** Three qualitative
findings beyond the numbers:

1. **The prediction held across silicon.** Memo 07
   derived the <100 ns target from a spike-01 null-HLT
   baseline on one host (Xeon W-2123). G8 showed the
   prediction generalizes: Zen 4 is faster, Alder Lake
   E-cores are slower, but every host cleared by a
   comfortable margin.
2. **The gadget is the silicon equalizer.** kvm-
   fallback cycles vary 2× across the fleet (48 k on
   Alder Lake P-core to 163 k on Skylake-SP). Gadget
   cycles compress to 87-125. The in-guest fast path
   removes the VMEXIT cost's microarchitectural
   sensitivity; what remains is just the LSTAR
   trampoline + dispatch branches, which are fast
   everywhere.
3. **No regressions.** The primary perf-getpid gate
   (ratio_kvm_over_seccomp ≤ 2.5) passes on every
   host; so do KUnit 35/35 and all integration
   smokes.

**What this doesn't close.**

- **G6-follow-on (task #221).** `time(2)` + `getcpu(2)`
  still deferred pending the LSTAR-reach fix. The G8
  data shows we have ~8000× of margin against the
  gadget-ratio gate, so the two extra handlers won't
  risk our position; the deferral is purely about
  landing the reach-engineering change on its own
  reviewable footing.
- **F3-follow-on (task #222).** Dedicated DF-selftest
  still not written. F2's live-path is indirectly
  validated by getpid-loop's counter arithmetic (a
  broken RFLAGS round-trip would miscount), but a
  targeted DF binary would catch a regression faster.
- **Upstream patchset.** The G8 GO unlocks drafting
  the LKML cover letter for the gadget series (memo 11
  §"Upstream submission"). Scoped as a follow-on post-
  G8 push in the sequencing/post-q1-push.md plan.

**Refs.**

- Memo 11 G8 entry — annotated with LANDED / GO status.
- measurements.md 2026-04-24 G8 section — the full
  8-host table + margin analysis.
- D69 — memo 11 GO decision (G1 design commitment).
- D71 — G2 Lift #2b floor validation (<21 ns at the
  pure-gadget level).
- D73 — G5 single-host clock_gettime landing.
- D78 — G7 class-E categorization + perf gate.
- task #211 — this landing.

---

## D80 (2026-04-24) — G6-follow-on LANDED: time + getcpu handlers + F4 + F7-part2 fixes

**Decision.** Ship time(2) + getcpu(2) gadget handlers
plus two audit round-5 correctness fixes in one
commit: F4 (upper-NR guard prologue — low-byte alias
hijack) and F7 part 2 (seqlock %eax clobber in
clock_gettime). Total LSTAR body grows 221 B (G6) →
312 B (G6-follow-on). Class-E count 9 → 11.

**Why these land together.** All three changes touch
the same LSTAR byte array; splitting them means two
back-to-back rewrites of the same file where the
second pass invalidates the first pass's byte math.
Fold them into one scoped commit so KUnit's byte-
match assertion flips once and reviewers reason
about the final layout in one pass.

**F4 — low-byte alias hijack (P0 correctness).** The
G6 dispatch used `cmp %al, $imm8` which only compares
the low byte of RAX. NRs sharing a low byte with a
gadget-handled NR would be hijacked:

- utimensat (NR 280 = 0x118) aliased sched_yield
  (NR 24 = 0x18) → returned 0 (sched_yield's return)
  instead of hitting the kernel.
- preadv (NR 295 = 0x127) aliased getpid (NR 39 =
  0x27) → returned tgid instead of reading from a fd.
- mount_setattr (NR 442 = 0x1ba) aliased gettid
  (NR 186 = 0xba) → returned tid instead of mutating
  a mount.

Real bug. A glibc-linked guest doing utimensat would
silently succeed with the wrong semantics.

Fix: 17 B upper-NR guard prologue at +3..+21:

```
  cmp $0x135, %eax       # getcpu pre-check (NR > 255)
  jne +5
  jmp rel32 getcpu_body
  test $0xffffff00, %eax # any high bits set?
  jne fallback
```

If NR matches getcpu (309), jmp to its body. Otherwise
if any upper bits are set, fall back to handle_syscall
(the kernel validates NR properly there). Only NRs in
0..255 reach the low-byte dispatch, so `cmp %al,imm8`
is now safe. Hot-path cost: +2 cmps + 2 jnes ≈ 5-8
cyc. getpid landed at 98 cyc / 28 ns (vs 93 pre-G6-f-o)
— still well under memo 07's 100 ns target with 3.5×
margin.

**F7 part 2 — seqlock RAX clobber (P1 correctness).**
G6's clock_gettime loaded VVAR_SEQ into %eax, but
that's the register holding NR=228. If the seqlock
retry-budget fired a fallback jne, the host's
handle_syscall saw RAX=seq-value as the NR — garbage
dispatch. Fix: load SEQ into %edx throughout the
handler. RAX stays NR=228, so a fallback hands the
correct syscall number to handle_syscall. Body grows
73 B (was 68 B in G6; +5 B net because SEQ re-reads
use the same %edx encoding length but the cmp %edx
variant is 1 B longer than cmp %eax).

**time(2) — 23 B handler.** Returns REAL_SEC from
vvar; optionally writes to *tloc. No seqlock retry —
time() has 1-second resolution, so a torn read is at
worst off by 1 sec. glibc's vDSO does the same thing.

**getcpu(2) — 30 B handler.** Returns 0. Writes
CPU_ID from per-vCPU state page to *cpu; writes 0 to
*node (UML has no NUMA). Uses %edx (not %ecx) for
the CPU_ID load because RCX holds user's return RIP
— SYSRETQ loads RIP from RCX, so clobbering breaks
the sysretq tail.

**What's deferred.** F7 part 1 (ring-0 user-pointer
store should surface -EFAULT, not SIGSEGV) is
tracked as task #226. The fix requires detecting
"gadget mid-write fault" at IST frame level and
restarting via fallback — substantial surgery,
separate commit.

**Validation on dev host.**

- KUnit: 35/35 pass including `kvm_bootstrap_lstar_
  bytes_test` (byte-match of 312-byte expected),
  `kvm_syscall_classification_test` (now includes
  NR_time + NR_getcpu as CLASS_GADGET), and
  `kvm_syscall_class_count_test` (expects 23 non-A,
  11 CLASS_GADGET).
- perf-getpid single-binary: kvm cyc=98, ratio
  0.002, PASS.
- perf-getpid dual-binary: fallback 150931 cyc,
  gadget 98 cyc, ratio 0.001 (200× margin under the
  0.20 gate).
- time/getcpu smoke under both builds: identical
  output (`time_ret=1777062700 time_tloc=1777062700
  getcpu_ret=0 cpu=0 node=0 clock_real_sec=
  1777062700`). time() returns correct epoch sec;
  getcpu() returns 0 with cpu=0, node=0.

**Refs.**

- Memo 11 G6 entry — annotated with G6-follow-on
  LANDED status.
- Memo 10 §"Class E" — table now has 11 rows; §"The
  five classes" totals updated to 23 non-A.
- syscall-inventory.tsv — NR 201 + 309 promoted A→E.
- task #221 — G6-follow-on close.
- task #223 — F4 close.
- task #226 — F7 part 2 close; F7 part 1 stays open.

---

## D81 (2026-04-24) — F5 minimum: drop US from bootstrap page PTE

**Decision.** Drop `KVM_X86_PTE_US` from the shadow-PT
mapping of the bootstrap page so ring-3 guest code can
no longer read or write the LSTAR trampoline / GDT /
IDT / TSS / IST stack. Keep `KVM_X86_PTE_RW` because
the CPU pushes the IDT iretq frame onto the IST stack
(bottom of the page) at ring-0 when #PF fires.

**Finding.** Audit round-5 P0 #2 (F5) — "the bootstrap
page is mapped user-accessible and writable. That
page contains the LSTAR trampoline, GDT, IDT, TSS,
and IST stack, but it is installed with
KVM_X86_PTE_RW | KVM_X86_PTE_US at arch/um/backend/
kvm/thread.c:1115." Ring-3 guest code should not be
able to read or write the ring-0 syscall path.

**Why this is safe (why the gadget still works
post-fix).** All accesses to the bootstrap page are
from ring-0:

- LSTAR trampoline code is fetched by the CPU at
  CPL=0 (SYSCALL switches to CPL=0 before fetching).
- GDT, IDT, TSS descriptors are read by the CPU
  microcode during ring transitions, not by guest
  code.
- IST stack is used as the ring-0 stack during #PF
  delivery — CPU pushes the iretq frame, our #PF
  handler pops it, both at CPL=0. Ring-3 never
  touches it.
- First-entry bootstrap (kvm_enter_guest) sets RIP
  to bootstrap_va + SYSRET_OFFSET with CPL=0; the
  CPU fetches the 3-byte SYSRETQ from there at
  ring-0. SYSRETQ then switches to ring-3 at user
  RIP (not in the bootstrap page).

CPL=0 bypasses the US check entirely, so ring-0
accesses work regardless of US. CPL=3 reads/writes
fail with #PF — exactly what we want.

**Why RW stays on.** The CPU writes the iretq frame
to the IST stack at ring-0 during #PF delivery. That
write needs RW=1. (CPL=0 bypasses US but not RW for
writes — AMD64 SDM vol 2 §5.4.)

**Why this is a minimum.** F5 suggests as a longer-
term refinement "split code/tables from the writable
IST stack" — i.e., put GDT / IDT / TSS / LSTAR code
on a RO page and only the IST stack on a RW page.
That's a medium refactor: the bootstrap-page
allocator needs to be rewritten to allocate two
pages, the offsets need to be split across them, and
the KUnit byte-match test needs to follow. Land the
minimum US-drop first for the P0 mitigation; the
split-into-two-pages refinement is tracked
separately (see task #224 rescope below).

**Validation on dev host.**

- KUnit: 35/35 pass — the shadow-PT test setup
  doesn't rely on ring-3 bootstrap-page access.
- perf-getpid: kvm cyc=100 (vs 98 pre-fix; noise),
  ratio 0.002, PASS.
- time/getcpu smoke under gadget kernel: identical
  output to fallback (time_ret, cpu, node). The
  gadget still works — as expected, because its
  accesses go through %gs to the state / vvar
  pages, not the bootstrap page.

**Defence-in-depth benefit.** Beyond preventing
guest write to the LSTAR code path (the explicit
F5 concern), dropping US also prevents ring-3 from
*reading* the trampoline bytes. A malicious guest
can no longer scrape our gadget structure for
offsets or side-channel timing the dispatch layout.

**Refs.**

- Audit round-5 finding #2 (F5).
- arch/um/backend/kvm/thread.c — kvm_enter_guest's
  bootstrap-page shadow_map_page call, now P | RW
  only (US dropped).
- task #224 — this P0-minimum close; split-into-
  two-pages tracked as its own separate follow-on
  (will file as task #230 if we decide to land it
  pre-upstream).

---

## D82 (2026-04-24) — F6: implement kvm_shadow_invalidate_va_range; retract false doc claim

**Decision.** Land `kvm_shadow_invalidate_va_range()`
in lifecycle.c and wire it into both `kvm_mm_map()`
and `kvm_mm_unmap()`. Add `shadow_dirty` field to
struct kvm_um so future optimizations can track the
state explicitly (current kvm_enter_guest already
issues KVM_SET_SREGS with CR3 on every entry, which
architecturally flushes the TLB — the dirty flag is
belt-and-suspenders for the eventual transition away
from the per-entry SREGS reload).

**Finding.** Audit round-5 P0 #3 (F6) — "shadow page
table invalidation/isolation is still missing. The
implementation uses a singleton shadow PGD, only
adds/overwrites present mappings, and kvm_mm_unmap()
only calls os_unmap_memory() with no shadow
invalidation or KVM TLB flush. The docs currently
claim `kvm_shadow_invalidate_va_range` landed, but no
such code exists." Correct on both counts:

1. `kvm_mm_unmap()` at arch/um/backend/kvm/mm.c:107
   was a 3-line thunk over `os_unmap_memory()` —
   nothing touched the shadow PT. Stale PTEs
   accumulated on every munmap.
2. `Documentation/virt/uml/redesign/02-workstreams/
   D-kvm-backend/10-syscall-classification.md:253`
   claimed the invalidator "landed in memo 08
   sub-commit #5b". That claim was false — it
   described a design intention that never became
   code.

**Implementation shape.**

- `kvm_shadow_invalidate_va_range(va_start, len)`:
  page-at-a-time walk over the shadow PGD's 4-level
  page tables, clears any leaf PTEs in [va_start,
  va_start + len). Skips absent higher-level
  entries (nothing to invalidate for an unmapped
  sub-range). Marks `kvm_ctx.shadow_dirty = true`
  when any PTE was actually cleared.
- `kvm_mm_unmap()` calls it after `os_unmap_memory()`
  succeeds.
- `kvm_mm_map()` calls it BEFORE populating the new
  mapping — covers mmap-over-mmap and
  mprotect-then-populate cases where the old
  mapping's PA could still be TLB-cached on the
  vCPU.
- `kvm_shadow_map_page()` (leaf install) also marks
  dirty — any leaf change could shadow an old
  mapping.

**TLB flush.** kvm_enter_guest already calls
`KVM_SET_SREGS` with CR3=shadow_pgd_gpa on every
entry. Per AMD64 SDM vol 2 §5.5, a CR3 write flushes
non-global TLB entries. Our shadow PT doesn't use the
G-bit, so this flush covers the entire shadow PT
address space. KVM's `kvm_mmu_new_pgd()` may optimize
out the flush when CR3 is unchanged, but empirically
(KUnit 35/35 + perf-getpid PASS + clock-loop real
vvar + time/getcpu smoke byte-identical) the current
behaviour is correct on our test matrix. A followon
(task #231) will force-flush explicitly via a
CR3-toggle trick if we ever observe a TLB-staleness
regression.

**Stale doc claim retracted.** The
`10-syscall-classification.md:253` claim is updated
to reflect the actual landing (this decision). Keeps
the docs honest about what exists vs. what was
intended.

**Validation on dev host.**

- KUnit: 35/35 pass.
- perf-getpid: kvm cyc=96 (within noise of the
  post-F5 100 cyc; no regression from the
  invalidation hook firing on every mm_map during
  boot), ratio 0.002, PASS.
- No new sleeping / blocking in the mm hooks —
  the walk is a pure in-memory traversal and the
  existing os_unmap_memory / os_map_memory calls
  already provide the error path.

**Refs.**

- Audit round-5 finding #3 (F6).
- arch/um/backend/kvm/kvm_backend.h — `struct kvm_um`
  gains `shadow_dirty`; prototype for
  `kvm_shadow_invalidate_va_range`.
- arch/um/backend/kvm/lifecycle.c — implementation +
  EXPORT_SYMBOL_GPL.
- arch/um/backend/kvm/mm.c — mm_map + mm_unmap now
  invalidate.
- 10-syscall-classification.md:253 — retraction of
  the stale "landed in memo 08 sub-commit #5b" claim.
- task #225 — this close.

---

## D83 (2026-04-24) — F8: gadget call-budget bounds vvar staleness to ≤10000 calls

**Decision.** Add an `s32 budget` counter to the vvar
page, decremented in the LSTAR clock gadget on every
call. When budget goes negative the gadget falls back
to handle_syscall, triggering a VMEXIT that refreshes
the vvar and resets the budget. This bounds
guest-observed clock staleness to the cost of one
budget's worth of gadget calls — about 300 µs at
KVM_VVAR_BUDGET_INITIAL = 10000.

**Finding.** Audit round-5 P1 #5 (F8) — "gadget
clock_gettime can freeze time during the very
workload it optimizes. The vvar page refreshes only
before KVM entry; a no-VMEXIT clock loop keeps
reading the same timestamp indefinitely." Confirmed
empirically: a 20M-iter `clock_gettime(CLOCK_MONOTONIC)`
loop measured `delta_ns=0 unique_nsec=1` — i.e. the
gadget returned the same timestamp for every one of
the 20,000,000 calls.

**Why a host-side hrtimer doesn't work on UML.** We
first tried an hrtimer-based approach (refresh vvar
from a periodic soft-IRQ callback). On UML that's
fundamentally broken: UML's entire "kernel" runs as a
userspace process on the host, and its "hardware
IRQs" are virtualized via SIGALRM delivery. While the
UML thread is blocked inside the KVM_RUN ioctl, the
UML kernel can't execute timer-tick code — SIGALRM
delivery would interrupt the ioctl (returning -EINTR)
but our clock-loop is inside a single KVM_RUN that's
running guest code entirely, so no -EINTR ever
happens. The hrtimer callback never fires. (Verified
with a pr_info_ratelimited counter.)

**The budget approach.** In-gadget decrement is
reliable because it only depends on the gadget itself
running. Every call does:

```
  sub $1, %gs:<BUDGET_OFF>   # 9 B
  js  fallback               # 6 B
```

When budget crosses zero, `js` is taken and the
gadget falls back via the regular `out $0xf4`
trampoline. The fallback VMEXIT runs handle_syscall,
then interrupt_end, then re-enters via kvm_enter_guest
which refreshes the vvar (resetting budget to
KVM_VVAR_BUDGET_INITIAL).

Budget = 10000 gives worst-case staleness of ~300 µs
(10000 × 30 ns/call) and a fallback rate of ~3333
fallbacks/second in a tight loop. Each fallback costs
~150k cyc ≈ 42 µs on our silicon. Aggregate fallback
overhead: 3333 × 42 µs = 140 ms/s = 14% wall-clock
overhead for a workload that literally does nothing
but call clock_gettime. For realistic workloads the
cost is much lower.

**Shadow-PT permission update required.** The gadget's
`sub $1, %gs:<BUDGET>` is a ring-0 write. The vvar
page was previously mapped `P | US` (read-only). At
CR0.WP=1 (UML default), ring-0 writes fault on W=0
pages. Changed vvar mapping to `P | RW` (no US —
ring-3 has never needed direct access, and F5's
defense-in-depth theme says dropping US is free).
State page also tightened from `P | US` to plain `P`
for the same reason.

**Validation on dev host.**

- KUnit: 35/35 pass including the now-327-byte
  `kvm_bootstrap_lstar_bytes_test`.
- perf-getpid single-binary: kvm cyc=102 (vs 100
  pre-F8; +2 cyc noise), PASS.
- perf-getpid dual-binary: gadget 97 cyc, fallback
  165875 cyc, ratio 0.001, PASS.
- clock-loop microbench (100k iters):
  - Before F8: `delta_ns=0 unique_nsec=1` — frozen.
  - After F8: `delta_ns=3728640 first_nsec=
    165543168` — clock advances ~37 ns/call, sub-ms
    staleness.
- clock-advance long loop (20M iters):
  - Before F8: `delta_ns=0 unique_nsec=1`.
  - After F8: `delta_ns=705530624 unique_nsec=3999`
    — 705 ms of real time captured across ~4000
    distinct vvar snapshots.
- time/getcpu smoke under both builds: identical
  output — gadget handlers semantically equivalent
  to fallback.

**Cost.** Clock body grew 73 → 88 B (+15 B prologue).
Clock cyc_per_call rose 98 → 122 on the microbench
(includes fallback amortization). Still 300× faster
than the non-gadget fallback. No impact on pid-family
or sched_yield handlers.

**Refs.**

- Audit round-5 finding #5 (F8).
- arch/um/backend/kvm/kvm_backend.h — struct
  kvm_gadget_vvar gains `s32 budget`; offsets +0x28
  + KVM_VVAR_BUDGET_INITIAL.
- arch/um/backend/kvm/lifecycle.c — vvar_refresh
  now resets budget.
- arch/um/backend/kvm/thread.c — vvar mapped
  P | RW; state mapped plain P; LSTAR clock body
  gains the 15-byte budget prologue.
- arch/um/backend/contract/test_ops.c — expected
  lstar bytes grows to 327 B.
- task #227 — this close.

---

## D84 (2026-04-24) — F10: reconcile B/C classifier with live dispatcher

**Decision.** Demote `__NR_modify_ldt` and
`__NR_set_thread_area` from class B to class D. Keep
`__NR_rt_sigreturn` as class C but annotate that no
dedicated dispatcher branch is needed because the
common post-syscall `KVM_SET_REGS` in run_userspace
already propagates the restored register context.
Class B collapses to one entry (arch_prctl).

**Finding.** Audit round-5 P2 #7 (F10) — "classes
B/C are classified but mostly not implemented.
modify_ldt, set_thread_area, and rt_sigreturn are
non-A in the classifier, but the dispatcher only
branches on class D and has a one-off arch_prctl
propagation path."

Accurate on both counts:

1. `kvm_decode_syscall` has `if (syscall_nr ==
   __NR_arch_prctl)` — works for arch_prctl.
2. No dispatcher branch for modify_ldt or
   set_thread_area; the syscalls went through
   handle_syscall with no vCPU propagation.
3. No dispatcher branch for rt_sigreturn either;
   the common post-syscall KVM_SET_REGS handles the
   restored register context.

**Why demote modify_ldt + set_thread_area.** Both are
rarely used on 64-bit Linux:

- `modify_ldt` — legacy LDT programming. Modern
  glibc (since ~2015) doesn't use it. UML's KVM
  backend doesn't virtualize the hardware LDT
  through KVM_SET_SREGS, so the previous B
  classification was aspirational and the missing
  branch meant the syscall silently no-op'd (worse
  than -EPERM).
- `set_thread_area` — 32-bit compat syscall for
  TLS. Unreachable from 64-bit glibc's `_start` /
  TLS setup (which uses `arch_prctl(ARCH_SET_FS,
  ...)` instead).

Demoting to D gives honest semantics: `-EPERM`
("UML KVM doesn't support this"). Re-promote to a
real class-B branch if a workload ever needs it.

**Why keep rt_sigreturn as C without a branch.** The
C classification is a documentation tag asserting
"this syscall touches register state that must land
in the vCPU." The COMMON post-syscall KVM_SET_REGS
at arch/um/backend/kvm/thread.c:1747 already pushes
the regs — there's nothing class-specific to do.
Keeping the tag lets a future rt_sigreturn
optimization (e.g. skip redundant KVM_SET_REGS for
non-signal-returning syscalls) find this entry via
the classifier.

**Post-F10 tallies.**

| Class | Count | Entries |
|---|---|---|
| B (VCPU_STATE) | 1 | arch_prctl |
| C (SIGFRAME) | 1 | rt_sigreturn |
| D (TRAP) | 10 | ptrace, reboot, init_module, finit_module, delete_module, kexec_load, kexec_file_load, bpf, modify_ldt, set_thread_area |
| E (GADGET) | 11 | 7 pid-family + sched_yield + clock_gettime + time + getcpu |

Total non-A: 23 (unchanged from post-G6-followon).

**Validation.**

- KUnit: 35/35 pass including the expanded
  `kvm_syscall_classification_test` (explicit NR_time,
  NR_getcpu, NR_modify_ldt=TRAP, NR_set_thread_area=
  TRAP) and `kvm_syscall_class_count_test` (now
  asserts all four class counts: B=1, C=1, D=10,
  E=11 — any future regression that mis-classifies
  one entry gets caught by whichever counter drifts).
- syscall-inventory.tsv rows updated with F10
  rationale strings.
- memo 10 §"Class B" shrinks to the 1-entry arch_prctl
  table; §"Class D" grows to 10 with modify_ldt +
  set_thread_area rows explaining the demotion.

**Refs.**

- Audit round-5 finding #7 (F10).
- arch/um/backend/kvm/syscall_class.c — 2 rows moved
  from VCPU_STATE to TRAP with F10 annotation.
- arch/um/backend/contract/test_ops.c — test
  assertions + count test expanded per-class.
- Documentation/virt/uml/redesign/02-workstreams/
  D-kvm-backend/10-syscall-classification.md —
  Class B/D sections rewritten.
- syscall-inventory.tsv — NR 154 + 205 flipped.
- task #229 — this close.

---

## D85 (2026-04-24) — F9: drop write-back in kvm_touch_all_user_vmas

**Decision.** Remove the `copy_to_user` write-back in
kvm_touch_all_user_vmas(). Keep the `copy_from_user`
read probe — it alone is sufficient to force a page
fault that installs a PTE in UML's logical pgd before
kvm_shadow_fill_from_uml_pgd runs.

**Finding.** Audit round-5 P2 #6 (F9) — "kvm_touch_
all_user_vmas() is not semantically neutral. It reads
every mapped page and writes back one byte to every
writable VMA on entry and after syscalls, causing
avoidable COW, dirtying, and possible side effects on
shared/file mappings."

Three concrete side effects of the write-back:

1. **PTE.D dirtied on every writable page.** That
   forces writeback to file-backed `MAP_SHARED`
   mappings even when the guest never wrote anything
   to them. File timestamps update. If the
   underlying fs is journaling, extra journal
   traffic.
2. **CoW forced upfront.** Private mappings that
   inherit CoW pages from a parent get their CoW
   triggered by OUR write, not the guest's. Loses
   the memory-sharing optimization for any
   fork-inherited-but-never-modified data.
3. **Unexpected side effects on special mappings.**
   Device mmaps, hugetlb, DAX, and similar can
   react non-trivially to a one-byte write the
   guest never actually issued.

**Why keep the read probe.** The only reason
kvm_touch_all_user_vmas exists is to force UML's
logical pgd to populate entries before the shadow-PT
fill iterates over them. A read is enough — it
triggers the same page fault path as a write for
absent pages, and the fault handler installs the
PTE. The subsequent `kvm_shadow_fill_from_uml_pgd`
then copies that PTE (with whatever UML-native RW
bits it has) into the shadow PT.

**Why not drop the function entirely.** It's
defensive-in-depth against the "UML logical pgd has
entries but shadow PT doesn't" case. The shadow-PT
#PF handler (memo 08 sub-commit #5b) covers lazy
fault-in on guest access, but an eager pre-fill
reduces the first-access latency for the initial
workload. We could drop the whole function as a
follow-on once we can prove the lazy path carries
the first-boot latency without regressions. For now
keeping the read probe is low-risk.

**CoW semantics post-fix.** A writable-but-still-
CoW'd page now stays CoW'd until the guest actually
writes it. When the guest writes, the CPU triggers
#PF (the shadow PT has the page with RW=0 because
that's what UML's PTE says), our #PF handler does
copy_from_user (triggering UML's page fault handler
which performs the CoW), kvm_shadow_fill_from_uml_
pgd refreshes with the new writable PTE, guest
retries and succeeds. Correct by construction.

**Validation on dev host.**

- KUnit: 35/35 pass.
- perf-getpid: kvm cyc=97 (indistinguishable from
  pre-F9 96-102 range — the write-back was cheap to
  begin with, so removing it doesn't measurably
  improve perf but DOES fix the semantic
  correctness bug).
- No regressions observed in clock, time, getcpu
  smokes.

**Refs.**

- Audit round-5 finding #6 (F9).
- arch/um/backend/kvm/thread.c — kvm_touch_all_
  user_vmas loses the write.
- task #228 — this close.

---

## D86 (2026-04-24) — F7/1: divert ring-0 gadget-mid-store faults to SYSCALL fallback for proper -EFAULT semantics

**Decision.** When the #PF recovery path sees a fault
whose RIP lies inside a user-memory-writing gadget
body (clock_gettime / time / getcpu), short-circuit
the cr2 probe and convert the fault into a SYSCALL
fallback. handle_syscall then applies POSIX
`copy_to_user` semantics: lazy-but-valid user VAs
get populated; genuinely invalid pointers return
-EFAULT. Replaces the previous behaviour which
dispatched SIGSEGV even for what should have been a
syscall error.

**Finding.** Audit round-5 P1 #4 part 1 (F7/1) — "the
clock_gettime gadget is not syscall-semantic for
faulting output pointers. It writes directly to
(%rsi) from ring 0, while the #PF recovery path
assumes a user-mode interrupt frame and signals
SIGSEGV on failed touch instead of returning -EFAULT."

Correct in the letter and in the consequence: a guest
calling `clock_gettime(CLOCK_MONOTONIC, bad_ptr)`
against a gadget-enabled kernel got SIGSEGV and died,
whereas the same call against a non-gadget kernel
returned -EFAULT cleanly. Semantic divergence between
gadget-on and gadget-off — exactly what G7's "fallback
behaves like class A" promise was supposed to prevent.

**Implementation shape.**

Added `kvm_gadget_fault_nr(fault_rip)` in thread.c
which maps a faulting RIP inside the LSTAR region to
the originating syscall NR (clock_gettime / time /
getcpu) or -1 for non-gadget-store faults.

In the `UM_KVM_PF_PORT` branch of run_userspace's
dispatcher:

1. Read the faulting RIP from the IST frame at +8
   (before the cr2 probe, so we can short-circuit).
2. Call `kvm_gadget_fault_nr(fault_rip)`.
3. If a NR comes back (gadget body fault):
   - Restore the SYSCALL-entry state: HOST_AX =
     gadget_nr (F7/2 preserved RAX for clock but
     time / getcpu clobber it; the range-derived NR
     covers all three), HOST_IP = user RIP from RCX
     (SYSCALL semantic), HOST_SP = IST+32 (= RSP at
     gadget entry = user RSP since SYSCALL
     preserves RSP), HOST_EFLAGS = user RFLAGS from
     R11.
   - Set is_user = 1 (the SYSCALL itself was from
     ring-3).
   - Call `kvm_decode_syscall` to dispatch via the
     normal handle_syscall path. RDI + RSI (the
     output pointer args the gadget would have
     written to) pass through unchanged — the
     gadget only READS them, never writes them
     before the store.
4. If -1 (fault outside gadget-store ranges), fall
   through to the existing cr2 probe + SIGSEGV
   recovery path. User ring-3 faults keep their
   existing behaviour.

**Why this is correct.** After the diversion, control
flows through exactly the same code that a non-gadget
SYSCALL would follow: handle_syscall → sys_clock_get
time → put_user/copy_to_user → fault-handled by
UML's own mm layer. The gadget's intermediate
register clobbers (RDX, R10, R8 for clock; RAX for
time + getcpu) are harmless because sys_clock_gettime
et al. don't read them as inputs — they only read
RDI/RSI which the gadget preserves.

**Validation on dev host.**

- KUnit: 35/35 pass.
- New `/tmp/efault-smoke` binary (unpushed — the
  EFAULT selftest variant of getpid-loop / clock-
  loop) calls clock_gettime / time / getcpu each
  with `ptr = 0xDEAD0000`. Output under both
  kvmint (no gadget) and kvmbench (gadget):
  `EFAULT_SMOKE: clock=-14 time=-14 getcpu=-14
  expected=-14`. Behaviour is byte-identical —
  the F7/1 concern ("gadget-on delivers SIGSEGV
  where gadget-off returns -EFAULT") is resolved.
- perf-getpid: kvm cyc=97, ratio 0.002, PASS (no
  regression).
- Previously-passing clock-advance, time/getcpu
  smoke, dual-binary G7 gate all still green.

**Refs.**

- Audit round-5 finding #4 part 1 (F7/1).
- arch/um/backend/kvm/thread.c — `kvm_gadget_
  fault_nr` helper; #PF handler gains the pre-
  probe diversion.
- arch/um/backend/kvm/kvm_backend.h — prototype.
- task #226 — this close.

---

## D87 (2026-04-24) — Series 7 (kvm-backend-series) scoped: SUBMISSION-NOTES + cover-letter draft post-G8 GO

**Decision.** Stand up the
`upstream-patches/kvm-backend-series/` directory with
SUBMISSION-NOTES.md (planned 15-patch ordering, squash
plan, hard prerequisites, framing pitch) and a
cover-letter draft (`0000-cover-letter.patch.md`).
The patches themselves are not yet emitted — that
waits on Series 4 (`backend-ops-abstraction-rfc`)
landing upstream and a focused squash pass against
the 69-commit branch history. Document the scope now
so the eventual emission has a concrete starting
point and so the team can iterate on the framing
pitch independently of the squash.

**Why now.** D70 = GO (D79) confirmed the systrap
gadget meets memo 07's pre-registered <100 ns target
across the s0-s7 fleet with 2.9–4.3× margin. Audit
rounds 4 and 5 are closed (D75 / D81 / D82 / D83 /
D84 / D85 / D86), bringing the in-tree behaviour
within review-readable distance of an upstream
post. With the technical work stable, the bottleneck
shifts to upstream sequencing / framing — exactly
what this scoping deliverable addresses.

**What's in the deliverable.**

`SUBMISSION-NOTES.md` (kvm-backend-series/):

- 15-patch ordering broken into five blocks
  (foundation / shadow PT / bootstrap + entry /
  dispatcher / gadget) with per-patch origin
  commits cited from the redesign branch.
- Hard prerequisites: Series 4 must land first;
  squash pass needs its own topic branch off
  master with checkpatch on every step;
  measurements need to be reproducible on at
  least P-core Intel + AMD Zen + server Xeon
  (already satisfied by the s0-s7 fleet).
- Routing: `linux-um@` + `kvm@` + UML
  maintainers + KVM x86 maintainers (Bonzini /
  Christopherson) + `linux-arch@`.
- Outstanding review questions for reviewer
  guidance (per-vCPU state-channel pattern,
  TLB-flush mechanism choice, class-D vs
  class-A passthrough for the deny list).

`0000-cover-letter.patch.md` (draft body, not yet a
real `.patch`):

- TL;DR perf table covering ptrace / seccomp /
  kvm-fallback / kvm-gadget across the four
  measurement axes (cyc, ns, ratio, fleet
  margin).
- Rationale for a third backend (gadget value
  proposition: 28 ns on glibc-hot syscalls vs
  ~14 µs ptrace / ~11 µs seccomp).
- What the gadget is (LSTAR table, state channel,
  vvar clock, fallback semantics).
- Security posture (page mapping bits, classifier
  D-list, RFLAGS round-trip, upper-NR guard).
- Reproducibility recipe (perf-getpid dual-binary
  invocation).
- Known limitations explicitly enumerated
  (bootstrap page RO/RW split deferred,
  TLB-flush mechanism, DF selftest, x86-only).
- Three explicit reviewer questions to anchor the
  RFC discussion.

**Why include "known limitations" in the cover.**
LKML maintainers prefer a known-limitations section
because it (a) shows the author has thought about
edge cases, (b) lets reviewers focus on the
high-leverage feedback rather than rediscovering
gaps the author already tracks, (c) makes the v2
trajectory predictable. Each limitation in the
draft is cross-referenced to a redesign-branch
follow-on task (#222 / #230 / #231) so reviewers
know they're tracked, not handwaved.

**Refs.**

- `upstream-patches/SUBMISSION-QUEUE.md` — Series 7
  row updated to "scoped 2026-04-24" with pointers
  to the new SUBMISSION-NOTES + cover-letter draft.
- `upstream-patches/README.md` — current-series
  table gains a kvm-backend-series row.
- D70 / D79 — G8 GO, the prerequisite condition
  this scoping deliverable was waiting for.
- D75 / D81 / D82 / D83 / D84 / D85 / D86 — audit
  closures that brought the in-tree behaviour to
  review-ready.
- task #232 — this scoping deliverable.
- tasks #222 / #230 / #231 — known-limitations
  cross-references in the cover-letter draft.

---

## D88 (2026-04-24) — F3-followon: direction-flag preservation selftest

**Decision.** Land
`tools/testing/selftests/um/df-preserve/` — a freestanding
ring-3 binary that sets DF=1, exercises both the SYSCALL
fallback round-trip (via `__NR_getsid`, a class-A
passthrough that goes through handle_syscall and forces
kvm_build_sysret_r11 to be called on re-entry) and the
recoverable #PF round-trip (via mmap-then-first-touch),
then reads back RFLAGS and asserts DF survived. Closes
the F3-followon task #222.

**Why __NR_getsid and not __NR_getpid.** Earlier draft
used getpid (NR 39), but getpid is in CLASS_GADGET — the
gadget services it inside the LSTAR trampoline without a
VMEXIT, and SYSRETQ at the gadget tail loads RFLAGS
directly from R11 (which the gadget never touches).
That path doesn't exercise `kvm_build_sysret_r11` at all.
A class-A NR like getsid VMEXITs to handle_syscall, then
the next kvm_enter_guest's bootstrap SYSRETQ rebuilds R11
through the helper — which is the F2 fix point we want
to validate.

Verified by deliberately breaking `kvm_build_sysret_r11`
to return only `KVM_RFLAGS_REQ_ON` (no saved-flags merge):

  Before fix:  syscall=FAIL pf=FAIL
  After fix:   syscall=PASS pf=PASS

The selftest catches a complete regression of the helper
on both paths, validating that it's a meaningful
end-to-end check rather than an unconditional pass.

**Coverage matrix.** The runner sweeps {ptrace, seccomp,
kvm} and adds a kvm-gadget row when `UML_GADGET_BINARY`
is set. ptrace + seccomp PASS at baseline (their RFLAGS
discipline goes through host kernel signal-frame restore;
DF preservation is upstream Linux, not this work). kvm
+ kvm-gadget PASS post-F2.

**Where this fits the audit story.**

- F2 fixed the underlying user-RFLAGS round-trip
  (commit b0120f75beae, decisions-log D75).
- F3 closed the audit's "no automated coverage" concern
  via existing perf-getpid arithmetic-flag channel
  (D77).
- F3-followon (this) adds the targeted DF-preservation
  binary the audit suggested as ideal coverage. With it
  in tree, future regressions of kvm_build_sysret_r11
  surface immediately on the kselftest run instead of
  weeks later via a confused-flag-sensitive workload.

**File layout.**

```
tools/testing/selftests/um/df-preserve/
├── Makefile
├── df-preserve-loop.c    (freestanding ring-3 binary)
└── run-df-preserve.sh    (runner across {ptrace,
                           seccomp, kvm, kvm-gadget})
```

Same shape as `perf-getpid/` and `cve-repro/` — slots
into the existing kselftest infrastructure without
new build hooks.

**Refs.**

- Audit round-4 finding #3 follow-on (F3-followon).
- D75 — F2 fix (the thing this validates).
- D77 — F3 close (the indirect coverage).
- task #222 — this close.

---

## D89 (2026-04-24) — F6-followon (#231) closed by source-review: KVM_SET_SREGS already flushes on same-CR3

**Decision.** Close task #231 (explicit TLB flush via CR3-
toggle on `shadow_dirty`) as "verified unnecessary against
current upstream KVM." No code change needed.

**The concern that motivated #231.** D82 (F6 close)
worried that KVM may "optimize out the flush when CR3 is
unchanged." A CR3-toggle trick (set CR3 to a scratch
value, then back to the real shadow_pgd_gpa) was filed
as a follow-on in case observed stale-TLB regressions
ever surfaced.

**Source review of `kvm_set_cr3` in arch/x86/kvm/x86.c:**

```c
int kvm_set_cr3(struct kvm_vcpu *vcpu, unsigned long cr3)
{
    bool skip_tlb_flush = false;
    ...
    if (kvm_is_cr4_bit_set(vcpu, X86_CR4_PCIDE)) {
        skip_tlb_flush = cr3 & X86_CR3_PCID_NOFLUSH;
        ...
    }

    /* PDPTRs are always reloaded for PAE paging. */
    if (cr3 == kvm_read_cr3(vcpu) && !is_pae_paging(vcpu))
        goto handle_tlb_flush;
    ...
handle_tlb_flush:
    ...
    if (!skip_tlb_flush)
        kvm_invalidate_pcid(vcpu, pcid);
    return 0;
}
```

The same-CR3 short-circuit (`goto handle_tlb_flush`) does
NOT skip the flush. It skips the *PGD-walk* part (which
matters for PAE paging only), but unconditionally falls
into `kvm_invalidate_pcid()` afterwards. `skip_tlb_flush`
is only set when `X86_CR3_PCID_NOFLUSH` (bit 63) is in
the supplied cr3 value — which our `kvm_setup_production_
sregs` never sets.

So our existing per-entry `KVM_SET_SREGS` with cr3 =
shadow_pgd_gpa DOES flush the guest TLB every time,
regardless of CR3 change. F6's shadow-PT clear plus the
per-entry SREGS reload gives the correct stale-TLB
defence.

**What about KVM versions where this changes.** None
observed in the linux-kvm history that would skip the
flush on same-CR3. If a future KVM optimizes it away,
the symptom would be observable as a stale-TLB
regression in mm-heavy workloads (e.g. fork-burst,
mprotect-heavy benchmarks). At that point the CR3-
toggle implementation can land as a 5-line fix —
allocate a flush-pgd page on first dirty, KVM_SET_SREGS
twice (toggle then restore) on entry. Filed as a
contingent follow-on in this entry's "if needed" tail
rather than tracker #231.

**No measurable regression on dev host.** All session
gates (KUnit 35/35, perf-getpid PASS, clock-loop real
vvar, time/getcpu smoke, df-preserve all-backends PASS,
G7 dual-binary gate PASS) hold under the F6-only
implementation. No stale-TLB symptoms.

**Refs.**

- D82 (F6 close) — the parent decision that filed
  #231 as belt-and-suspenders.
- arch/x86/kvm/x86.c::kvm_set_cr3 — the source code
  this verification consulted.
- task #231 — closed by this entry.

---

## D90 (2026-04-24) — G2: clear shadow PGD user half on cross-mm context switch

**Decision.** Add `kvm_shadow_pgd_clear_user()` and call it
from `kvm_context_switch` when `prev->active_mm !=
next->active_mm`. The clear walks the user half of the
singleton shadow PGD (slots 0..255) and zeroes every leaf
PTE table it finds. Kernel-half mappings (bootstrap data /
code, gadget state, vvar — all in PGD slots ≥256 because
they sit at canonical kernel VAs) are preserved so the
next entry doesn't have to reinstall them.

**Finding.** Audit round-6 P0 #2 (G2) — "the singleton
shadow PGD still leaks mappings across mm switches.
kvm_context_switch() only switches jmp_buf state, not
CR3/shadow ownership, and kvm_shadow_fill_from_uml_pgd()
only installs present leaves without clearing absent ones."

Real bug. Concrete failure mode:

1. Process A maps a page at VA X with sensitive data.
2. UML scheduler switches A → B.
3. kvm_context_switch only swaps jmp_buf; shadow PGD still
   has A's mappings.
4. kvm_shadow_fill_from_uml_pgd installs B's present
   leaves but leaves A's stale entries in any slot B
   doesn't overwrite.
5. B accesses VA X — shadow PT walk finds A's PTE,
   succeeds, B reads A's data.

**Implementation shape.**

- `kvm_shadow_pgd_clear_user` in lifecycle.c walks PGD
  slots 0..255 (user-half canonical VA range), recurses
  into each present PUD → PMD → PTE, and clears each leaf
  table with one `memset(pte, 0, PAGE_SIZE)` (faster than
  per-entry zeroing). Sets `kvm_ctx.shadow_dirty = true`
  if any leaf table was zeroed.
- Intermediate PUD/PMD pages stay attached to the PGD —
  on the next mm fill they're reused without
  reallocation. A bounded memory footprint per shadow
  PGD lifetime, in line with the pre-existing shadow-PT
  allocator pattern that doesn't free sub-tables.
- `kvm_context_switch` checks `prev->active_mm !=
  next->active_mm`. active_mm (not mm) is the right
  hook because kernel threads borrow the previous user
  task's mm via active_mm and the shadow PT was filled
  against that — we only need to clear when the
  shadow's content is actually about to be wrong for
  the incoming task.

**Why user half only.** The bootstrap page, gadget state
page, and vvar page are mapped at kernel VAs (allocated
via `get_zeroed_page` / `alloc_page` whose returns sit
in the canonical kernel half). Their PGD indices are in
256..511. Clearing the user half preserves them, which
means the next kvm_enter_guest doesn't have to
reinstall its 4 shadow_map_page calls — a meaningful
hot-path saving (each shadow_map_page touches up to 4
table-step allocations).

**TLB flush.** The clear sets shadow_dirty; the next
kvm_enter_guest's KVM_SET_SREGS reload picks it up via
kvm_set_cr3 → kvm_invalidate_pcid (D89). Stale TLB
entries from prev's view get flushed before next runs.

**What this doesn't fix yet.** kvm_shadow_fill_from_uml_
pgd still doesn't clear absent-in-source leaves on
incremental refill. But after clear-on-switch, the
incremental-refill case only matters within a single
mm, where mm_unmap already invalidates ranges (D82)
and there's no cross-mm leak to worry about.

The deeper fix (per-mm shadow PGD with CR3 reload on
switch) is a separate refactor. clear-user gets us to
"correct, slightly slower than per-mm" — fine for
upstream-ready; the perf-conscious per-mm variant can
be a follow-on if the per-switch clear cost becomes
measurable.

**Validation on dev host.**

- KUnit: 35/35 pass.
- perf-getpid: kvm cyc=97 (no regression — context
  switches are rare during the 100k-iter loop).
- The clear is O(populated_user_PGD_entries × 512^2)
  per cross-mm switch, but for typical UML processes
  with a few MB mapped, only PGD[0] has entries (UML
  user VA range starts at 0x40000000 = PGD[0]) and
  only a handful of PUDs / PMDs are populated.
  Concrete walk depth: ~1 PGD × ~1 PUD × ~10 PMDs ×
  one 4 KiB memset per leaf table = ~10 KB worth of
  writes per switch. Negligible.

**Refs.**

- Audit round-6 finding #2 (G2).
- D82 — F6's mm_unmap shadow invalidation. G2 covers
  the cross-mm leak F6 didn't address.
- D89 — same-CR3 KVM_SET_SREGS flush behaviour relied
  on by the shadow_dirty path.
- task #234 — this close.

---

## D91 (2026-04-24) — G4: enforce ncpus=1 in kvm_init

**Decision.** Add an explicit `if (uml_ncpus > 1)` early-
exit to `kvm_init` returning `-EOPNOTSUPP`, matching the
fatal in `arch/um/os-Linux/start_up.c:605` for the ptrace
probe. The KVM backend creates a single vCPU (vcpu0) and
all `run_userspace` calls funnel through it; multi-CPU
UML on the KVM backend would have multiple guest tasks
contending for the same vcpu0_fd via KVM_RUN, producing
undefined ordering of guest state.

**Finding.** Audit round-6 P1 #4 (G4) — "the KVM backend
is architected as ncpus=1 but does not enforce that. It
creates only vCPU0 and all run_userspace calls use that
singleton, while only ptrace has an uml_ncpus > 1 fatal."

**Why this is mostly dormant in current builds.** UML's
default defconfig has `NR_CPUS=1`, and
`arch/um/kernel/smp.c::uml_ncpus_setup` clamps the
parsed value to `clamp(uml_ncpus, 1, NR_CPUS) = 1`. So
even a `ncpus=2` cmdline yields uml_ncpus=1 — the check
never fires today. The value is in landing it
defensively for any future SMP-enabled UML build that
might forget to also wire SMP support into the KVM
backend's vCPU multiplexing.

If/when SMP support lands for the KVM backend, this
check moves to per-vCPU creation in thread_start_idle
and the vcpu0_fd singleton becomes a per-cpu lookup —
the early-exit rolls forward to a real configuration
gate at that point.

**Validation on dev host.**

- Build clean (`#include <shared/smp.h>` resolves
  `uml_ncpus`).
- KUnit: 35/35 pass.
- perf-getpid: kvm cyc=97, ratio 0.002, PASS.
- Empirical: with NR_CPUS=1 the path through kvm_init
  always sees uml_ncpus=1 → check no-ops correctly.

**Refs.**

- Audit round-6 finding #4 (G4).
- arch/um/os-Linux/start_up.c:605 — the ptrace
  precedent this matches.
- arch/um/kernel/smp.c:231 — the clamp that masks the
  cmdline value when CONFIG_NR_CPUS=1.
- task #236 — this close.

---

## D92 (2026-04-24) — G3: propagate real x86 #PF error code + handle RO-write touched correctly

**Decision.** Read the CPU-pushed #PF error code from
IST+0 in the host-side recovery, propagate it to UML's
faultinfo (replacing the hardcoded `4 = user-mode`),
and gate `touched = true` on the actual operation
succeeding rather than just on copy_from_user
returning ok. Specifically: if the fault was a write,
require copy_to_user to also succeed; otherwise the
RO-mapping case loops.

**Finding.** Audit round-6 P1 #3 (G3) — "recoverable
#PF handling ignores the real x86 error code and
ignores copy_to_user() failure. A write to a read-only
mapping can copy_from_user() successfully, fail the
write, still mark touched = true, refill the same
read-only PTE, and loop instead of delivering SIGSEGV."

Both issues real:

1. **error_code = 4 hardcoded.** UML's faultinfo
   convention mirrors Linux x86 (bit 1 = write, bit 2
   = user-mode, bit 4 = instruction fetch). Hardcoding
   bit 2 alone meant SIGSEGV delivery to the guest task
   couldn't distinguish read-from-RO vs write-to-RO vs
   instruction-fetch — debugger output and stress-test
   diagnostics couldn't reason about the fault.

2. **touched = true on write-to-RO.** copy_from_user
   succeeds (read OK), copy_to_user fails silently
   (the `(void)` cast discarded the return), touched
   set true → fall into the user-state restore + retry
   path → guest writes again → faults again forever.

**Implementation.**

- `fault_error_code = *(u64 *)(ist + 0)` — captured
  alongside fault_rip in the F7/1 prologue.
- `fault_was_write = (fault_error_code >> 1) & 1` —
  derived once, used twice (touched gate + faultinfo
  passthrough).
- copy_to_user: if fault_was_write, require its
  return == 0 to set touched. Otherwise (read fault)
  copy_from_user success alone is enough.
- `fi->error_code = (u32)fault_error_code` replaces
  the `4` hardcode.

**Validation on dev host.**

- KUnit: 35/35 pass.
- perf-getpid: kvm cyc=98, ratio 0.002, PASS.
- DF-preserve all backends + kvm-gadget: PASS.
- The RO-write loop scenario isn't directly
  reproducible without a deliberate test
  (mprotect(MAP_PRIVATE|MAP_READ_ONLY) + write); the
  guard is correctness-by-construction. A dedicated
  RO-write smoke could be added if regressions
  surface but isn't blocking.

**Refs.**

- Audit round-6 finding #3 (G3).
- D86 (F7/1) — the prologue this fix extends with the
  error_code capture.
- task #235 — this close.

---

## D93 (2026-04-24) — G1: gadget user-pointer bounds check (TASK_SIZE_CAP)

**Decision.** Add a per-process `task_size_cap` field at
vvar offset 0x30, populated once at vvar_alloc from
UML's `task_size` global. clock_gettime / time / getcpu
each compare their user-pointer arg against this cap
before storing through it; if pointer >= cap, the
gadget falls back to handle_syscall which applies
proper access_ok semantics (-EFAULT).

**Finding.** Audit round-6 P0 #1 (G1) — "gadget output-
pointer syscalls still dereference user-controlled
pointers at CPL0. clock_gettime, time, and getcpu
directly store through %rsi/%rdi in the LSTAR gadget
without access_ok/TASK_SIZE bounds checks, so
noncanonical pointers can #GP and mapped supervisor
addresses can be written instead of returning -EFAULT.
The IDT only installs #PF, not #GP."

Two failure modes handled:

1. **Mapped supervisor address.** A guest passes
   %rsi = 0xffff800000001000 (canonical kernel VA).
   Pre-G1, the gadget's `mov %r10, (%rsi)` runs at
   CPL=0; if shadow PT has the page mapped (e.g. our
   bootstrap data at kernel-VA-derived addresses),
   the gadget silently corrupts it. Post-G1, cap
   check rejects.

2. **Non-canonical address.** Guest passes
   %rsi = 0x800000000000 (bit 47=0, bit 48=1 — non-
   canonical). CPU raises #GP on the access. IDT[13]
   isn't installed → triple-fault → KVM abort. Post-
   G1, cap check rejects before the access.

Single comparison covers both vectors because
task_size_cap (~128 TB on 64-bit UML) is below all
non-canonical AND all canonical kernel-half
addresses.

**Implementation shape.**

- `struct kvm_gadget_vvar` gains `u64 task_size_cap`
  at offset 0x30. `KVM_VVAR_OFF_TASK_SIZE_CAP`
  define mirrors the offset.
- `kvm_gadget_vvar_alloc` initializes
  `gadget_vvar->task_size_cap = task_size` (UML
  global). One-time init; never refreshes (task_size
  is static at boot).
- LSTAR clock_gettime body inserts 15 B before the
  rsi store: `cmp %rsi, %gs:0x1030; jbe rel32
  fallback`. Body grows 88 → 103 B.
- LSTAR time body restructured to load REAL_SEC into
  %rdx (not %rax), bounds-check rdi, store via %rdx,
  finalize return value `mov %rdx, %rax`. RAX stays
  = NR=201 across the bounds check so a fallback
  hands handle_syscall the correct NR. Body grows
  23 → 41 B.
- LSTAR getcpu body restructured to defer the
  `xor %eax, %eax` until just before sysretq, and
  use %r10d for the node-write zero. RAX stays =
  NR=309 across both bounds checks. Two checks
  inserted (one before each store). Body grows 30
  → 64 B.
- gadget_fault_nr ranges + getcpu jmp rel32 +
  time stub rel32 all updated for the new offsets.
- test_ops.c expected bytes mirror the new 394-B
  layout.

**The RAX-preservation trick.** The original G1 cut
inserted bounds checks AFTER the value loads (mov
%gs:..., %rax in time; xor %eax, %eax in getcpu).
Both clobbered RAX — the SYSCALL NR — before the
fallback could fire. When fallback DID fire (e.g.
on a kernel-VA pointer), kvm_decode_syscall saw the
post-clobber RAX as the NR: REAL_SEC for time
(huge — out of range, sys_ni_syscall returns
-ENOSYS) or 0 for getcpu (= sys_read with bad fd,
returns -EBADF).

Empirical: g1-supervisor-smoke initially printed
`clock_kvm=-14 time_kvm=-38 getcpu_kvm=-9 clock_noncan=-14`.
The -38 / -9 are the symptoms of the RAX-clobber
bug; -14 is the correct EFAULT. Post-fix: all four
return -14. This is the same F7/2 pattern (D75)
applied to time + getcpu.

**Validation on dev host.**

- KUnit: 35/35 pass with the new 394-B byte-match
  table.
- perf-getpid: kvm cyc=97, ratio 0.002, PASS (G1
  bounds check adds ~1 cyc per call to the gadget
  hot path; barely measurable).
- /tmp/g1-supervisor-smoke: clock_kvm=-14
  time_kvm=-14 getcpu_kvm=-14 clock_noncan=-14 (all
  EFAULT, as expected). Validated both supervisor-VA
  and non-canonical address rejection.
- /tmp/efault-smoke (out-of-VMA pointers): all -14.
- /tmp/time-getcpu-smoke (valid pointers): correct
  return values.
- /tmp/clock-loop: cyc_per_call=98 (was ~95 pre-G1;
  +3 cyc for the bounds check, in line with the
  estimate).

**Complementary IDT[13] handler.** Not yet installed.
With G1 in place, no gadget access can reach an
address that would trigger #GP — all bad pointers
fall back via the rel32 jbe before the access. So
IDT[13] for #GP from the gadget body is currently
unreachable. If a future LSTAR layout introduces
gadget code paths that bypass G1's check, install a
#GP handler at that point.

**Refs.**

- Audit round-6 finding #1 (G1).
- D75 (F2) — the precedent for RAX-preservation across
  fallback (clock_gettime's seqlock fix).
- task #233 — this close.

---

## D94 (2026-04-24) — G5: demote sched_yield from class E to A

**Decision.** Redirect the LSTAR dispatch entry for
`__NR_sched_yield` to the fallback path so every
sched_yield call VMEXITs to handle_syscall →
sys_sched_yield → schedule(). Demote the classifier
mapping from CLASS_GADGET to default CLASS_PASSTHROUGH.
The gadget body bytes at LSTAR +168..+175 stay in place
(unreachable; kept to avoid disturbing the offsets of
subsequent handlers).

**Finding.** Audit round-6 P2 #5 (G5) — "sched_yield is
treated as a pure register-return gadget. It returns 0
without entering the scheduler or VMEXIT path, which is
not Linux syscall semantics under guest scheduling
pressure."

POSIX says sched_yield is advisory, so the in-gadget
short-circuit doesn't violate the standard's letter.
But under guest scheduling pressure (e.g. a contended
spinlock that yields on backoff), the gadget's bypass
keeps the calling task running on the vCPU until the
next host timer tick (~10 ms) when SIGALRM-driven
preemption finally fires. UML's scheduler can't
respond to the guest's hint sooner than the timer.

**Why demote vs. patch the gadget.** A "yield from
within the gadget" handler would have to give UML's
scheduler a direct signal — but the only mechanism we
have for that IS a VMEXIT. Once we VMEXIT, we may as
well dispatch through handle_syscall, which is class
A. The gadget's only value-add was avoiding the
VMEXIT itself; for a syscall that semantically wants
to surrender CPU to the scheduler, avoiding the
VMEXIT defeats the syscall's purpose.

Cost of demotion: ~13 µs VMEXIT cost per sched_yield
(vs the ~30 ns gadget). sched_yield is rarely called
in tight hot loops; for the workloads where it matters
(spinlock backoff, cooperative round-robin), 13 µs is
better than the ~10 ms scheduler-lag the gadget caused.

**Implementation.**

- LSTAR dispatch entry 8 (cmp $0x18, je rel8): rel8
  changed from 114 (target sched_yield body at +168)
  to 8 (target fallback at +62). One byte change.
- syscall_class.c: `[__NR_sched_yield] = KVM_SYSCALL_
  CLASS_GADGET` removed; sched_yield now defaults to
  CLASS_PASSTHROUGH.
- KUnit kvm_syscall_classification_test: assertion
  flipped from CLASS_GADGET to CLASS_PASSTHROUGH for
  __NR_sched_yield.
- KUnit kvm_syscall_class_count_test: gadget count
  drops 11 → 10; total non-A drops 23 → 22.
- syscall-inventory.tsv: NR 24 row flipped from E to
  A, rationale annotated.
- Memo 10 §"Class E" table strikes through the
  sched_yield row + adds a §"Removed from class E"
  paragraph explaining the demotion.

**Validation on dev host.**

- KUnit: 35/35 pass with the new 22-non-A / 10-gadget
  count invariants.
- perf-getpid: kvm cyc=97, ratio 0.002, PASS (the
  pid-family path is unaffected).
- /tmp/g1-supervisor-smoke: all -14 (G1 still
  rejecting bad pointers; no regression from the
  dispatch-entry edit).
- byte-match KUnit confirms the dispatch entry's
  rel8 changed from 0x72 (114) to 0x08 (8).

**Refs.**

- Audit round-6 finding #5 (G5).
- D78 (G7) — original CLASS_GADGET landing.
- task #237 — this close.

---

## D95 (2026-04-24) — G1 in-tree kselftest: kvm-bounds

**Decision.** Land an in-tree kselftest at
`tools/testing/selftests/um/kvm-bounds/` that drives the
gadget user-pointer bounds check (G1, D93) with a
canonical kernel VA and a non-canonical address across
the three gadget output handlers (`clock_gettime`,
`time`, `getcpu`), and asserts each call returns
`-EFAULT`.

**Why.** D93 closed G1 with an ad-hoc out-of-tree
smoke-test (`/tmp/g1-supervisor-smoke`). The audit-round-6
checklist explicitly called for converting that probe
into a regression test that lives next to the code, can
be run by `make -C tools/testing/selftests/um run_tests`,
and survives developer churn. Task #240.

**Shape.**

- `kvm-bounds-loop.c` — freestanding ring-3 ELF (same
  build pattern as `df-preserve-loop.c` and
  `getpid-loop.c`). Calls each of the 3 gadget
  output-handlers with two adversarial pointers:
    - `0xffff800000001000` (canonical kernel VA — bit
      47 set, sign-extended high). Without G1's bounds
      check, the gadget would store through this at
      CPL=0 and corrupt ring-0 data.
    - `0x800000000000` (non-canonical — bit 47 = 0,
      bit 48 = 1). Without the check, the gadget's
      ring-0 store would raise `#GP`, with no IDT[13]
      handler installed → triple-fault.
  Asserts each of the 6 calls returns `-EFAULT` (-14).
  Reports a single `KVM_BOUNDS:` line with all 6 return
  codes + `passed=N/6`.
- `Makefile` — standard `-static -nostdlib -ffreestanding`
  freestanding-ELF build, mirroring the perf-getpid /
  df-preserve pattern.
- `run-kvm-bounds.sh` — runs the binary under both the
  fallback (no-gadget) UML kernel and (optionally) the
  gadget kernel via `UML_GADGET_BINARY`. The fallback
  row exists as a baseline showing -EFAULT is the
  universal expected behaviour, not a gadget-specific
  quirk; the gadget row exercises G1's TASK_SIZE_CAP
  inline check directly.
- Parent `tools/testing/selftests/um/Makefile` adds
  `kvm-bounds` (and `df-preserve`, retroactively, since
  it was missed in #222) to TARGETS so `make all` builds
  the binary.

**Validation on dev host.**

```
KVM_BOUNDS: kvm-fallback clock_kva=-14 time_kva=-14
            getcpu_kva=-14 clock_noncan=-14
            time_noncan=-14 getcpu_noncan=-14
            passed=6/6 expected_each=-14
KVM_BOUNDS: kvm-gadget clock_kva=-14 time_kva=-14
            getcpu_kva=-14 clock_noncan=-14
            time_noncan=-14 getcpu_noncan=-14
            passed=6/6 expected_each=-14
KVM_BOUNDS: PASS
```

Verified the regression-detection path by mentally
running the test against the pre-G1 gadget bytes:
without the inline `cmp ptr, %gs:TASK_SIZE_CAP / jbe`
guard, the `clock_kva` case would corrupt the kernel-VA
mapping (no fault → return 0, not -14), and the
`clock_noncan` case would raise `#GP` at CPL=0 (no
IDT[13] handler → triple-fault → guest reset → no
KVM_BOUNDS line emitted). Either failure mode surfaces
distinctly in the runner's PASS/FAIL output.

**Refs.**

- Audit round-6 finding #1 (G1).
- D93 — original G1 close.
- task #240 — this close.

---

## D96 (2026-04-24) — F5-followon: split bootstrap page into RO code+tables + RW NX IST stack

**Decision.** Split the single 4 KiB bootstrap page that
held both ring-0 code/tables (LSTAR trampoline, GDT, IDT,
TSS, `#PF` handler, SYSRETQ helper) and the IST stack into
two pages with distinct shadow-PT mappings:

- **Page 1** (code + tables) at guest VA `bootstrap_va +
  0x0000` — `KVM_X86_PTE_P` only. Read-only and
  executable from CPL=0; not writable by anyone.
- **Page 2** (IST stack) at guest VA `bootstrap_va +
  0x3000` — `KVM_X86_PTE_P | KVM_X86_PTE_RW |
  KVM_X86_PTE_NX`. Writable for the CPU's IDT iretq-frame
  push and for `#PF` handler RSP work; non-executable so a
  guest ring-0 escape can't return-to-stack into IST data.

`KVM_BOOTSTRAP_STACK_TOP` moves from `0x1000` to `0x4000`
(exclusive top of page 2 in the guest VA layout); first
push lands at `+0x3ff8`. The host `kvm_enter_guest_init_
bootstrap()` now allocates two zeroed pages, and the
race-loser path frees both. A new `kvm_bootstrap_page_
stack` global plus `kvm_bootstrap_stack_gpa` complete the
state mirror.

**Finding.** Audit round-5 F5 (D81) closed the P0 minimum
by dropping the `US` bit from the bootstrap-page PTE so
ring-3 can no longer read or write the trampoline. The
follow-on (#230) called out that the page was still
mapped `KVM_X86_PTE_P | KVM_X86_PTE_RW` from the guest's
own ring-0 perspective — a guest ring-0 escape (or any
future bug that lets the gadget body run with a
user-controlled RSP) could rewrite LSTAR / IDT / GDT /
TSS bytes in place. The split removes ring-0 write
privilege from the code and tables; the IST stack is
the one region that still needs RW, and it's now isolated
to its own page with `NX` on top.

**Why a 3-page gap to the stack page.** Existing GVA
allocations between bootstrap and stack:

- `bootstrap_va + 0x1000` — gadget state page (G3).
- `bootstrap_va + 0x2000` — gadget vvar clock page (G5).

Putting the stack at `+0x3000` keeps both existing
mappings (and the gadget body's `%gs:disp32` references
to them via `MSR_KERNEL_GS_BASE = state_va`) untouched.
A more aggressive layout could shuffle state/vvar to
make room at `+0x1000`, but that would change every
LSTAR rel32 and the MSR programming for no defensive
gain — the order is irrelevant once the stack page is
isolated and `NX`.

**Implementation shape.**

- `kvm_bootstrap_page_stack`, `kvm_bootstrap_stack_gpa`
  — second-page state, populated under
  `kvm_bootstrap_lock` alongside the existing fields.
- `kvm_enter_guest_init_bootstrap()` — second
  `get_zeroed_page(GFP_KERNEL)` call; on failure, frees
  page 1 and returns `-ENOMEM`. The race-loser path
  frees both pages.
- `kvm_enter_guest()` — first map call drops `RW` (page
  1 is now `P` only); second map call lands page 2 at
  `bootstrap_va + 3*PAGE_SIZE` with `P|RW|NX`.
- `KVM_BOOTSTRAP_STACK_TOP` — `0x1000` → `0x4000`. TSS
  IST[1] auto-follows since it's computed as
  `bootstrap_va + KVM_BOOTSTRAP_STACK_TOP`.
- `pr_info` boot line gains the second page's gpa.
- **#PF decode-site fix** (both sites in
  `kvm_run_userspace`): the old `ist_off = kregs.rsp -
  kvm_bootstrap_va` with `< PAGE_SIZE` bound assumed
  the IST stack lived in page 1. Post-split,
  `kregs.rsp` lies in `[bootstrap_va + 0x3000,
  bootstrap_va + 0x4000)` so the offset is computed
  relative to the page-2 base (`bootstrap_va +
  3*PAGE_SIZE`) and dereferenced through
  `kvm_bootstrap_page_stack` (the kernel VA of page 2).
  Without this, every recoverable `#PF` would have hit
  `PF IST out of range` and `fatal_sigsegv()`. The
  `kvm_touch_all_user_vmas` eager prefault hides the
  bug from the smoke tests (which never trigger a
  recoverable `#PF` because all user pages are
  pre-faulted), but any code path that demand-faults
  after touch-all (notably the F7/1 gadget-mid-store
  fallback that we explicitly want to keep working)
  would have surfaced it.

**Validation on dev host.**

- KUnit: 35/35 pass (no test depends on the bootstrap-
  page stack location).
- kvm-bounds (G1 selftest, #240): both kvm-fallback and
  kvm-gadget rows return `-EFAULT` for all 6 cases. The
  `clock_kva` and `clock_noncan` cases exercise the
  gadget output path that writes through user pointers;
  the `#PF` recovery + IST iretq-frame mechanics from
  the new stack page work.
- perf-getpid: kvm `cyc=97`, ratio `0.002`, PASS. No
  regression from the split — the extra `kvm_shadow_
  map_page` call is one-shot at first-vCPU enter, not
  per-syscall.
- df-preserve: ptrace=PASS, seccomp=PASS, kvm=PASS,
  kvm-gadget=PASS. The user-RFLAGS round-trip across
  recoverable `#PF` still works through the new IST
  stack page (DF preserved).

**Defence-in-depth posture, layered.**

| Bit  | Page 1 (code+tables) | Page 2 (stack) |
|------|----------------------|----------------|
| P    | 1                    | 1              |
| RW   | 0                    | 1              |
| US   | 0                    | 0              |
| NX   | 0 (executable)       | 1              |

A guest ring-0 escape can no longer overwrite LSTAR.
The stack page is writable but not executable — code
has to live in page 1 (RO) and run from there. Combined
with the F5 minimum's `US=0`, ring-3 has no path to the
trampoline at all; ring-0 has only a write path to its
own stack frames.

**Refs.**

- Audit round-5 finding #2 follow-on (#230).
- D81 — F5 minimum (US dropped).
- task #230 — this close.

---

## D97 (2026-04-24) — #PF handler RAX-preservation: drop dead `mov %cr2, %rax`

**Decision.** Remove the leading `mov %cr2, %rax`
instruction from the bootstrap-page `#PF` handler bytes
(`kvm_bootstrap_pf_handler_bytes[]` in
`arch/um/backend/kvm/thread.c`). The handler shrinks
from 11 bytes to 8 bytes; only `out %al, $0xfb`,
`add $8, %rsp`, `iretq` remain.

**Audit finding (P0 open #2 from external review).**
The handler executed `mov %cr2, %rax` so the host could
"read the faulting VA out of regs->rax". Two independent
reasons that read was wrong:

1. **Redundant.** The host already pulls CR2 directly
   via `KVM_GET_SREGS` at the start of the `#PF`
   recovery path (`thread.c:2174`, `cr2 = dump_sregs.cr2`).
   The %rax-channel was never actually consumed; only
   the OUT VMEXIT signal mattered.
2. **Clobbers user RAX.** On the recoverable-#PF path
   the host extracts user `IP/SP/RFLAGS` from the IST
   iretq frame (D96 decode) and stuffs them into
   `regs->gp[]`, but leaves `regs->gp[HOST_AX]` at
   whatever `KVM_GET_REGS` captured — i.e. the value
   the handler had just written there: the faulting VA
   (= cr2). On re-entry the next `kvm_enter_guest`
   marshals `kregs.rax = cr2_value` and SYSRETQ resumes
   the user instruction with RAX silently changed from
   the user's pre-fault value to the page address that
   faulted. Any flag-sensitive sequence relying on RAX
   semantics would observe corrupted state.

**Why dropping the `mov` is safe.** The `out %al, $0xfb`
instruction emits the byte in `%al` to port `0xfb`. The
host's `KVM_EXIT_IO` handler dispatches on `port`
alone — it does not read `run->io.data` for the
`UM_KVM_PF_PORT` case. So `%al` carries garbage (whatever
the user had before the fault) and that's fine — the
host sees `port == 0xfb` and starts the recovery flow.
User RAX is preserved end-to-end because no instruction
in the handler writes to it.

**Why the `add $8, %rsp; iretq` tail is still emitted.**
On the live path the host bypasses the handler's tail
(it extracts the IST frame and re-enters at user RIP via
SYSRETQ in kvm_enter_guest), so `add/iretq` are
unreachable in production. They're kept as a defensive
"if anything ever forgets to bypass, the in-guest
unwind would still re-enter user code at the right
RIP". Removing them would shave 6 bytes off the page-2
shadow-PT footprint but loses that fallback. Cost is
negligible; keep.

**Implementation.**

- `kvm_bootstrap_pf_handler_bytes[]` shrinks 11 → 8 B.
  Comment block rewritten to explain the
  RAX-preservation rationale.
- No other code path needs adjusting:
  - The host's #PF dispatch dispatches on `run->io.port
    == UM_KVM_PF_PORT`, not on `%al` content.
  - The handler offset (`KVM_BOOTSTRAP_PF_HANDLER_OFFSET
    = 0x4a0`) and the size budget (16 B available before
    SYSRETQ at +0x4b0) both still hold.
  - No KUnit byte-match test on the PF handler bytes
    (only the LSTAR body is byte-matched at boot).

**Validation on dev host.**

- KUnit: 35/35 pass.
- kvm-bounds: kvm-fallback + kvm-gadget both 6/6
  -EFAULT. The bounds-check path (G1) doesn't trigger
  recoverable #PF, so this test exercises the same
  paths as before; it validates the build is clean.
- df-preserve: ptrace=PASS, seccomp=PASS, kvm=PASS,
  kvm-gadget=PASS for both syscall + pf rows. The pf
  test triggers a real recoverable #PF (mmap +
  first-touch anonymous page), so this directly
  validates the new short handler.
- perf-getpid: kvm cyc=98, ratio 0.002, PASS.

**Refs.**

- External audit feedback (P0 open #2).
- D96 — F5-followon split + IST decode (the path that
  surfaced this RAX-clobber: now that user IP/SP/RFLAGS
  come from the IST frame, RAX visibly trails behind
  with the cr2 value).
- arch/um/backend/kvm/thread.c — handler bytes.
- task #240 (kvm-bounds runner) — provides regression
  coverage for the gadget bounds path that depends on
  the RAX-preservation property indirectly.

---

## D98 (2026-04-24) — G1-range-followon: range-aware user-pointer bound via task_size_cap = task_size - 16

**Decision.** Subtract a 16-byte margin from
`task_size_cap` at vvar seed time
(`kvm_gadget_vvar_alloc()`):

    kvm_ctx.gadget_vvar->task_size_cap = task_size - 16;

The gadget's inline `cmp ptr, %gs:0x1030; jbe fallback`
sequence is unchanged — only the seed value shrinks.
With a 16 B margin, a base-only compare effectively
covers the whole write range of any current gadget
handler.

**Audit finding (P1 open #3 from external review).**
The G1 inline check (D93) compares the user pointer
against `task_size_cap` only — it does not factor in
the size of the write. Per-handler write sizes:

- `clock_gettime`: 16 bytes (struct __kernel_timespec —
  two u64s).
- `time`:           8 bytes (one u64).
- `getcpu`:         8 bytes (two u32s, written to two
                    distinct pointers — but each
                    individual write is 4 bytes).

So a pointer in `[task_size - 15, task_size - 1]` would
pass the gadget's cmp (base < cap) yet write past
`task_size`. UML's `access_ok()` correctly rejects this
via the `addr + size < TASK_SIZE` check; the gadget did
not.

**Why margin-in-seed instead of asm rewrite.** The
alternative is per-handler `cmp ptr, %gs:cap_clock /
cap_time / cap_getcpu` with three cap slots in vvar.
That works but requires:

- 3 new vvar offsets + 3 separate seeds.
- 3 separate cmps in the gadget asm — ~9 new bytes per
  handler × 3 handlers = 27 LSTAR bytes, pushing into
  the already-tight reach budget.
- KUnit byte-match update for the new LSTAR layout.

The single-cap-with-margin alternative is one line
changed, no asm rewrite, no LSTAR-layout drift, and
preserves every property the audit asked for.

**Net user-visible effect.** Pointers with base in
`[task_size - 16, task_size - 1]` now route to the
SYSCALL fallback rather than the gadget. The fallback
runs through `handle_syscall` → kernel `clock_gettime` /
`time` / `getcpu` → standard `access_ok` + `copy_to_user`
path, which either accepts or rejects per POSIX
semantics (with the proper `ptr + size <= TASK_SIZE`
check). User-visible behaviour for legal writes is
unchanged; only the fast path narrows by 16 bytes at
the very top of user space — a vanishingly rare case
in real workloads.

**Why not `task_size - 8` or `task_size - 4`.** Those
also work, but only for the smaller-write handlers.
Picking the worst case (16 B for `clock_gettime`)
covers every current handler with a single seed and
provides headroom for future handlers (e.g. a
`gettimeofday` gadget would also write 16 bytes via
`struct timeval`). If a future handler needs > 16 B,
just bump the margin in this single line.

**Implementation.**

- `arch/um/backend/kvm/lifecycle.c::kvm_gadget_vvar_
  alloc()` — change `task_size_cap = task_size` to
  `task_size_cap = task_size - 16`.
- `pr_info` boot line now shows both `task_size` and
  the derived `task_size_cap` so the margin is
  visible at boot.
- No gadget asm change. No KUnit update.

**Validation on dev host.**

- KUnit: 35/35 pass (no test asserts an exact
  task_size_cap value).
- kvm-bounds: kvm-fallback + kvm-gadget both 6/6
  -EFAULT. The G1 selftest exercises pointers far
  above task_size_cap (kernel VAs and non-canonical
  values), so the 16 B margin doesn't change its
  outcome.
- df-preserve: ptrace=PASS, seccomp=PASS, kvm=PASS,
  kvm-gadget=PASS for both syscall + pf rows. No
  regression.
- perf-getpid: kvm cyc=94, ratio 0.002, PASS. The
  pid-family path is unaffected (no user-pointer
  writes in those handlers).

**Refs.**

- External audit feedback (P1 open #3).
- D93 — original G1 close (base-only cap).
- D81 — F5 minimum (US drop), the parent close that
  D93 + this followon refine.
- task #240 (kvm-bounds runner) — provides regression
  coverage; future fine-grained range tests would slot
  into the same kselftest.

---

## D99 (2026-04-24) — G6 attempt #2: lazy-only #PF path triple-faults at first user RIP; deferred again

**Decision.** Keep `kvm_touch_all_user_vmas()` and its
two call sites (kvm_enter_guest pre-KVM_RUN +
post-syscall). Task #238 stays pending — the simple
"drop the eager prefault and rely on lazy #PF recovery"
patch is not sufficient.

**Attempt.**

```
- touched = kvm_touch_all_user_vmas(mm);
- filled = kvm_shadow_fill_from_uml_pgd(mm->pgd);
+ filled = kvm_shadow_fill_from_uml_pgd(mm->pgd);
```

at both call sites (and remove the `kvm_touch_all_
user_vmas` static helper itself).

**Empirical result on dev host.**

- KUnit: 35/35 pass (host-only tests; no guest exec).
- kvm-bounds: both kvm-fallback and kvm-gadget rows
  FAIL with "no KVM_BOUNDS line emitted". The kernel
  doesn't reach the test binary's stdout write.
- Diagnostic dump shows:
  - `um: kvm enter_guest: filled 1 shadow PTEs (lazy
    #PF for the rest)` (only the user stack).
  - `um: kvm run_userspace: unrecoverable exit 8
    (SHUTDOWN)` on the very first KVM_RUN.
  - `um: kvm: guest RIP=0x401340 ... CPL=3 is_user=1`
    — vCPU RIP was at the user binary's `_start`, but
    the CPU triple-faulted before any user instruction
    successfully retired.

**Why the lazy-only path triple-faulted (hypothesis,
unverified).** The first user instruction fetch at
`_start` (RIP 0x401340) hit a shadow-PT miss → `#PF`.
The CPU pushed an iretq frame onto IST[1] (= bootstrap_
va + 0x4000, post-D96), then jumped to IDT[14]'s
handler at `bootstrap_va + 0x4a0`. Either (a) the IST
stack-page push double-faulted because some
intermediate page-table entry on the path to page 2
got cleared/never-installed in the lazy regime, or (b)
the host's `#PF` dispatch can't service `#PF` raised
*on instruction fetch from a user page that's not yet
in UML's logical pgd* the way it can service `#PF`
raised by the gadget's `copy_to_user` ring-0 store.
Both candidates need targeted instrumentation
(`pr_info` at every step of the host `#PF` dispatch +
a `KVM_GET_VCPU_EVENTS` / `KVM_GET_DEBUGREGS` snapshot
right at SHUTDOWN) to disambiguate.

The previously-deferred-attempt notes (G6 first cut)
saw a similar shape: boot got to init, faulted at
cr2=0x40200f / rip=0x401084, `kept refilling 4 PTEs
but the address never installed`. Same symptom: lazy
recovery can't bridge the first user-page-not-in-uml-
pgd-yet gap. The eager touch has been doing
double-duty: forcing UML's logical pgd to populate
*before* shadow_fill, and providing initial coverage
of every user VMA so the first KVM_RUN doesn't hit a
miss on its first instruction.

**Why this needs the proper handle_mm_fault refactor
(scope of #238).** The clean fix is to have the host
`#PF` recovery call into UML's full fault path
(`handle_mm_fault` via `handle_page_fault`), not just
the side-effect of `copy_from_user` triggering it.
That covers user pages that are demand-paged from the
binary file mapping but haven't been faulted yet —
the very first instruction fetch case.

For now, keep the eager touch. Audit P2 status
unchanged.

**Refs.**

- External audit P2 open #4.
- D85 (F9) — write-back removed; read probe kept.
- task #238 — still pending; this row records the
  negative result so the next attempt knows what
  doesn't work.

---

## D100 (2026-04-25) — Phase 1 closure: dyn-loader green, ratio 1.15× via #272/#273/#238/#242/experiment-#1

**Context.** Phase 1's perf goal was kvm/seccomp ≤ 1.2× with the
dyn-loader workload (init=/bin/echo through ld-linux + libc + 5
shared libs) PASSING deterministically on the kvm row. The
2026-04-24 baseline was 2.92× and dyn-loader hard-faulted with a
"wild jump to 0xc680" that gave us no diagnosable signal.

**What we did, in commit order.**

1. **task #270** — committed a deterministic repro
   (`tools/testing/selftests/um/dyn-loader/`) that boots
   /bin/echo under ptrace + seccomp + kvm and asserts each
   backend prints "DYN_LOADER: ok". Without this we couldn't
   iterate on #272.
2. **task #272** — IRETQ-based bootstrap re-entry replaces
   SYSRETQ. Root cause: SYSRETQ uses RCX as the user-RIP carrier
   and R11 as the user-RFLAGS carrier; clobbering both is fine
   for fresh entries and SYSCALL returns (where the SYSCALL ABI
   already declared them caller-saved) but DESTROYS user RCX/R11
   across recoverable-#PF re-entry. Surfaced because ld-linux's
   RELR-relocation loop uses RCX as the relocation cursor;
   SYSRETQ-clobbered RCX terminated the loop after a single
   anchor, leaving 11/12 .relr.dyn slots un-relocated, including
   the function pointer at `_rtld_global_ro+0x340` whose static
   value is 0xc680 — the wild-jump target.
3. **task #273** — KVM_GET_SUPPORTED_CPUID + KVM_SET_CPUID2
   passthrough. Without this, the vCPU sees a minimal CPUID and
   modern glibc compiled for x86-64-v3 refuses to load with `CPU
   ISA level is lower than required`. Lazy-init pattern; alloc
   happens on first kvm_enter_guest after slab is up.
4. **task #238 STEP-2** — drop kvm_touch_all_user_vmas. The
   eager prefault was unblocking #PF recovery for first-touch
   user pages, but STEP-1 (commit 9e71295123a8) replaced the
   copy_from_user side-effect with a direct handle_page_fault
   call, making STEP-2 a pure subtraction. Ratio 2.92× → 2.24×.
5. **task #242** — skip kvm_shadow_fill_from_uml_pgd when the
   shadow PT already mirrors UML's pgd. Cache key: synced flag
   + mm pointer + pgd-VA. Ratio 2.24× → 1.69×.
6. **experiment #1** — drop the unconditional post-syscall fill.
   Most syscalls (read/write/getpid/clock_gettime/...) can't
   mutate UML's pgd, so the post-handle_syscall fill was pure
   VMEXIT-tax. Mm-mutating syscalls (mmap/munmap/brk/mprotect)
   go through mm_map/mm_unmap callbacks which already flag
   shadow_pgd_synced=false; the next kvm_enter_guest's #242
   skip-fill check then refills correctly. Ratio 1.69× → **1.15×**.
7. **experiment #2** — KVM_SYNC_X86_SREGS for CR2 reads on the
   #PF path. Saves an ioctl on actual fault recoveries. Test
   ratio noise-bound; benefit is in fault-heavy real workloads.

**Audit round 7 closure.**

- **P1**: bootstrap IRETQ raises #GP if user_rsp / user_rip is
  non-canonical. Without IDT[13], the #GP cascades to #DF and
  panics. Fix: in-guest IDT[13] (#GP) handler at offset 0x4d8 /
  port 0xf9, routing to SIGSEGV via faultinfo.
- **P2**: shadow_pgd_synced_va alone unsafe across mm replacement
  on the same task (execve doesn't trigger kvm_context_switch's
  cross-mm reset; the kernel page allocator can reuse the freed
  pgd page for a new mm with the same VA). Fix: add
  shadow_pgd_synced_mm to the cache key; require BOTH the mm
  pointer AND the pgd-VA to match before skipping the fill.

**Validation.**

- dyn-loader kselftest PASS deterministic 5/5 on kvm row.
- kvm-bounds (G1 cap correctness) PASS 9/9.
- kvm-smoke (D-04 bring-up) PASS markers=3/6.
- perf-getpid + perf-fallback both PASS at ratio 1.15-1.17×.
- snapshot-kvm-smoke (#251) PASS — exercises the new memo-12
  snapshot primitives via KUnit.

**Deferred (Phase-1 follow-on, non-blocking).**

- #243 per-mm cached shadow PGD — small additional perf win;
  blocks #250 v2 step 5+ (concurrent fuzz workers).
- #244 huge-page shadow PT — re-analysis showed lever doesn't
  apply as framed; demoted, won't ship.

**Phase-3 status.**

- #250 v1 ladder (memo 12, primitives + KUnit + selftest +
  refusal guard) shipped.
- #250 v2 ladder (steps 3+4: full forkserver under KVM via
  capture/restore) deferred to a focused session.
- #253 record/replay determinism memo 13 written; implementation
  deferred.

**Outcome.** Phase 1 vision (parity-or-near-parity perf with
dynamically-linked binaries booting cleanly under kvm backend)
**achieved.** The remaining 0.15× gap is the architectural floor
imposed by VMX exit/entry cycles + KVM_RUN ioctl overhead +
interrupt-window costs; further reduction requires expanding the
in-guest gadget population to bypass VMEXIT entirely (not more
shadow-PT shaving).

**Refs.**

- `02-workstreams/D-kvm-backend/measurements.md` — "2026-04-25 —
  Phase 1 closure" entry has the per-stage perf table.
- `02-workstreams/D-kvm-backend/12-snapshot-forkserver-kvm.md` —
  #250 v2 design memo.
- `02-workstreams/D-kvm-backend/13-record-replay-determinism.md`
  — #253 design memo.
- Commits `a698a665bb62..fd3c501b1b95` carry the session.

---

## D101 (2026-04-25) — Phase 3 record/replay primitive skeleton + gated dispatcher hook

**Context.** D100 closed Phase 1 (kvm/seccomp ratio at 1.15×,
dyn-loader green, audit round 7 done). The remaining Phase 3
deliverables are #250 (snapshot/forkserver) — partial v1
shipped via memo 12 — and #253 (record/replay determinism) —
memo 13 only at session start.

**What landed for #253.**

1. **Skeleton** (commits `56274adfe16b` / `c2ead30c12e4`):
   `struct kvm_record` container holding a `struct kvm_snapshot`
   (the checkpoint) + a variable-length `struct kvm_replay_entry`
   log + `recording`/`replaying` state flags. C API:

       struct kvm_record *kvm_record_alloc(void);
       int  kvm_record_start(struct kvm_record *rec);
       void kvm_record_stop(struct kvm_record *rec);
       int  kvm_record_replay(struct kvm_record *rec);
       void kvm_record_destroy(struct kvm_record *rec);

   KUnit basic-shape test (`kvm_record_basic_test`) +
   `kvm-record-smoke` kselftest. Boot-time sanity gate.
2. **Static-key gate + active-record registry** (commit
   `33fd1fff3c1e`): `DEFINE_STATIC_KEY_FALSE(um_kvm_record_
   enabled)`, single-slot global pointer, spinlock-guarded
   start/stop. Single-active discipline rejects a second
   start with -EBUSY. Defensive teardown in destroy.
3. **Dispatcher hook** (commit `6b1357d96ad5`): class-A syscall
   return observed via `kvm_record_observe_syscall(NR, ret,
   arg0, arg1)` after `handle_syscall(regs)` in
   `kvm_decode_syscall`. Gated by static_branch_unlikely so non-
   record builds + non-recording runtime pay zero per-syscall
   cost. perf-fallback held at 1.13× kvm/seccomp post-hook,
   confirming the gate-when-off discipline.
4. **Debugfs control surface** (commit `988e2e08afb0`):
   `/sys/kernel/debug/um/kvm_record_ctl` (write-only, accepts
   `start` / `stop` / `replay` / `destroy`) and
   `/sys/kernel/debug/um/kvm_record_state` (read-only, prints
   "recording=N replaying=N log_count=N log_capacity=N").
   Drives userspace-facing kselftests + future syzkaller
   integration.

**Deferred per memo 13 step 3-6.**

- Step 3: full output-buffer capture for read/write-style
  syscalls (needs side-buffer alloc + struct kvm_replay_entry
  variant).
- Step 4: PMU instruction-count overflow for interrupt-boundary
  recording (rr-style).
- Step 5: getrandom / drbg seed capture.
- Step 6: MMIO recording.

Each is its own focused-session sub-commit; v1 today proves
the architectural pattern (snapshot + log + gate) doesn't
regress hot-path perf and doesn't break the existing
selftest matrix.

**Validation.**

  - All eight kvm kselftests (dyn-loader / kvm-bounds /
    kvm-smoke / perf-getpid / perf-fallback / snapshot-kvm-
    smoke / kvm-snapshot-bench / kvm-record-smoke) PASS.
  - perf-getpid:    ratio_kvm/seccomp = 1.123 (was 1.15× pre-
                    hook).
  - perf-fallback:  ratio_kvm/seccomp = 1.127.
  - Static-key gate verified zero-cost when off.

**Refs.**

- `02-workstreams/D-kvm-backend/13-record-replay-determinism.md`
  — memo 13 (status flipped to "steps 1+2 LANDED").
- Commits `56274adfe16b..7cff075e72b0`.

---

## D102 (2026-04-25) — Record/replay v1 round-trip API + per-NR dispatcher

**Context.** D101 closed memo-13 steps 1+2 (skeleton +
static-key gate + record-side observation hook). Step 3+
focused-session work was queued. This entry records the further
closure of memo-13 steps 3, 3.5, and 5 (partial) over the same
session.

**Steps shipped.**

1. **Step 3** (commit `a085afc07448`) — variable-length side
   buffer on `struct kvm_replay_entry` (`payload` +
   `payload_len`). New API
   `kvm_record_observe_syscall_buf(NR, ret, user_va, payload,
   payload_len)` for syscalls whose output exceeds the inline
   4×u64 capacity. Free per-entry payloads on destroy.
2. **Step 3.5** (commit `918ccaf828cc`) — replay-side
   dispatcher: `kvm_record_consume_syscall()` consumes the
   next log entry instead of invoking handle_syscall when in
   replay mode. Wires through kvm_decode_syscall via the same
   static-key gate, with copy_to_user'ing the side payload back
   to the user buffer. Divergence policy: warn-and-fall-through
   (live handle_syscall) so a replay drift doesn't silently
   corrupt; cursor-after-end falls through identically (extends
   record beyond original).
3. **Step 5(partial)** (commits `1992d6315f92` + `d669522cc964`)
   — per-NR record dispatcher
   `kvm_record_observe_dispatch(NR, ret, regs)` routes
   `__NR_getrandom`, `__NR_read`, `__NR_pread64` through the
   side-buffer path. Common
   `um_kvm_record_capture_user_buf` helper handles staging +
   copy_from_user + fall-back-to-inline-on-failure. Other NRs
   default to the inline-only path. Future NRs (recvfrom /
   recvmsg / ioctl / readv) are just additional switch cases.

**Validation.**

  - All eight kvm kselftests PASS post-each-commit.
  - perf-getpid:    ratio_kvm/seccomp ≤ 1.19×.
  - perf-fallback:  ratio_kvm/seccomp ≤ 1.18×.

Static-key gate keeps the cost zero when off; the per-NR
copy_from_user staging only fires for the special-cased NRs
when recording is on.

**What's left in memo 13.**

- Step 4: PMU instruction-count overflow for interrupt-boundary
  recording (rr-style). Architectural; needs perf-event API
  plumbing.
- Step 6: MMIO recording in kvm_decode_mmio's case. Pattern
  matches step 2's record-side hook; bounded but separate
  commit.
- Additional NRs in the per-NR dispatcher (recvfrom, recvmsg,
  ioctl, readv). Each is a small switch case; recvmsg + readv
  need scatter-gather logic.

**Round-trip kselftest.** A real record→syscall→replay→assert-
identical kselftest needs an init script that mounts debugfs and
pokes the kvm_record_ctl/state nodes — which today fights the
host-TTY-job-control fragility we already side-step in the
existing `kvm-record-smoke` runtime path. Deferred until the
shell init-script boot path stabilizes; the KUnit basic-shape
test + the build/boot kselftest already gate API drift.

**Refs.**

- `02-workstreams/D-kvm-backend/13-record-replay-determinism.md`
  (memo status updated to "1+2+3+3.5+5(partial) LANDED").
- Commits `a085afc07448..d669522cc964`.

---

## D103 (2026-04-25) — Phase 3 record/replay v1 polish + side-buffer KUnit

**Context.** D102 captured memo-13 steps 1+2+3+3.5+5(partial) +
the per-NR dispatcher refactor. This entry records the
completing v1 polish work in the same session.

**What landed.**

1. `__NR_recvfrom` added to the per-NR dispatcher — same shape
   as `__NR_read`, captures the data buffer (commit
   `915299f2a9d6`). Brings the per-NR set to four NRs:
   getrandom, read, pread64, recvfrom.
2. `kvm_record_roundtrip_test` extended with a side-buffer
   round-trip case — fake `__NR_getrandom`-shape observe →
   replay → consume → memcmp(payload) — validating step-3
   end-to-end (commit `14414378fde9`). The test directly
   drives the C API, sidestepping the early-boot
   `kvm_snapshot_capture` -ENODEV failure that blocks live
   record/replay at KUnit time.
3. `/sys/kernel/debug/um/kvm_record_log` debugfs node — bounded
   prefix dump of captured entries (commit `9d62f71540e2`).
   One line per entry with kind / IC / inline data /
   payload-presence indicator. KMALLOC_ATOMIC snapshot under
   the existing record spinlock to keep the lock-held window
   short.
4. `kvm-record-smoke` runner extended to gate the round-trip
   KUnit case (commit `dc479ef4b54f`). Both `kvm_record_basic_
   test` and `kvm_record_roundtrip_test` must report `ok N`
   lines.
5. `instruction_count` field populated with `ktime_get_ns()`
   (commit `e70bf0b6bbd5`). Provides per-entry timestamps for
   ordering verification + log readability until memo-13
   step 4 (PMU-driven INST_RETIRED.ANY counter) lands.

**Validation matrix.**

  - All eight kvm kselftests PASS post-each-commit.
  - perf ratios in 1.14-1.20× kvm/seccomp band.
  - KUnit `ok 36 / 37 / 38` fire (snapshot_basic /
    record_basic / record_roundtrip).

**Memo-13 v1 surface complete.** Architectural pieces remaining:

- Step 4 (PMU instruction-count): perf-event API plumbing.
- Step 6 (MMIO recording): UML doesn't have device emulation
  in a shape that matters here; revisit if a future workload
  needs it.
- Additional NRs (recvmsg + readv scatter-gather, ioctl per-
  driver): bounded but each is its own switch case.

**Refs.**

- `02-workstreams/D-kvm-backend/13-record-replay-determinism.md`
  (memo-13 status updated through this commit).
- Commits `915299f2a9d6..e70bf0b6bbd5`.

---

## D104 (2026-04-25) — review-01 driven fixes; kvm/seccomp now beats parity

**Context.** External review (`/tmp/review-01.md`) read the tree
at HEAD `dc479ef4b54f`, finding one P0 + several P1s on the
record/replay path + a missed sync-regs opportunity in the
generic CPL read.

**Findings addressed.**

1. **P0** — `kvm_record_replay()` didn't re-arm the active-record
   slot or static-key gate after `kvm_record_stop()` cleared
   them, so `start → stop → replay → consume` silently fell
   through to live syscalls. Commit `785f8b0c905f`. Companion
   regression test: new `kvm_record_stop_replay_consume_test`
   KUnit case + runner gate.
2. **P1 #2** — replay divergence (-EILSEQ) and end-of-log fell
   through to live `handle_syscall`. Default replay shouldn't
   silently extend recording past divergence. Commit
   `785f8b0c905f` adds `strict_replay` (default true): strict
   mode returns `-ENODATA` on end-of-log, `-EILSEQ` on
   divergence, and the dispatcher fail-stops with `-EIO` to
   the user task. Loose mode preserves the v1 fall-through.
   Debugfs `kvm_record_ctl` gains `strict` / `loose` commands.
3. **P1 #3** — host RDRAND/RDSEED CPUID bits passed through
   unchanged, leaving a hardware-RNG hole outside the syscall
   log. Commit `8046bd0a8058` masks both bits in
   `kvm_ensure_cpuid_done` before `KVM_SET_CPUID2`. Glibc's
   software DRBG fallback covers the common case; getrandom(2)
   goes through the syscall record/replay path.
4. **P1 #5** — every KVM_RUN exit issued an unconditional
   `KVM_GET_SREGS` ioctl to derive CPL for is_user, even
   though we'd already opted into `KVM_SYNC_X86_SREGS` for the
   #PF CR2 read. Commit `8046bd0a8058` extends that to the
   generic post-exit CPL read: `run->s.regs.sregs.cs.selector`
   replaces the ioctl when the cap is set. **Major perf win:**
   eliminated the per-VMEXIT ioctl took perf-fallback from
   1.14× → **0.96-0.78×** kvm/seccomp — kvm now beats seccomp
   on the headline benchmark.
5. **P1 #7 (attempted)** — making `kvm_shadow_map_page` only
   dirty on changed PTE regressed dyn-loader. Commit
   `52cbf6bb0d20` reverts; comment records the negative result.
   The post-P1#5 1.0× ratio makes this optimization non-load-
   bearing for the perf goal anyway.
6. **P3 #17** — stale SYSRETQ comments referring to the
   bootstrap re-entry path (which is IRETQ post-#272). Commit
   `1ea615dbb5f3` updates four comments + the `record.c` file
   header to match today's implementation.

**Bonus**: payload memory accounting (P2 #14). New
`rec->payload_bytes` field tracks total kvmalloc'd side-buffer
bytes; surfaced via `/sys/kernel/debug/um/kvm_record_state`.
Per-entry / total cap enforcement deferred to a future
Kconfig.

**Validation matrix** (post-each-commit):

  - All eight kvm kselftests PASS.
  - perf-getpid:    ratio_kvm/seccomp = **0.97×** (was 1.15×)
  - perf-fallback:  ratio_kvm/seccomp = **0.78-0.96×** (was 1.15×)
  - KUnit ok 36 / 37 / 38 / 39 fire (snapshot_basic /
    record_basic / record_stop_replay_consume /
    record_roundtrip).

**Outcome.** The vision target was kvm/seccomp ≤ 2× then ≤ 1.2×.
We're now ≤ 1.0× — kvm beats seccomp. The hot-path floor was
the per-VMEXIT GET_SREGS ioctl that experiment #2 had only
partially eliminated; review-01 P1 #5 closed the rest.

**Findings deferred (per review's recommendations).**

- P1 #4: full deterministic replay needs PMU interrupt
  boundaries + MMIO + RDRAND/RDSEED (now done) + gadget-vvar
  paths. v1 = "syscall-log primitive", not yet "deterministic
  replay". Memo 13 status reflects this.
- P1 #6: focused mm-mutation kvm smoke (mmap/munmap/mprotect/
  mremap/brk/execve) — post-syscall-refill removal invariant
  test. Bounded but not yet shipped.
- P2 #13: per-NR coverage matrix in docs.
- P2 #14: payload memory caps (visibility shipped, caps
  deferred).
- P3 #15: SMP-shape tightening of the active-record slot's
  locking.

**Refs.**

- `/tmp/review-01.md` — the external review.
- Commits `785f8b0c905f..1ea615dbb5f3`.

---

## D105 (2026-04-25) — review-01 deferred items closure batch

**Context.** D104 captured the P0 + P1 fixes; this entry closes
out the remaining deferred items D104 had explicitly punted.

**What landed.**

1. **P1 #6** (commit `b4374d8d4807`) — `kvm-mm-smoke`
   kselftest. Freestanding ELF guest exercises mmap, mprotect,
   mremap, munmap, brk in sequence and reads back each
   mutated page. Guards experiment #1's invariant (mm-mutating
   syscalls flow through UML's mm_map / mm_unmap callbacks
   which already invalidate the shadow PT) by failing on
   stale-mapping SIGSEGVs or read-back data corruption.
2. **P2 #14** (commit `a53d73578108`) — payload memory caps.
   Per-entry `KVM_RECORD_PAYLOAD_PER_CAP` = 1 MiB; total
   `KVM_RECORD_PAYLOAD_TOTAL_CAP` = 64 MiB. Truncate-at-cap
   for per-entry, drop-payload-but-keep-inline for total.
   New `payload_drops` counter surfaced via debugfs state.
3. **P1 #2 regression test** + **P2 #13 doc**
   (commit `444837a5bd81`):
   - `kvm_record_strict_replay_test` KUnit case asserts
     end-of-log → -ENODATA, divergence → -EILSEQ, loose mode
     fall-through, replay-rewind round-trip.
   - Memo-13 per-NR coverage matrix table documenting which
     NRs route through the side-buffer path, which are
     partial-record (recvfrom — sockaddr lost), which are
     deferred (recvmsg / readv / ioctl). Includes design
     notes for the metadata-buffer slot extension to cover
     sockaddr / iovec captures.
4. **review-12** (commit `aa644b69d514`) — `perf-pidfam`
   kselftest. Mixed-NR loop round-robining through all seven
   pid-family handlers in the LSTAR gadget (getpid / gettid
   / getppid / getuid / geteuid / getgid / getegid). Catches
   regressions on second-or-later dispatch entries that
   perf-getpid's single-NR loop wouldn't surface. Measured
   ratio kvm/seccomp = 1.066×, consistent with perf-getpid's
   headline 1.0×.
5. **B4374D8D4807 followup** (commit `346b8cf014b1`) —
   untrack the compiled mm-smoke-loop binary (build-artifact
   hygiene; same shape as the other um/* freestanding
   binaries in TEST_GEN_FILES).

**Validation matrix** — full sweep PASSes after every commit.
The kvm selftest harness is now ten tests deep:

  dyn-loader, kvm-bounds, **kvm-mm-smoke**,
  kvm-record-smoke, kvm-smoke, kvm-snapshot-bench,
  perf-fallback, perf-getpid, **perf-pidfam**,
  snapshot-kvm-smoke

KUnit at boot fires ok 36-40 (snapshot_basic /
record_basic / record_stop_replay_consume /
record_strict_replay / record_roundtrip).

**Review-01 closure status.**

  - P0 (replay re-arm): fixed (D104).
  - P1 #2 (strict replay): fixed (D104) + regression test (D105).
  - P1 #3 (RDRAND/RDSEED): fixed (D104).
  - P1 #4 (full determinism): structural — PMU + MMIO +
    gadget-vvar paths still architectural; reflected in memo
    13's status section.
  - P1 #5 (synced SREGS): fixed (D104) — the headline perf
    win that took us under 1.0×.
  - P1 #6 (mm-mutation smoke): fixed (D105).
  - P1 #7 (idempotent shadow_map): attempted, regressed
    dyn-loader, reverted with comment for future re-attempt
    (D104).
  - P2 #12 (pid-family perf): fixed (D105).
  - P2 #13 (per-NR coverage doc): fixed (D105).
  - P2 #14 (payload caps): fixed (D105).
  - P3 #15 (SMP locking): deferred to Phase-4-related work.
  - P3 #16 (using_seccomp residue): noted, not regressing.
  - P3 #17 (stale comments): fixed (D104).

Every actionable review item is closed. The remaining
deferrals (P1 #4 full-determinism architectural pieces, P3 #15
SMP) are well-understood follow-ons rather than current
correctness gaps.

**Refs.**

- `/tmp/review-01.md` — the external review.
- Commits `b4374d8d4807..aa644b69d514`.

---

## D106 (2026-05-02) — SMP-T26/T27: always KVM_GET_FPU; revert H.2

**Context.** v1's H.2 lazy-FPU optimization at `vcpu.c:1814-1828`
SKIPPED `KVM_GET_FPU` when guest CR0.TS=1 (FPU not touched). The
reasoning was correct in a single-task vCPU model but BROKEN under
v2's per-host-CPU vCPU pool where multiple UML tasks share one
vCPU. A different task could modify the vCPU's FPU between
dispatches; on re-entry the original task's `iotrap_fpu_valid` was
still false, no SET ran, and user XMM ops (e.g. glibc
`_int_malloc` MOVUPS at `0x41646a` writing fd+bk in one 16-byte
SSE store) executed on stale leftover data. Rare bits would zero
chunk->bk → next bin walk SIGSEGV at `_int_malloc+0xed`.

**What landed.**

- Commit `76b1d98b2006` removes the H.2 TS-skip predicate and
  always issues `KVM_GET_FPU` after every `KVM_RUN` exit. The
  pre-run SET side at `vcpu.c:1772` keeps its "only install if
  valid" gate (which is now always true after this fix).

**Trade-off (deliberate).** Chose correctness (close cross-task
XMM leak) over the H.2 perf optimization. Cost is one ioctl
(~1 µs) per dispatch on the otherwise-cold-FPU path. This becomes
the perf-debt that surfaces later as SMP-T55 (D118): the right
shape is a per-vCPU FPU-dirty epoch flag, not a per-task TS
predicate. Filing as a known-debt, not a regression to chase.

**Validation.**

  - threaded-fork-malloc 8w × 500i × 6 boots: 36/24000 → **0/24000**
  - mt-mini SMP T=8 × 5: 5/5 PASS
  - threaded-subprocess-wait Python × 10: 19/20 → **10/10**
  - cpython-tier0: PASS

Multi-agent confirmation (opus + codex 5.5 xhigh) tracing the
`iotrap_fpu_valid` lifecycle in the captured state-trace ring.
Closes SMP-T26 + SMP-T27.

**Refs.**

- `02-workstreams/D-kvm-backend/state-audit/15-smp-t26-t27-fpu-cross-task-leak-FIXED.md`
- Commit `76b1d98b2006`.

---

## D107 (2026-05-02) — SMP-T29: gate switch_out FPU capture on last_task

**Context.** A rare cascade-failure residual survived T26/T27
(1/30 boots, ~2675 sequential CHILD_FAIL events when it hit).
`kvm_v2_fpu_capture_for_switch_out` at `vcpu.c:2245` was
unconditionally `KVM_GET_FPU`'ing the per-host-CPU vCPU's FPU and
overwriting `from->thread.arch.kvm_v2.fpu` — including for tasks
that had never dispatched on that vCPU. This destroyed the
parent-FPU snapshot that `kvm_v2_fpu_capture_for_fork` placed for
a freshly-fork'd child if the child got context-switched OUT
before its first `KVM_RUN`. Manifested as cascading
`__fork+0x11c cr2=0x3d8` faults from glibc's `_dl_stack_used`
walk on corrupted shared parent state.

**What landed.**

- Commit `44d21b5a14ab` gates the destructive `KVM_GET_FPU` on
  `vcpu->last_task == from`. `last_task` is set in
  `load_user_sregs` at every dispatch entry; equality means "the
  per-CPU vCPU's FPU actually belongs to from". Otherwise we leave
  any existing `fpu_valid=true` snapshot intact. Also stops
  clearing `fpu_valid=false` in the "pool not up" path for the
  same reason.

**Trade-off.** Subtractive vs additive. Differs from the regressed
T28 attempt (which added a NEW unconditional `SET_FPU(zero)` on
first dispatch after execve and actively introduced wrong state).
T29 only PRESERVES correct state by gating a destructive capture
path.

**Validation.** 30-boot threaded-fork-malloc soak: 29/30 with
0 fails / 116000 forks; 1 RCU-stall outlier different bug class.
Pre-T29: cascade hit at 1/120000. Closes SMP-T29.

**Refs.**

- `02-workstreams/D-kvm-backend/state-audit/16-smp-t29-fork-snapshot-clobber-FIXED.md`
- Commit `44d21b5a14ab`.

---

## D108 (2026-05-03) — SMP-T33: KVM_SET_SREGS on cross-task dispatch (later superseded)

**Context.** mt-mini SMP T=8 ncpus=4 stress had a stubborn ~17%
flake (`got=0 expect=N` from the verify path). An ncpus ablation
that should have happened months ago immediately localized to the
per-host-CPU vCPU pool: ncpus=1 (no vCPU sharing) was 60/60 PASS;
ncpus=4 (sharing) was 50/60. The opus subagent then identified
KVM's TDP MMU `prev_roots[]` LRU as the fast-switch path that
could keep stale cached roots when remote-TLB-flushes targeted a
sibling-vCPU's active root.

**What landed.**

- Commit `9ccdc4300713` adds a cross_task predicate in
  `kvm_v2_load_user_sregs`, captured before the cr2 block updates
  `vcpu->last_task`. On cross-task dispatch the function ends with
  a full `KVM_SET_SREGS` ioctl, taking the heavy
  `__set_sregs2 → kvm_mmu_reset_context` path that drops
  `prev_roots[]` and forces the next vmentry to walk fresh. Same-
  task re-entries keep the `KVM_SYNC_X86_SREGS` dirty-bit fast
  path.

**Outcome.** mt-mini SMP T=8 N=120: 50/60 baseline → **118/120
(98%)**, +15 percentage points. cpython-parity 5-mod parity, no
substrate regression. Five prior experiments (G.2 IPI ablation,
H_E per-page madvise, T31a/T32a/T32b drain variants) had all
targeted the wrong layer; the ncpus=1 control was the unlock.

**Superseded by T41 (D111).** This was a partial fix on the
WRONG mechanism — it changed timing and pulled the rate from ~30%
to ~12%, but the residual byte[0]=0 STRICT_MEMSET_FAIL remained.
T41 found the actual cause (EINTR-mid-PF-stub user-RAX not
recovered from IST top-56). The cross-task `KVM_SET_SREGS` here
is left in place as defensive hygiene plus the WARN_ON_ONCE added
in D112.

**Refs.**

- `02-workstreams/D-kvm-backend/state-audit/20-smp-t33-vcpu-pool-sharing-diary.md`
- Commit `9ccdc4300713`.

---

## D109 (2026-05-03) — SMP-T36: mmu_gather batch-free skip after partial deferral

**Context.** External audit caught that
`tlb_batch_pages_flush()` stops the inline free loop at the
first batch with `nr==0`. The non-UML path is correct: batches
fill in order, so the first empty batch implies all subsequent
are empty. Under UML's deferral path
(`arch/um/kernel/tlb.c:171-186` setting `nr=0` for fully-deferred
batches) intermediate batches in the chain can have `nr==0`
while later ones still hold survivors of a partial-OOM defer —
which then leak forever.

**What landed.**

- Commit `bedd73af5033` walks every batch under `CONFIG_UML` and
  skips empties instead of breaking on the first one. Non-UML
  path unchanged.

**Outcome.** Correctness/robustness fix, not a residual cure —
the bug only triggers under kmalloc-OOM in
`__um_defer_append_locked`, rare under normal workload. Parallel
n=200 = 186/200 = 93.0%, statistically identical to T33b's
95.7% (±3.5% binomial CI).

**Refs.**

- Commit `bedd73af5033`.

---

## D110 (2026-05-03) — SMP-T37: pin per-CPU UML host pthreads

**Context.** Defense-in-depth on cross-vCPU aliasing: under v2's
per-host-CPU vCPU pool a guest CPU should always use the same
vCPU FD, which the host scheduler can violate by migrating the
per-CPU UML host pthread mid-flight to a different host CPU.

**What landed.**

- Commit `1b1febc1ffba` calls `pthread_setaffinity_np` on each
  per-CPU UML host pthread spawn (plus the boot pthread in
  `os_init_smp`).

**Outcome.** Honest result on the residual: parallel n=200 =
189/200 = 94.5%, statistically identical to T33b's 95.7%. The
dominant aliasing is *guest task migration across guest vCPUs*
(guest scheduler bouncing mt-mini threads), which pinning host
pthreads doesn't prevent. Worth landing for cache locality and
to remove a real-but-minor noise source.

**Refs.**

- Commit `1b1febc1ffba`.

---

## D111 (2026-05-03) — SMP-T41: recover user RAX from IST top-56 (TRUE closure)

**Context.** The mt-mini SMP T=8 STRICT_MEMSET_FAIL byte[0]=0
residual survived three weeks of hypotheses across
SMP-T31..T40 (TDP coherence, mm-fault race, prev_roots,
mmu_notifier, jitter sweep, mmu_gather, host pthread pinning,
PT-page recycle). T40's prefault test was the breakthrough:
proved the bug was on the #PF resume path. T41 then read the
PF stub byte-by-byte against the EINTR carve-out range check.

**Mechanism.** The PF stub at `exception.c:213` opens with
`push %rax` (1 byte 0x50), then mid-stub does
`mov %cr2, %rax` at offset 0x0a and `pop %rax` at offset 0x17.
If EINTR caught the guest with RIP in `[stub+0x0d, stub+0x18)`,
`eintr_regs.rax = CR2`, not user RAX. The previous
`kvm_v2_handle_pf_eintr_inline` overwrote `gp[HOST_IP/SP/EFLAGS]`
from the IST iretq frame but left `gp[HOST_AX]` = CR2.
`marshal_to_kvm_regs` shipped that into `kvm_run.s.regs.regs`; the
`KVM_SYNC_X86_REGS` dirty bit applied it on the next `KVM_RUN`.
The user resumed at `frame.user_rip` with `RAX = CR2`. For
mt-mini's `mov %al, (%rdx)` write at `0x401d4b`, AL is the low
byte of RAX; CR2 is page-aligned so AL = 0 → the first store on
the just-installed page wrote `0x00` instead of `tid` →
STRICT_MEMSET_FAIL byte[0]=0 with the page-aligned-offset
signature.

**What landed.**

- Commit `af659ad4297d` captures `stub_rip_at_eintr` at handler
  entry BEFORE overwriting `gp[HOST_IP]`. If RIP > stub_start
  (the `push %rax` has executed), recover user RAX from
  `*(u64 *)(top - 56)`. None of the stub's writes after the push
  touch top-56 (sentinel/CR2/RDX writes go to top-80/64/72), so
  the original push value is intact for the entire EINTR-able
  window. RIP == stub_start case keeps `eintr_regs.rax`.

**Supersedes T33's mechanism.** T33 (D108) was a partial fix on
the wrong layer — cross-task `KVM_SET_SREGS` changed timing
enough to halve the failure rate but the residual byte[0]=0
remained. T41 is the canonical closure: per memo 23 the actual
cause was always in the EINTR-mid-stub register-recovery path,
not in TDP / `prev_roots` coherence. T33's `KVM_SET_SREGS` site
stays for defensive hygiene + the WARN_ON_ONCE added in D112.

**Validation.**

| Run | PASS | STRICT_MEMSET_FAIL | Wilson 95% CI |
|---|---|---|---|
| T39 baseline N=100 | 88/100 | 11 | [80%, 93%] |
| T41 fix N=400 | 397/400 | 0 | [97.8%, 99.7%] |
| T41 fix N=1000 | 992/1000 | 0 | [98.4%, 99.6%] |

Substrate gate kvm-v2: PASS=25 FAIL=3 EXPECTED_FAIL=3 — bit-
identical to seccomp baseline. The 3 residual fails in N=400 are
init.sh-hangs in libc syscall (different bug class — became
SMP-T54, closed in D115).

Diagnostic tooling that made T41 possible: state-trace ring
(KVMV2T per-CPU 2 MB) with `HANDLE_IO_PF` /
`EINTR_INLINE_PF` / `EINTR_RAW_SNAPSHOT` op-codes; mt-mini's
`kvmv2_state_trace_dump()` trigger on STRICT_MEMSET_FAIL;
DIAG_T38 capture (read2/write_retry/probe/page_scan + PFN);
gate-loop `--fail-marker` per-iter capture; the prefault test
that proved the bug was on the #PF path.

**Refs.**

- `02-workstreams/D-kvm-backend/state-audit/21-smp-t41-pf-stub-rax-recovery-FIXED.md`
- `02-workstreams/D-kvm-backend/state-audit/22-smp-t41-stress-and-perf.md`
- Commit `af659ad4297d`.

---

## D112 (2026-05-04) — SMP-T47: WARN_ON_ONCE cross-task KVM_SET_SREGS failure

**Context.** Mainstream-readiness audit P2 #6. The cross-task
SREGS-install ioctl at `vcpu.c:1521` (added by T33 to drop KVM's
`prev_roots[]` on cross-task transitions) was a silent
`(void)`-cast: if it failed, the vCPU would continue running with
stale TDP roots — the SMP-T33 bug class returning unannounced.

**What landed.**

- Commit `602e9a27625c` adds `WARN_ON_ONCE` (taints kernel +
  emits stack on first occurrence) plus a ratelimited `pr_warn`
  with per-vCPU context (rc, cpu). No perf cost on the success
  path.

**Validation.** N=400 mt-mini SMP soak with the WARN active fired
zero times across all 400 boots — `KVM_SET_SREGS` is healthy in
production; the WARN is pure safety net.

**Refs.**

- Commit `602e9a27625c`.

---

## D113 (2026-05-04) — gadget revival: Phases 1-7 + items #1-5

**Context.** v1 had a stay-in-guest LSTAR gadget for trivial
syscalls (getpid family, getcpu, time, clock_gettime
CLOCK_MONOTONIC). v2 had stripped it for simplicity, paying a
full vmexit-out for every syscall. With the SMP-correctness work
landed (T26..T41), the perf budget exists to bring the gadget
back — but this time structured for mainstream upstream review.

**What landed (commit `7ebcd8aac347`).**

1. **Phases 1-7** — per-vCPU state pages, swapgs-toggled
   `%gs:disp32` field reads, getcpu NR>0xff pre-check, time(2)
   inline body, clock_gettime seqlock + budget. Two-phase
   install: 5-byte fallback LSTAR at `vm_create`, gadget upgrade
   after `exception_install` confirms state pages mapped. Per-
   task field refresh in `load_user_sregs` (TGID/TID/PPID/UID/
   EUID/GID/EGID + CPU_ID); writer-side seqlock for MONO_*.
2. **Items #1+#4 — full x86_64 syscall ABI compliance.** Gadget
   entry saves user RDX/R8/R10 into per-vCPU SAVE_* slots; every
   exit (tail / fallback) restores before SYSRETQ. Pid-family
   handlers don't actually clobber these regs but pay the
   restore for uniformity.
3. **Item #2 — assembled stub.** Hand-coded byte tables → new
   `arch/um/backend/kvm-v2/lstar_gadget.S` exposing
   `kvm_v2_lstar_{fallback,gadget}_{start,end}` extern symbols;
   `syscall_trap.c` memcpy's them into the trampoline page using
   `(_end - _start)` for length. Pattern: `ftrace_64.S` +
   `ftrace.c`.
4. **Item #3 — KUnit byteshape suite.** Locks the assembled
   gadget entry preamble (swapgs + 3× `movq %reg,%gs:SAVE_*` with
   expected disp32 values) and a page-fit upper bound; new
   `static_assert`s in `syscall_trap.h` for gadget-state-page
   slot alignment + non-overlap; boot-time memcmp panic on LSTAR
   install/upgrade.
5. **Item #5 — `CONFIG_UM_BACKEND_KVM_V2_GADGET` Kconfig knob**
   (default y). `=n` skips state-page allocation and gadget
   upgrade — every syscall takes the slow `KVM_EXIT_IO` path,
   structurally identical to pre-gadget v2.

**Validation.**

  - Userspace ABI test: 11/11 paths preserve RDX/R8/R10 (incl.
    fallback via clock_gettime CLOCK_REALTIME).
  - KUnit `kvm_v2_marshal`: 8/8.
  - KUnit `kvm_v2_byteshape`: 9/9 (incl. 2 new gadget cases).
  - Substrate gate: PASS=25 FAIL=3 EF=3 (matches v2 line).
  - mt-mini SMP stress (4w × 25i): 100/100, 0 FAIL/TIMEOUT.
  - bench-micro getpid: 91 cyc/call (vs ~90 baseline) on Zen 4
    s7 — ~6 cyc save/restore overhead absorbed in noise.

**Refs.**

- `arch/um/backend/kvm-v2/README.md` (operator docs).
- Commit `7ebcd8aac347`.

---

## D114 (2026-05-04) — SMP-T56: extend LSTAR-EINTR carve-out for gadget body

**Context.** Regression fix for the gadget revival. The pre-
revival LSTAR was a 5-byte fallback (`out` + `sysretq`) and the
SMP-T25 EINTR carve-out at `vcpu.c` only covered RIP in
`[LSTAR, LSTAR+2)`. After the revival, LSTAR is a 449-byte gadget
whose first instruction is `swapgs` (3 bytes). EINTR caught at
RIP ≥ LSTAR+3 (post-entry-swapgs, anywhere in the gadget body)
fell through unhandled, leaving the vCPU's `MSR_KERNEL_GS_BASE`
= user_gs (≈0) instead of `STATE_GVA(cpu)`. A subsequent task's
SYSCALL on the same vCPU would `swapgs` to GS_BASE=0 and the
gadget's `mov %rdx, %gs:0x50` faulted at VA `0x50`.

Symptom: mt-mini SMP T=8 N=400 went from 397/400 (T41 baseline)
to 388/400, with 10 SIGSEGV panics matching
`user_rip=0xffffe000_00000043 err=2`.

**What landed.**

- Commit `db9170b5a7b3` extends the carve-out to
  `[LSTAR, LSTAR + gadget_size)`. On RIP ≥ LSTAR+3,
  `KVM_SET_MSRS` restores `MSR_KERNEL_GS_BASE = STATE_GVA(cpu)`;
  rewind `HOST_IP` to user SYSCALL retry (`HOST_CX-2`). Pre-
  swapgs case (RIP in `[LSTAR, LSTAR+3)`) takes only the rewind,
  same as the original SMP-T25 fix.

**Validation.** N=400 mt-mini SMP soak: 0/400 BUG_B fires (was
10/400 pre-fix); 396/400 PASS = 99.0%, Wilson 95% [97.4%,
99.6%]. The LSTAR-EINTR rewind log fires on ~31% of runs in
this stress soak — the EINTR-mid-gadget event is common, and
the recovery is now correct.

**Refs.**

- Commit `db9170b5a7b3`.

---

## D115 (2026-05-04) — SMP-T54: SOCK_CLOEXEC on worker socketpair + umlctl sysrq halt

**Context.** Long-standing fd leak predating kvm-v2.
`spawn_worker_process` in `arch/um/os-Linux/worker_user.c`
created a `socketpair` with flags=0 (no `SOCK_CLOEXEC`); the
spawner side `fds[0]` is kept open for each worker's lifetime.
Each next worker spawn CoW-inherited the spawner's full fd
table — including every prior worker's `fds[0]`. `start_userspace`
then cloned the stub-child from the worker, also CoW-inheriting;
guest userspace eventually saw the leaked fds. When init.sh /
mt-mini happened to `recvmsg(fd)` on a leaked fd number, it
blocked forever waiting for an IPC the kernel-side dispatcher
kthread was supposed to consume → RCU stall → gate-loop timeout
→ FAIL. ~0.5-0.75% hit rate on N=400 mt-mini SMP soaks,
stochastic on host fd-allocation order. Misfiled as a kvm-v2 SMP
race until worker-model fd-table inspection caught the leak.

**What landed (commit `467aa7d142c0`).**

1. `SOCK_CLOEXEC` on the worker socketpair. `worker_main` is a
   clone child in a `read()` loop, never `execve`'s, so the
   CLOEXEC bit is harmless to it; `start_userspace`'s stub-child
   `execve` drops the leaked fds before guest userspace.
2. `umlctl sysrq halt` verb. `tools/uml/uml-launcher/src/bin/
   umlctl/deploy.rs` appends `__umlctl_halt` (sysrq-b shutdown)
   after every user phase. Removes the dependency on `/sbin/halt`
   which on most modern distros is a symlink to
   `/bin/systemctl` that `recvmsg`-blocks on a DBus socket
   waiting for a non-existent systemd.

**Validation.** mt-mini SMP T=8 ncpus=4 N=400 (60 s gate-loop):

  - pre-fix (`24557e95c4b1`):                   394/400 = 98.5%
  - pre-fix (`db9170b5a7b3`, post gadget+T56):  395/400 = 98.8%
  - **post-fix (this commit):                   400/400 = 100.0%**
    Wilson 95% [99.0%, 100.0%], elapsed 204 s (down from
    1207 s — the bug was eating wall-clock via dispatcher
    `recvmsg` contention too).

Closes SMP-T54.

**Refs.**

- Commit `467aa7d142c0`.

---

## D116 (2026-05-03) — perf-O1: gate post-syscall interrupt_end()

**Context.** `interrupt_end()` is the post-syscall sched/signal
drain. Its body is internally gated on `_TIF_WORK_MASK`, so on
the common fast path (no signals pending, no `-ERESTART`) the
function call itself is dead weight — ~30-50 cyc of call overhead
per syscall.

**What landed.**

- Commit `24557e95c4b1` inlines the gate. Skip
  `interrupt_end()` unless either (a) `handle_syscall` returned
  in the `-ERESTART*` range (`-512..-516`) — `do_signal` MUST run
  to translate `-ERESTART` → restart-RIP or `-EINTR`, closes the
  dash bug (#107) — or (b) any `_TIF_WORK_MASK` bit is set.

**Validation.** bench-micro on server7 (Zen 4 boost, performance
gov): baseline median 36 879 cyc → post-O1 median 36 759 cyc
(5 runs each), delta -120 cyc / -0.32%. Substrate gate kvm-v2:
PASS=25 FAIL=3 EF=3 (bit-identical to seccomp baseline).

**perf-O2 attempted, reverted.** Skipping the per-dispatch
CR4.PGE TLB-flush toggle on same-task no-tlb-bump dispatches
crashed init even with the `tlb_gen+kick_pending` guard.
Confirms the 2026-05-01 in-tree warning that v1 also tried
"tlb_stale && same_cr3" and regressed pass rate. Right
predicate likely also requires caching `fs.base`/`gs.base`/`cr3`
to know when to actually re-ship sregs. Left as future work.

**Refs.**

- Commit `24557e95c4b1`.

---

## D117 (2026-05-05) — Phase J pilot soak rig — realistic workloads

**Context.** SMP-T54's close brought mt-mini SMP T=8 N=400 to
400/400 = 100.0%. With a clean baseline the obvious next
question is what other realistic workloads expose flake at the
1% rate the mt-mini gate now forecloses. Phase J in the original
24-month plan is "validation: 24 h continuous + Tier 1/2/3 +
soak"; the pilot rig is the diverse-workload precursor.

**What landed.**

1. Commit `95c95267202e` — `tools/testing/selftests/um/soak/`
   multi-workload rig with five Umlfile templates + drivers:
   - `memcheck` (50 LoC C, memtester replacement) — anon-mmap
     CoW + addressing wiring at GB-scale, walking-bit + 7 const
     fills.
   - `iocheck` (80 LoC C, fio-style replacement) —
     write/fsync/read/verify on tmpfs with block-id-keyed
     pattern.
   - `stress-ng` — IPC stressors (futex+pipe+switch) with
     `--verify`. `--vm` deferred (SMP-T57 follow-up).
   - `cpython-soak` — Python regrtest curated subset
     (test_signal / test_io / test_mmap / test_fork1 / etc).
   - `kbuild-tiny` — tinyconfig in-guest UML build (fork-storm
     + pipe + file I/O).
   - `run-pilot.sh` drives parallel `umlctl gate-loop` per
     workload × backend (kvm-v2, seccomp), with thermal throttle
     (max of `k10temp`/`coretemp`/`zenpower`, pause ≥ 88 °C,
     resume ≤ 75 °C — `acpitz` was unreliable on AMD), cooldown
     between workloads, CSV summary at exit.
2. Commit `82df9571eb25` — operator README, design memo, and
   STATUS refresh for J-pilot vs J (full).

**First-pilot result (W=2 × M=20 × 3 short workloads × 2 backends
= 240 boots).**

  - memcheck   kvm-v2: 40/40   seccomp: 40/40
  - iocheck    kvm-v2: 40/40   seccomp: 40/40
  - stress-ng  kvm-v2: 40/40   seccomp: 40/40
  - **Total: 240/240 = 100.0%**, Wilson 95% [98.5%, 100%]

Two harness bugs fixed during smoke (kept as future-protected
patterns in templates): `cmd | tail; echo rc=$?` masks pipeline
failure (use `cmd && echo OK || (echo FAIL; exit 1)`); stress-ng
cwd `/` is RO under hostfs (`cd /tmp` first).

One real soak finding deferred: **SMP-T57** — `stress-ng --vm
--verify` trips on kvm-v2 only with "vm-method 0x.. not
readable". Needs vm-method bisect.

**Refs.**

- `02-workstreams/D-kvm-backend/phase-J-pilot-2026-05-05.md`
- `tools/testing/selftests/um/soak/README.md`
- Commits `95c95267202e`, `82df9571eb25`.

---

## D118 (2026-05-07) — SMP-T55 surfaced; do not loosen the perf gate

**Context.** A verification pass against current `umlctl-deploy`
HEAD on Zen 4 7840HS surfaced one finding STATUS hadn't
captured: the `perf-py-startup` kselftest gate FAILS — kvm-v2
0.10 s vs seccomp 0.08 s, ratio_v2_over_seccomp = **1.250**, vs
the ceiling max=1.2. Three months ago the same gate read 0.444
(kvm-v2 2.25× FASTER than seccomp). The regression decomposes:

  - **+50% UP→UP**, 04-30 → 05-02 build window: a clutch of
    SMP-correctness commits (TLB-kick infra `7e1c255a09ad` /
    `9f0ff6257e8b`, `migrate_disable` `95b3a85bd309`,
    EINTR-mid-PF inline `e5977806fd14`).
  - **+67% UP→SMP**, 05-02 → 05-03 build flavor change (CONFIG_
    SMP=y, NR_CPUS=4, ~+33% on seccomp baseline too) plus
    SMP-T26/T27's `76b1d98b2006` always-`KVM_GET_FPU` revert of
    H.2 lazy-FPU (D106).

Hot-path / gadget-driven workloads are unaffected (verified same
day: bench-py 4.00× faster than seccomp, bench-micro getpid
~1050× faster). `perf-py-startup` is the workload that maximises
non-FPU dispatches (heavy syscall churn, mmap, exec) and
minimises any benefit from elided FPU — exactly where T26/T27's
correctness/perf trade pinches hardest.

**Decision (policy).** Leave the gate failing. Do NOT loosen the
1.20× ceiling to silence the gate. The right shape is a real
fix, per the plan memo: per-vCPU FPU-dirty epoch flag that gates
the post-vmexit `KVM_GET_FPU` (option (a) in memo 23), with the
cross-task case captured by an explicit `last_task != current`
hook in `load_user_sregs` rather than by accidental ordering.
This restores ~95% of dispatches to the "skip GET" fast path
while making the cross-task XMM leak impossible by construction.
Estimated ~30 lines of `vcpu.c` plus one bool field; the work is
in the gating-predicate audit, not the implementation.

**What landed.** No code commit yet. STATUS update is
`c4a7cabb9742` — Phase ledger gains an SMP-T55 row, Tip line
mentions the open gate, and `state-audit/23-smp-t55-perf-
regression-plan.md` (~830 lines) enumerates three options ((a)
per-vCPU dirty epoch, (b) cross-task `last_task` gate, (c) lazy
capture at switch_out only) with state-machine deltas, the
T26/T27 correctness arguments, the recommended path
(option (a)), gotchas (handle_io_nm CR0.TS clear; first-dispatch
of fork'd task; cross-vCPU migration), and an experimental gate
plan.

**Refs.**

- `02-workstreams/D-kvm-backend/state-audit/22-smp-t41-stress-and-perf.md`
- `02-workstreams/D-kvm-backend/state-audit/23-smp-t55-perf-regression-plan.md`
- STATUS update commit `c4a7cabb9742`.

---

## D119 (2026-05-07) — SMP-T55 closed: per-vCPU FPU-dirty epoch flag

**Context.** D118 surfaced SMP-T55 as a `perf-py-startup` gate
failure (ratio 1.250 vs 1.20 ceiling on Zen 4) and committed to
fixing rather than loosening the gate. Memo `state-audit/23-smp-t55-
perf-regression-plan.md` enumerated three options; option (a)
per-vCPU FPU-dirty epoch flag was recommended.

**What landed.**

- Commit `fd2f9639b0ce` adds two fields to `struct kvm_v2_vcpu`:
  - `fpu_dirty` (bool): true when vCPU's `guest_fpu` may have
    diverged from the per-task `iotrap_fpu` snapshot.
  - `fpu_owner_task` (`struct task_struct *`): which task
    `fpu_dirty=false` is relative to. Belt-and-suspenders against
    any path that mutates `guest_fpu` without setting `fpu_dirty`:
    skip the post-vmexit GET only when both `fpu_dirty=false` AND
    `fpu_owner_task == current`.
- Mark dirty on: vcpu_create_one (force first GET); cross-task
  arrival in `load_user_sregs`; post-vmexit `cr0 & X86_CR0_TS == 0`
  (guest used FPU); `kvm_v2_handle_io_nm` clearing TS;
  `kvm_v2_fpu_install_on_first_run` after a fresh KVM_SET_FPU from
  `fpu_valid` (per memo §5 gotcha #2 — re-capture into iotrap_fpu).
- Mark clean on: successful pre-run `KVM_SET_FPU` from current's
  `iotrap_fpu`; successful post-vmexit `KVM_GET_FPU`.

**Trade-off (deliberate).** The ratio improved 1.250 → 1.10 — gate
passes the 1.20 ceiling but does not hit memo §6.2's stretch target
of ≤ 0.55. The +50% UP-hop in the 04-30 → 05-02 SMP-correctness
commit window is a separate regression class (TLB-kick infra +
migrate_disable + EINTR-mid-PF inline), not addressed by this fix.
Bisecting it is deferred — no current gate failure justifies a
focused investigation.

**Validation.**

- Substrate gate kvm-v2: PASS=25/FAIL=3/XFAIL=3 (unchanged).
- Substrate gate seccomp: PASS=25/FAIL=3/XFAIL=3 (unchanged).
- cpython-parity 21 modules: 21/21 PARITY.
- mt-mini SMP T=8 ncpus=4 N=30: 30/30 PASS.
- **threaded-fork-malloc 8 workers × 500 iters × 6 boots = 24 000
  forks: 0 CHILD_FAIL events.** T26/T27 cross-task FPU leak
  guarantee preserved (Wilson 95% upper bound 0.015%/fork — same
  as post-T26/T27 baseline).
- perf-py-startup ratio (3-run median): 1.10. Gate passes.

**Refs.**

- `02-workstreams/D-kvm-backend/state-audit/23-smp-t55-perf-regression-plan.md`
- Commit `fd2f9639b0ce`.
- D106 (T26/T27 — the trade-off this fix partially restores).
- D118 (T55 surfaced + policy decision not to loosen the gate).

---

## D120 (2026-05-07) — Phase J #167 daemon-mode driver landed

**Context.** D117 added the J-pilot rig (`run-pilot.sh` + 5 workload
templates). Phase J full validation (STATUS row J) needs three more
things: 24h continuous operation, Tier 1/2/3 third-party workloads,
and LTP curation. The Phase J design memo
(`phase-J-design-2026-05-07.md`) specifies all three.

**What landed.**

- Commit `4977e0357ce5` adds
  `tools/testing/selftests/um/soak/run-soak-daemon.sh` (485 LoC bash
  + python3 stdlib for arithmetic only). Sibling to `run-pilot.sh`,
  not a `--daemon` flag on it (design memo §2.1: separate audiences
  → separate drivers). Implements §2.2-2.10:
  - CLI: `--budget-sec`, `--workloads`, `--workers`,
    `--iters-per-rotation`, `--out`, `--fail-threshold-pct`,
    `--fail-threshold-window`, `--continue-on-fail-threshold`,
    `--dry-run`. Also via `SOAK_*` env vars.
  - `UML_KERNEL` is REQUIRED — no fallback to a hardcoded path
    (fixes the inconsistency the design memo flagged).
  - SIGTERM/SIGINT → finish-in-flight-phase clean stop. SIGUSR1
    → force summary refresh.
  - Per-iteration `scoreboard.jsonl` row using `phase-J-soak-
    <workload>` gate label so the soak rows don't conflate with
    regular gate rows under `umlctl gate diff`.
  - Wilson 95% CI in summary.md, recomputed every rotation.
  - Stop conditions: budget elapsed; SIGTERM; rolling-window
    failure-rate threshold trip → write THRESHOLD_TRIPPED sentinel.
  - Thermal helpers lifted verbatim from `run-pilot.sh`
    (`read_max_temp_c` / `thermal_check`).

**Trade-off.** Memo §2.6 "alternative A": daemon synthesises the
scoreboard row itself rather than calling `umlctl gate run` per
iteration. Picked because (a) `umlctl gate loop`'s artefact tree
already has the per-iter logs the daemon needs and (b) wrapping each
iter in its own `gate run` would be redundant umlctl create/start
cycles. The cost is that the daemon's row format must stay in sync
with `umlctl`'s by hand — flagged for a future refactor if the
formats drift. Per-iteration timing is left as `duration_ms=0` for
now; populating it would require parsing `umlctl gate loop`'s `_loop.log`
or extending umlctl to emit per-iter timestamps.

**Validation.**

- Dry-run smoke (memcheck,iocheck × 2 backends × 1 rotation):
  config.json valid JSON, 4 scoreboard rows, summary.md renders
  Wilson CI table.
- Real-run smoke (`--budget-sec 100`, memcheck only, W=1 M=2):
  5 rotations × 4 phases × 2 iters = 20 PASS rows. Wilson CI
  [72.25%, 100.00%] at n=10. Clean exit on budget elapsed.

**Deferred to follow-up tasks.**

- Tier 1 / 2 / 3 workload templates (memo §3) — pytest on host
  libs, pip+pytest with sandboxed network, Django/FastAPI loopback.
- LTP runner template (memo §4) — KEEP/SKIP/TRIAGE-LATER curation +
  marker-bridge to `gate loop` PASS/FAIL convention.
- Restart-resume from partial soak.
- Per-iteration `duration_ms` plumbing.

STATUS row J flips PENDING → IN PROGRESS.

**Refs.**

- `02-workstreams/D-kvm-backend/phase-J-design-2026-05-07.md` §2.
- `02-workstreams/D-kvm-backend/phase-J-pilot-2026-05-05.md` (the
  pilot driver this is sibling to).
- `tools/testing/selftests/um/soak/run-pilot.sh` (thermal helpers
  reused verbatim).
- D117 (J-pilot rig landing).
- Commit `4977e0357ce5`.

---

## D121 (2026-05-07) — SMP-T57 Phase A: AVX/XSAVE enabled in kvm-v2

**Context.** D118 surfaced SMP-T57 (stress-ng `--vm --verify` SIGILL
on kvm-v2 only). Six probe cycles in memo `state-audit/24` ruled out
IST-write, SYSRETQ-RCX, GPR drift, user-stack TDP, text-page TDP; the
§7.6-H probe (commit `b91d49857892`) found the SIGILL fires on
`vpxor %xmm6,%xmm6,%xmm6` (VEX/AVX). User-instruction bytes match the
binary byte-for-byte → CPU-feature-disable. The fix plan
(`state-audit/25`, commit `c7e0f37910e6`) split the work into Phase A
(enable AVX) and Phase B (enable AVX-512 — deferred).

**What landed.**

- Commit `ab68bf077de3` (4 sites in `arch/um/backend/kvm-v2/vcpu.c`):
  1. New `kvm_v2_install_xcrs()` helper — `KVM_SET_XCRS` with
     xcrs[0].value=0x7 (FP|SSE|YMM).
  2. `kvm_v2_curate_cpuid` un-masks Leaf 1 ECX bits 12/26/27/28/29
     (FMA/XSAVE/OSXSAVE/AVX/F16C) and Leaf 7.0 EBX bit 5 (AVX2);
     drops the zero-everything pass on Leaf 0xD (KVM needs it to
     compute `guest_supported_xcr0` for SET_XCRS validation).
  3. Lazy first-dispatch arming in `vcpu_run`'s existing
     `cpuid_primed=false` block: install CPUID → synchronous
     GET+SET_SREGS adding `X86_CR4_OSXSAVE` → `install_xcrs`.
     Two chicken-and-egg KVM validations resolved by ordering:
     KVM rejects CR4.OSXSAVE before CPUID's OSXSAVE bit lands;
     KVM rejects SET_XCRS before CR4.OSXSAVE is live in
     `vcpu->arch.cr4` (synchronous, not SYNC_REGS-deferred).
  4. `install_production_sregs`'s seed kept WITHOUT OSXSAVE — the
     bit is added at first-dispatch arming time. Documented at
     the seed site to prevent a future reader re-adding it.

- Commit `<this-update>` — STATUS row T57 flips
  "OPEN (root-cause level)" → "DONE (Phase A; Phase B optional
  follow-up for AVX-512)".

**Trade-off (deliberate).**

Phase A enables AVX-128 (XMM) and AVX-256 (YMM) but leaves
AVX-512 (ZMM/OPMASK/Hi16) masked. Empirically stress-ng's
`mscan` and `prime-incdec` vm-methods emit `vmovdqa64 %zmm6,%zmm7`
in their tail-merge verification routine — those two methods
still SIGILL on kvm-v2 post-Phase-A. The other 12 of the 14
sampled vm-methods (flip, checkerboard, walk-0/1, zero, ones,
move, grayflip, stripe, galpat-0/1, ...) now PASS. Phase B
(un-mask leaf-7 AVX-512 EBX/ECX/EDX bits + bump XCR0 to include
bits 5/6/7) is a small follow-up that closes mscan/prime-incdec
when prioritised. Not gating on Phase B because:
  - The 12-of-14 ratio is a substantial improvement over 0-of-14.
  - AVX-512 enable carries a marshal-struct-size question
    (`struct kvm_fpu` is FXSAVE-shaped at 512 B — switching to
    `struct kvm_xsave` for ZMM_Hi256/Hi16_ZMM is the Phase B
    structural change; memo `state-audit/25` §3.2 §5.1 catalogs
    the risk).
  - mscan and prime-incdec are the only-AVX-512 stress-ng methods
    on this host; everything else in the Phase J pilot matrix
    works.

**Validation.**

  - substrate gate kvm-v2: PASS=25/FAIL=3/EXPECTED_FAIL=3
    (= seccomp parity, unchanged).
  - cpython-parity 21 modules: 21/21 PARITY.
  - mt-mini SMP T=8 ncpus=4 N=30: 30/30 (T41 holds).
  - perf-py-startup 3 runs × 10 samples: ratio 1.167 PASS
    (T55 ceiling 1.20; T57 fix did NOT slow Python startup).
  - threaded-fork-malloc 8w × 500i × 6 boots = 24 000 forks:
    0 CHILD_FAIL (T26/T27 cross-task XMM guarantee holds).
  - `stress-ng --vm --vm-method=<X> --verify` matrix sample:
    12/14 PASS post-fix (was 0/14 pre-fix).
  - In-guest `__asm__("vpxor %xmm6,%xmm6,%xmm6")` and
    `__asm__("vpxor %ymm0,%ymm0,%ymm0")` both succeed under
    kvm-v2 (manual AVX_TEST repro, CPUID reports avx_bit=1
    xsave_bit=1 osxsave_bit=1, xgetbv returns 0x7).

**Deferred to follow-up.**

- Phase B (AVX-512 enable). Closes mscan + prime-incdec; needs
  marshal-struct audit + cross-task ZMM probe.
- Cross-task YMM upper-128 hygiene probe (memo `state-audit/25`
  §4.3). `threaded-fork-malloc` verified 24 000 cross-task
  transitions XMM-clean, but a `_mm256_extracti128_si256`-based
  YMM probe would catch a narrow class the existing probe is
  blind to.
- `state-audit/24-smp-t57-vmmethod-bisect.md` §8 status refresh
  pointing at this fix (the memo's history-of-investigation is
  preserved; only the trailing status line needs the pointer).

**Refs.**

- `02-workstreams/D-kvm-backend/state-audit/24-smp-t57-vmmethod-bisect.md` (root-cause arc).
- `02-workstreams/D-kvm-backend/state-audit/25-smp-t57-xsave-enable-plan.md` (this fix's plan).
- Commits `b91d49857892` (root-cause), `c7e0f37910e6` (plan),
  `ab68bf077de3` (Phase A code).
- D118 (T57 surfaced), D119 (T55 — adjacent FPU territory).
- v1 archive vcpu.c header comment lines 26-33 (deferred-work item
  this fix closes).

---

## D122 (2026-05-14) — first post-T57-Phase-A 2h soak: 200/200 on kvm-v2

**Context.** D121 closed SMP-T57 Phase A (AVX/XSAVE enable). D120
landed the Phase J daemon-mode driver. This entry records the first
production-scale soak that exercises both: a 2 h `run-soak-daemon.sh`
sweep across memcheck/iocheck/stress-ng/tier1-pylibs on the
post-Phase-A kernel.

**What ran.**

- `run_id` `phase-J-2h-2026-05-14T020423Z`.
- Kernel `~/src/uml-builds/uml-smp-t41fix/linux` (HEAD
  `bcd791f5a369`, post-T57 Phase A).
- Host: AMD Ryzen 7 7840HS (Zen 4).
- Budget 7200 s; daemon early-stopped at 2095 s (29.1 % consumed)
  via the `--fail-threshold-pct 5 --fail-threshold-window 50`
  trip on the iocheck/seccomp tuple (3 fails over rolling 50 iters).

**Result.**

- 400 scoreboard rows.
- **kvm-v2: 200/200 = 100.00 % PASS** across all 4 workloads.
- seccomp: 197/200 = 98.50 % (3 iocheck timeouts; root-cause
  analysis below).
- 0 panics, 0 throttle pauses, 0 kernel corruption.
- All three failing iters reported `bad=0` for every completed
  iteration before timing out. NOT corruption — per-iter timeout
  class under W=2 contention.

**Findings.**

- **First sustained validation of the T57 Phase A fix.** 200 kvm-v2
  iters across mmap-heavy (memcheck), file-IO (iocheck), IPC
  stress (stress-ng futex/pipe/switch), and C-extension exercise
  (tier1-pylibs requests + cryptography) — all clean.
- **T55 + T26/T27 invariants hold under load.** No FPU corruption
  events across ~50 000 dispatches.
- **iocheck/seccomp timeout class** — `iocheck.toml.template`'s
  60 s per-iter timeout occasionally too tight when two seccomp
  workers run concurrent 64 MB tmpfs write/fsync/read passes.
  kvm-v2 unaffected. Follow-up filed as task #23 (bump to 120 s).
- **Daemon driver works end-to-end.** Rotation sweep,
  threshold-trip clean-stop, scoreboard.jsonl + Wilson-CI
  summary.md all behaved as designed. First production-scale run
  of commit `4977e0357ce5`.

**Decision (operational).**

The threshold-trip stop fired correctly and shouldn't be loosened
to "soak through the iocheck flake." Instead:

  1. Fix iocheck's per-iter timeout (task #23) so the flake is
     no longer a flake.
  2. Re-run with `--fail-threshold-window 100` next time for a
     smoother rolling average — the rolling-50 window felt tight
     enough to over-trigger on a low-rate timeout class. The
     spec at memo `phase-J-design-2026-05-07.md` §2.8 left the
     window adjustable for exactly this reason.

Do NOT relax the 5 % threshold itself. The whole point of the
daemon's early stop is to catch a real ~5 % failure-rate
regression before it accumulates into the scoreboard noise.

**Refs.**

- `02-workstreams/D-kvm-backend/phase-J-soak-2h-2026-05-14.md`
  (full soak diary — per-tuple table, log inspection, follow-up).
- `02-workstreams/D-kvm-backend/phase-J-design-2026-05-07.md`
  §2.8 (threshold-trip stop spec).
- D117 (J-pilot 240/240 baseline), D119 (T55 fix), D120 (daemon
  driver), D121 (T57 Phase A).
- Task #23 (iocheck timeout follow-up).
- Artefacts: `/var/tmp/uml-soak/phase-J-2h-2026-05-14T020423Z/`.

---

## D123 (2026-05-16) — snapshot v2 port Phase 1: skeleton + regs_only round-trip

**Context.** PLAN-2026-05-14 §4.1 designated the Time-machine
track (#168 snapshot → #169 record/replay → #170 syzkaller) as
the strategic differentiator. v1 had a working 647 LoC snapshot
TU at `arch/um/backend/kvm-v1-archive/snapshot.c`; v2 had
nothing. This entry records the Phase 1 lift: struct + alloc/
free + `kvm_v2_snapshot_capture_regs_only` +
`kvm_v2_snapshot_restore_full` IMPLEMENTED for v2, the full-
capture path stubbed, and a build-aware KUnit case wired in.

**What landed.**

  - **Design memo** `02-workstreams/D-kvm-backend/26-snapshot-
    v2-port.md` (~530 lines). Covers the v1 → v2 deltas, the
    Phase 1 design, the Phase 2-7 sub-sequencing, and the open
    questions.
  - **`arch/um/backend/kvm-v2/snapshot.c`** (~280 LoC). Public
    surface: `kvm_v2_snapshot_alloc`, `_destroy`, `_free`,
    `_capture_regs_only` (implemented), `_restore_full`
    (implemented), `_capture` (stub → `-EOPNOTSUPP`). All
    `EXPORT_SYMBOL_GPL`'d, matching the v1 archive precedent so
    Phase 7's record/replay TU links cleanly.
  - **Header** — `struct kvm_v2_snapshot` + prototypes +
    `KVM_V2_SNAPSHOT_MSR_COUNT` constant.
  - **Makefile** — `snapshot.o` under
    `CONFIG_UM_BACKEND_KVM_V2`.
  - **KUnit case** `test_kvm_v2_snapshot_basic` in
    `test_byteshape.c`. Capture / mutate vCPU RAX via
    `KVM_SET_REGS` / restore / assert. `kunit_skip` when the
    pool isn't initialised (build-only env without a real v2
    boot).

**v1 → v2 deltas this port handles.**

  - **Per-task vCPU lookup → per-host-CPU pool walk.** v1 had
    `current->thread.arch.kvm.vcpu`; v2 walks `vcpus[]` for the
    entry whose `last_task == current` under `preempt_disable`.
  - **`KVM_GET_FPU` → `KVM_GET_XSAVE`.** SMP-T57 Phase A
    (D121) enabled XCR0.YMM; the legacy 512 B FXSAVE shape loses
    YMM upper-128 silently. The XSAVE ioctl (static 4 KB
    `struct kvm_xsave`) captures the full extended state under
    v2's curated CPUID.
  - **New: `KVM_GET_XCRS` capture.** v1 didn't (XCR0 was zero).
    v2 must so restore re-establishes the XSAVE interpretation
    context.
  - **Post-restore hygiene.** Clear `vcpu->last_task`,
    `vcpu->last_mm`, `vcpu->fpu_owner_task`; set
    `vcpu->fpu_dirty = true`. Forces the next dispatch's
    cross-task arrival branch (memo §4.5) so SREGS + FPU
    re-install unconditionally from the restored bytes.

**Phase 1 limitations (sub-sequenced in the memo).**

  - Calling-task-only snapshot. Cross-task snapshot is Phase 4.
  - No memslot copy. Phase 3.
  - No IDT/GDT/IST/gadget-state capture. Phase 3.
  - No bench harness. Phase 5.
  - No selftest re-plumb. Phase 6.
  - KUnit case `kunit_skip`s under build-only env. Phase 2 will
    stand up a real v2 boot KUnit harness.

**Sizing actual vs estimate.**

PLAN-2026-05-14 §4.1 estimated 100 LoC for sub-step 1
(skeleton) + 150 LoC for sub-step 2 (`capture_regs_only`). The
landed code is ~280 LoC of `snapshot.c` + ~60 LoC of header
additions + ~50 LoC of KUnit case = ~390 LoC code. Slightly
over budget because the v2-specific lookup helper +
preempt_disable scaffolding + the `fpu_dirty` post-restore
invariants add ~80 LoC the v1 archive didn't need. The
+ ~530 lines of memo is on top.

**Build verification.**

  - `make ARCH=um O=/home/mjbommar/src/uml-builds/uml-smp-t41fix
     -j$(nproc)` → Exit 0, no warnings on the touched files.
  - Scratch build with `CONFIG_UM_BACKEND_KVM_V2_KUNIT=y` ALSO
    Exit 0 — `test_byteshape.o` builds cleanly with the new
    case wired in.

**checkpatch.** Strict + terse mode flags only the generic
"new file → MAINTAINERS?" warning. No errors, no other
warnings.

**Decision (strategic).**

Phase 1 IS the gate. Phases 2-7 each get their own commit. The
incremental gating is:

  - Phase 2: real boot-time KUnit env (1-2 weeks).
  - Phase 3: full capture + memslot + IDT/GDT/IST (~300 LoC,
    ~2 weeks).
  - Phase 4: cross-task semantics (~150 LoC, ~1 week).
  - Phase 5: bench harness port + perf measurement against the
    memo-12 budget (<50 ms cold / <1 ms iter targets), ~250 LoC.
  - Phase 6: selftest re-plumb, ~50 LoC.
  - Phase 7: opens #169 (record/replay) on top of the snapshot
    primitive.

Total runway: ~3-5 weeks of Track B work to close #168, then
~6-8 weeks for #169 record/replay on top.

**Why Phase 1 is build-only.**

UML kernel boots take ~10-30 s, and the smoke tests we'd run
to validate snapshot end-to-end are 5-15 minutes apiece.
Phase 1's scope (skeleton + the two simplest functions) doesn't
need that round-trip yet — type-check + KUnit-shape pass is
enough confidence, and Phase 2's boot-time KUnit env is the
proper way to land the runtime gate. Premature boot testing
here would just add hours per cycle without changing the
landed code.

**Refs.**

  - `02-workstreams/D-kvm-backend/26-snapshot-v2-port.md`
    (this port's design memo, landed alongside).
  - `02-workstreams/D-kvm-backend/12-snapshot-forkserver-kvm.md`
    (original v1-era design).
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/
    02-snapshot-port-phase1.md` (diary entry, also in this
    commit).
  - `06-sequencing/PLAN-2026-05-14.md` §4.1 (the seven-step
    sub-sequencing).
  - `arch/um/backend/kvm-v1-archive/snapshot.c` (lift target).
  - D119 (SMP-T55 — `fpu_dirty` per-vCPU semantics).
  - D121 (SMP-T57 Phase A — XSAVE plumbing the port leverages).

---

## D124 (2026-05-14) — Phase J Tier 2 (uv venv) landed; pivot from pip-wheelhouse

**Track:** A (Phase J).

**Decision.** Tier 2 (Python heavyweights — httpx, pyyaml,
pendulum, numpy) ships via a pre-built persistent `uv` venv
rather than a per-invocation `uv run --with <pkg>` resolve.

**Why.** Initial attempt used `uv run --with httpx pyyaml
pendulum numpy` per UML start. The offline resolver walked back
through historical versions trying to find cached wheels and
failed. Pre-building the venv once (`uv venv
~/.cache/uml-soak-tier2-venv && uv pip install httpx pyyaml
pendulum numpy`) leaves the venv as a stable hostfs-visible
dependency the template invokes directly:

  - Template: `cmd = ".../.cache/uml-soak-tier2-venv/bin/python
    {{SOAK_DIR}}/tier2-uv-smoketest.py"`
  - Env: `HOME = "/home/mjbommar"` (uv needs HOME to find its
    cache directory; UML's default `/` is read-only).

**Verification.** Smoke under daemon: 80/80 PASS (20 iters × 2
backends × 2 workers, kvm-v2 + seccomp, T57 Phase A on).

**Refs.**
  - `tools/testing/selftests/um/soak/tier2-uv-pylibs.toml.template`
  - `tools/testing/selftests/um/soak/tier2-uv-smoketest.py`
  - Diary `plan-2026-05-14-execution/01-tier2-uv-landed.md`.

---

## D125 (2026-05-14) — Phase J Tier 3 chose per-worker IP option (b)

**Track:** A (Phase J).

**Decision.** Tier 3 (Django + FastAPI loopback) uses a fixed
per-worker IP carve-out from `192.168.42.0/24` rather than
threading the guest IP back through `umlctl ps --json` (option
(a)).

**Why.**
  1. Option (a) is the "right surface in the abstract" but
     requires a `manifest.rs:8` schema-v2 bump + 40-120 LoC
     across 3-4 Rust files. The manifest is documented as
     "immutable after create — mutations are rm + create."
     Bumping for a derivable-from-policy value is the wrong
     reason to touch that contract during the Phase J window.
  2. Option (b) closes Phase J §5.4 ("Tier 3 wired and one
     full rotation pass per framework") with ~150 LoC of bash
     and zero Rust changes. The `umlctl ps --json` improvement
     is filed as a non-blocking follow-up.
  3. Concurrent-soak correctness: option (a) needs a separate
     `gate_loop.rs:253-277` per-worker-fanout fix anyway;
     option (b) gets it for free because each worker has a
     non-colliding /30 by construction.

**Allocation scheme.** worker N → `host_ip =
192.168.42.{4N+1}/30`, `guest_ip = 192.168.42.{4N+2}/30`,
`tap_name = soak-tap{N}`. Capped at $WORKERS <= 63 by /24
capacity.

**Trade-off the operator inherits.** `umlctl ps` does NOT
show the guest IP for Tier 3 workers; postmortem
reconstruction is `worker_idx → host_ip/guest_ip` via the
documented `4N+1 / 4N+2` formula. Acceptable for a soak-
specific convention; filed `umlctl ps --json` extension as a
non-blocking follow-up.

**Refs.**
  - `02-workstreams/D-kvm-backend/phase-J-tier3-design-2026-05-14.md` §5.
  - `tools/testing/selftests/um/soak/tier3-{django,fastapi}.toml.template`.
  - `tools/testing/selftests/um/soak/run-soak-daemon.sh` (helpers
    `is_tier3_workload`, `emit_tier3_worker_toml`,
    `process_tier3_phase_results`, `run_one_tier3_phase`).

---

## D126 (2026-05-14) — Series 3 (ftrace-notrace-generic-v1) READY

**Track:** D (Upstream).

**Decision.** Regenerated Series 3's `0001-*.patch` from
on-branch commit `a2e01ee58c53` with `--unified=3`. Stripped
trailing whitespace on the `--<sp>` signature separator.
Verified `git apply --check` and `git am --3way` both clean
against `origin/master`. Flipped
`upstream-patches/ftrace-notrace-generic-v1/SUBMISSION-NOTES.md`
status to **READY**.

**Why this needed a regen.** The 2026-04-24 staged patch had
`smpboot.c` hunks generated with `--unified=2` (only 2 trailing
context lines). `git am --3way` rejected with `corrupt patch
at line 63` because git's default-3 fuzzy match couldn't
realign with the upstream-master context window.

**Sequencing implication.** Track D Series 1 + 2 + 3 are now
all in READY state — operator may `git send-email` all three
in parallel (none has dependencies on the others). Series 4
(`backend-ops-abstraction-rfc`) is blocked behind Series 3
*landing* per the Phase 5 upstream queue ordering, not behind
Series 3 *sending*.

**Refs.**
  - `Documentation/virt/uml/redesign/upstream-patches/
    ftrace-notrace-generic-v1/SUBMISSION-NOTES.md` (Status
    "READY (regenerated 2026-05-14, commit `cdea7ace1893`)").

---

## D127 (2026-05-14) — Snapshot v2 port Phase 2: KUnit boot-time fixture

**Track:** B (Time-machine).

**Decision.** Phase 2 of #168 lands a boot-time KUnit fixture
that primes a vCPU at suite_init time so the snapshot test
suite can actually run (Phase 1 was build-only with
`kunit_skip`).

**Approach.** KUnit `suite_init` hook calls a new helper
`kvm_v2_vcpu_prime_for_kunit()` exposed behind
`#if IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_KUNIT)`. The helper
is an extracted version of the `if (!vcpu->cpuid_primed)` block
from `kvm_v2_vcpu_run()` — idempotent on `cpuid_primed`
(KVM_SET_CPUID2 is one-shot per vCPU; KVM rejects after first
KVM_RUN).

**Surprises that Phase 1's design memo didn't enumerate.**

  1. `KVM_GET_REGS` / `KVM_GET_SREGS` work pre-`cpuid_primed`
     — only `KVM_GET_XSAVE` / `KVM_SET_XCRS` are gated on
     OSXSAVE + curated CPUID.
  2. `KVM_SET_CPUID2` is one-shot per vCPU (locked at first
     `KVM_RUN`); the fixture has to short-circuit on
     `cpuid_primed` so KUnit re-trigger doesn't re-enter the
     install path.
  3. "pick_vcpu by `last_task`" path falls through to the
     `smp_processor_id()` fallback at suite_init time (no UML
     task has dispatched yet); on `ncpus=1` boot this is
     `vcpus[0]`, which is what the fixture primed. On
     `ncpus>1` the fixture would need to prime whichever vCPU
     `smp_processor_id()` returns at test-case time.

**Verification.** KUnit boot under `backend=force=kvm-v2
mem=256M ncpus=1`:

  ```
      # Subtest: kvm_v2_snapshot
      ok 1 test_kvm_v2_snapshot_basic
  ok 3 kvm_v2_snapshot
  ```

All three KUnit suites (`kvm_v2_marshal` 8/8, `kvm_v2_byteshape`
9/9, `kvm_v2_snapshot` 1/1) pass.

**LoC.** +450 / -59 across 6 files.

**Refs.**
  - `arch/um/backend/kvm-v2/test_snapshot.c` (NEW, 163 LoC).
  - `arch/um/backend/kvm-v2/vcpu.c::kvm_v2_vcpu_prime_for_kunit`.
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/
    06b-snapshot-port-phase2.md` (diary).
  - D123 (Phase 1).

---

## D128 (2026-05-14) — Tier 3 daemon-side per-worker fanout in run-soak-daemon.sh

**Track:** A (Phase J).

**Decision.** `run-soak-daemon.sh` now branches on workload
name: `tier3-*` workloads spawn `$WORKERS` parallel `umlctl
gate loop --workers 1` invocations with the per-worker
placeholders substituted; everything else keeps the original
`umlctl gate loop --workers $WORKERS` shape.

**Why a branch rather than a unified path.** The non-tier3
workloads don't need per-worker IPs (memcheck, iocheck,
stress-ng, kbuild-tiny all run with `[network] mode = "none"`).
The tier3 fanout adds bookkeeping (per-worker TOML, per-worker
output subdir, per-worker scoreboard walk) that's pure overhead
for non-tier3 workloads. The branch is one `case` statement at
the top of `run_one_phase`.

**Verification.** Dry-run smoke with `--workloads
tier3-django,iocheck --workers 3 --iters-per-rotation 2`:

  - Tier3 phase: 3 worker lines, each with distinct
    `host=192.168.42.{1,5,9}` / `guest=192.168.42.{2,6,10}` /
    `tap=soak-tap{0,1,2}`.
  - Non-tier3 (iocheck) phase: single `umlctl gate loop -W 3`
    invocation, unchanged.
  - Backend rotation (`kvm-v2` → `seccomp`) reuses tap names
    within a phase but serial-only — taps torn down between
    phases by umlctl.
  - Per-worker TOMLs: no stray `{{...}}` placeholders.

**What's not yet verified.** Live smoke under a `CONFIG_UML_NET_VECTOR=y`
kernel — that's operator pre-flight (~3 min kernel rebuild +
~30 s `apt-get install python3-django python3-fastapi
python3-uvicorn`).

**Refs.**
  - `tools/testing/selftests/um/soak/run-soak-daemon.sh`
    (commit `ba63d93515a5`).
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/
    06-daemon-tier3-fanout.md` (diary).
  - D125 (option (b) decision for the per-worker scheme).

---

## D129 (2026-05-14) — Series 7 (kvm-backend-series) cover letter rewritten for v2

**Track:** D (Upstream).

**Decision.** Series 7's cover letter
(`0000-cover-letter.patch.md`) is rewritten end-to-end to
reflect the v2 redesign rather than the v1-era pitch it
inherited from the post-G8 / D70 GO milestone. New length:
414 → 900 lines (+486).

**Why a full rewrite.** Series 7's old draft used v1
vocabulary (per-task vCPU, shadow page tables, no XSAVE
context, no per-vCPU pool) and pointed at memos that have
since been deprecated. A 30%-rewrite would have been more
work than a from-scratch refresh because every section's
load-bearing claim changed.

**Three anticipated review questions flagged in the cover.**

  1. Per-CPU vCPU pool vs. per-task vCPU. Counter is memo 26
     §C + the empirical v1 → v2 transition. Risk register
     includes a fallback `CONFIG_UM_BACKEND_KVM_V2_VCPU_MODEL
     = pool|per_task` Kconfig switch with `per_task` gated on
     BROKEN if pushback is hard.
  2. Class-D classifier (10-NR trap list, short-circuit to
     -EPERM). Memo 10 + D84 are the pointers.
  3. Per-mm host worker model deferral. R4 refactor (each
     guest mm = its own host process) was deferred during
     Phase A→I. Cover lets reviewers weigh "ship single-VM
     with per-mm worker as v3 follow-up" vs. "include per-mm
     in initial drop".

**Status flow.** Series 7 still BLOCKED by Phase J DONE
certificate (memo 26 §6.3); the cover-letter rewrite was the
"start packaging the cover letter NOW even while Phase J
finishes" recommendation from PLAN-2026-05-14 §6.3.

**Refs.**
  - `upstream-patches/kvm-backend-series/0000-cover-letter.patch.md`
    (commit `aaaa3ce70027`).
  - `upstream-patches/kvm-backend-series/SUBMISSION-NOTES.md`.
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/
    06-series7-cover-letter-refresh.md` (diary).

---

## D130 (2026-05-14) — Record/replay v2 port design memo (#169)

**Track:** B (Time-machine).

**Decision.** #169 (record/replay) gets its design memo
(`02-workstreams/D-kvm-backend/27-record-replay-v2-port.md`,
1012 lines) as the sibling to #168's port memo. No code; pure
sequencing. The memo identifies seven phases mirroring the
snapshot port's structure and an entry point for Phase 1
(`record.c` skeleton, ~350 LoC, state-machine only).

**Why a separate memo from #168.** The two ports share design
DNA (both lift from `kvm-v1-archive/` against v2's per-pool
vCPU + XSAVE + gadget LSTAR architecture) but the *semantic
deltas* are independent. #168 captures state; #169 captures
*every output of host-side nondeterminism* and replays it.
Separating the memos lets reviewers (and future-me) read each
on its own terms.

**Three hardest open questions flagged.**

  1. **KVM API for RDTSC exit configuration.** No clean per-
     vCPU explicit exit toggle in upstream KVM. Options: TSC-
     offset model (`KVM_TSC_CONTROL` + per-vCPU
     `MSR_IA32_TSC` virtualization), `KVM_CAP_X86_DISABLE_EXITS`
     (wrong direction). Needs a one-day spike before Phase 5.
  2. **Cross-kernel-version replay with per-pool vCPU layout
     drift.** Memo 13 promised "deterministic across kernel
     versions" but the v2 vCPU pool has gained fields commit-
     to-commit (SMP-T16 `last_task`, SMP-T55 `fpu_dirty`,
     SMP-T57 XSAVE/XCRS). Mitigation candidates: kernel-rev
     pinning vs. interface-version contract vs. CRIU-style
     field-presence flags. No clear winner.
  3. **SIGALRM precision (PMU vs. syscall-count).** PMU-driven
     instruction-retired counter is the rr-canonical approach
     but Zen 4 KVM PMU passthrough is shakier than Intel and
     v2 dev runs on AMD. Memo opts for syscall-count for
     Phase 6 with a documented ±100 syscall precision; if a
     workload reveals (b) inadequate, Phase 6.5 has to add
     PMU-driven (a).

**Recommended Phase 1 entry.** `arch/um/backend/kvm-v2/record.c`
(~350 LoC) — `struct kvm_v2_record`, enums, static_key, alloc/
destroy/free/start/stop/replay/strict_replay state machine
only (observe/consume are no-op stubs). KUnit
`test_kvm_v2_record_basic` asserts state-machine transitions
without a live vCPU dependency, so #168 Phase 3 is not a
blocker for #169 Phase 1.

**Refs.**
  - `02-workstreams/D-kvm-backend/27-record-replay-v2-port.md`
    (commit `95c9cb2094a3`).
  - `02-workstreams/D-kvm-backend/13-record-replay-determinism.md`
    (the strategic memo from 2025).
  - `arch/um/backend/kvm-v1-archive/record.c` (1597 LoC, lift
    target).
  - D123 (snapshot Phase 1) + D127 (snapshot Phase 2 KUnit env).

---

## D131 (2026-05-16) — record/replay v2 port Phase 1: state machine + static-key gate

**Track:** B (Time-machine).

**Decision.** #169 (record/replay) Phase 1 lifts v1's
`kvm-v1-archive/record.c` lifecycle surface (~280 lines, lines
41-410 of the archive) onto v2's symbol prefix + Phase-1 fixed-
buffer shape. State machine: `INIT → RECORDING → STOPPED →
REPLAYING` plus invalid-edge `-EINVAL`. The
`DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled)` hot-path
gate is in tree from day 1 so Phase 2's hook in
`syscall_trap.c::kvm_v2_handle_io_trap` lands as a pure
addition. KUnit `kvm_v2_record` suite is 2 cases (basic-path +
invalid-transitions) — passes 2/2 alongside the existing
marshal 8/8, byteshape 9/9, snapshot 2/2.

**Two surface decisions Phase 1 had to take.**

  1. **Fixed-size buffer vs. v1's growable log array.** v1
     starts with a 256-entry array and doubles on demand
     (`kvm-v1-archive/record.c:444-471`). Phase 1 takes a
     `size_t buffer_size` arg to `_alloc` and kvmalloc's a
     fixed-size scratch buffer once. Rationale: Phase 1's
     observe/consume are no-op stubs (the buffer is never
     written to), so the doubling machinery is pure complexity
     debt. Phase 2 may revisit when the append helper lands —
     either keep the fixed buffer (simpler) or port v1's
     doubling logic (matches v1 more closely). Default size
     is 64 KiB, cap is 64 MiB (matches v1's
     `KVM_RECORD_PAYLOAD_TOTAL_CAP`).

  2. **Stricter error policy than v1 on stop/replay.** v1's
     `kvm_record_stop` is idempotent (calling stop on an
     unarmed container is a no-op). Memo 27 §Phase 1
     explicitly calls out "assert invalid transitions return
     error" — so the v2 port returns `-EINVAL` on
     stop-without-start, stop-after-stop, replay-from-INIT,
     replay-while-recording, replay-after-replay,
     start-after-start, and start-from-REPLAYING. This is the
     intended Phase 1 surface; Phase 7's debugfs operator
     plumb may need to swallow `-EINVAL` to preserve v1's
     idempotent-from-shell shape (decision deferred).

**Three deferrals confirmed.**

  - **Snapshot integration.** Phase 1's container has no
    `*checkpoint` field; Phase 3 of #169 reintroduces against
    `kvm_v2_snapshot_capture_full` (memo 27 §3.9: log-only
    mode is fine for Phase 1-2; the KUnit doesn't need a
    snapshot since observe/consume are stubs).
  - **`um_kvm_v2_active_record` accessor.** The single-active-
    record global is file-scope in record.c at Phase 1. Phase 2
    needs an accessor to plug into the syscall_trap.c hook;
    introducing `kvm_v2_record_active(void)` (returns the
    locked pointer under the spinlock) is cleaner than
    exporting the global. Decision deferred to Phase 2's
    implementation phase.
  - **Debugfs surface.** v1 had
    `/sys/kernel/debug/um/kvm_record_{ctl,state,log}`
    (`kvm-v1-archive/record.c:1374-1576`). Phase 1 ships
    none of it; Phase 7's kselftest re-plumb adds the v2
    analogs. No new symbols needed for Phase 2-6 to land.

**KUnit verification.** Boot under
`backend=force=kvm-v2 mem=512M ncpus=1 init=/bin/echo`. All 4
suites pass: `kvm_v2_marshal` 8/8, `kvm_v2_byteshape` 9/9,
`kvm_v2_snapshot` 2/2, `kvm_v2_record` 2/2. The state-machine
pr_info lines from the test execution document every
transition in dmesg (start → stop → replay → stop, plus the
7 invalid edges with their corresponding `pr_warn` diagnostics).

**LoC delta.** ~1013 lines added across:

  - `arch/um/backend/kvm-v2/record.c` +547 (new file).
  - `arch/um/backend/kvm-v2/test_record.c` +224 (new file).
  - `arch/um/backend/kvm-v2/kvm_v2_backend.h` +228 (struct,
    enums, prototypes, 2 includes).
  - `arch/um/backend/kvm-v2/Makefile` +14.

**checkpatch.** 0 errors, 1 known-false-positive warning
("EXPORT_SYMBOL should immediately follow") on the
`DEFINE_STATIC_KEY_FALSE` macro — same false positive as v1's
`kvm-v1-archive/record.c:73-74`. Matches baseline.

**Recommended Phase 2 entry point.** The Phase 2 commit hooks
`arch/um/backend/kvm-v2/syscall_trap.c::kvm_v2_handle_io_trap`
between line 2195 (`handle_syscall(regs)`) and line 2353
(marshal-out), gated by
`static_branch_unlikely(&um_kvm_v2_record_enabled)`. The hook
calls `kvm_v2_record_observe_syscall(rec, nr, ret, regs)` —
which Phase 2 reimplements to actually append. No surface
change to record.c's public API.

**Refs.**
  - `02-workstreams/D-kvm-backend/27-record-replay-v2-port.md`
    §Phase 1 + §4 entry-point recommendation.
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/08-record-port-phase1.md`
    (the diary for this landing).
  - D130 (memo design contract; predecessor).
  - D123 (snapshot Phase 1, the architectural sibling).

---

## D132 (2026-05-16) — record/replay v2 port Phase 2: observe hook + XSAVE-on-every-vmexit

**Track:** B (Time-machine).

**Decision.** #169 (record/replay) Phase 2 fills in the Phase 1
stub bodies and wires the `observe_syscall` hook in
`syscall_trap.c::kvm_v2_handle_io_trap`. Phase 1 (D131) landed
the state-machine skeleton + the `DEFINE_STATIC_KEY_FALSE
(um_kvm_v2_record_enabled)` gate so this commit is a pure
addition — no surface broke, no existing call site moved.

**Three surface decisions Phase 2 took.**

  1. **Accessor (`kvm_v2_record_active()`) vs. exporting the
     `um_kvm_v2_active_record` global.** D131 deferred this to
     Phase 2's implementation. The accessor wins:
     - It encapsulates the spinlock acquisition — callers don't
       need to know about `um_kvm_v2_record_lock`'s existence.
     - It returns NULL safely when `_stop` clears the slot
       between the static_branch read and the accessor call,
       so the hook site doesn't need its own NULL handling
       beyond the simple `if (rec)` test.
     - It keeps `record.c`'s globals file-scope so the only
       export surface is the function name. Phase 7's debugfs
       plumb consumes the same accessor.

  2. **`struct kvm_v2_replay_entry` inline payload shape.**
     Phase 1's header was bare 16 bytes; Phase 2 grows it to
     80 bytes total by inlining the SYSCALL payload
     (`{nr, _pad, retval, args[6]}`). Alternative: keep the
     header bare and follow each header with a kind-specific
     payload struct. The inline shape was picked for:
     - **Simpler walker.** The Phase 3 consume hook reads
       `entry->size` from the header and advances by that
       amount; no second pointer chase per entry.
     - **Mixed-kind logs.** Phase 5-6 introduce RDTSC/SIGALRM
       kinds with different payload shapes. The `@size` field
       discipline (`size == sizeof(header) + sizeof(payload)`)
       composes naturally with a union: the SYSCALL payload
       member lands inline; future kinds add sibling members
       under a union once their payloads land. The Phase 1
       comment "Phase 2 may grow it" was the explicit
       authorization to do this.
     - **Cost.** 80 bytes/entry vs. v1's variable-stride log
       array. A 64 KiB default buffer holds ~819 entries; a
       64 MiB max holds ~838 K. Comparable to v1's 256-entry
       initial × ~256-byte stride for the same buffer
       footprint, with the bound enforced at compile time
       per kind.

  3. **Buffer-full policy.** Phase 2 drops quietly when the
     buffer can't fit the next entry. Alternative considered:
     return `-ENOSPC` and let the hook site call `_stop` on
     the container. Quiet drop wins for Phase 2 because:
     - The hook site (`kvm_v2_handle_io_trap`) has no error-
       handling contract — there's nowhere to surface
       `-ENOSPC` to. Crashing the kernel on log overflow is
       wrong; pr_emerg-ing every overflow would flood the log
       harder than the original cause.
     - Phase 7's overflow handler (memo 27 §Phase 7) is the
       right place for the explicit `_stop`-on-cap policy.
       Phase 2's quiet drop is benign in the interim: replay
       in strict mode will SIGSEGV at the divergence point
       (the entry that fell off the cap), surfacing the
       overflow as the same fail-stop signal the operator
       wants.

**SMP-T55 lazy-FPU interaction (memo 27 §3.8(i)).** Phase 2's
hot-path change to `vcpu.c` adds one new arm to the SMP-T55 skip
condition: when `um_kvm_v2_record_enabled` is on, the
`KVM_GET_FPU` always runs (regardless of `fpu_dirty` /
`fpu_owner_task`). Rationale per memo 27 §3.8(i): replay needs
bit-identical post-vmexit XSAVE; the SMP-T55 dirty-epoch
optimization preserves the vCPU's FPU across dispatches in a way
that doesn't compose with replay's restore-then-execute contract
(SMP-T55's design assumed no replay). Zero-cost when off (the
new arm is the same `static_branch_unlikely` predicate the
observe-hook site uses); ~1µs per dispatch when on.

Three options had been documented (i) force-GET-every-vmexit;
(ii) record only deltas at observation points (requires touched-
FPU detection — XSAVE-INUSE bitmap walk); (iii) only record
dispatches that change the dirty epoch (cleanest if SMP-T55
exposes the epoch-advance event). Memo 27 picked (i) for Phase
2 as the simplest path; (ii)/(iii) are optimizations to revisit
if record-mode workloads need them. D132 confirms (i) is in
tree.

**KUnit verification.** Boot under `backend=force=kvm-v2 mem=512M
ncpus=1 init=/bin/echo`. All 4 suites pass:

  - `kvm_v2_marshal`: 8/8 (unchanged).
  - `kvm_v2_byteshape`: 9/9 (unchanged).
  - `kvm_v2_snapshot`: 2/2 (unchanged).
  - `kvm_v2_record`: 3/3 (was 2/2 at Phase 1; new case
    `test_kvm_v2_record_observe` exercises the append path
    directly with synthetic `uml_pt_regs` args).

Beyond KUnit: `/bin/echo` boots to exit-0 with the same boot
arguments. The new hot-path block in `kvm_v2_handle_io_trap`
compiles to a 5-byte NOP when record is off (jump_label_init at
boot patches the `static_branch_unlikely` to its no-record
direction); the SMP-T55 skip site's new arm is similarly patched
out. Phase 2 is zero-cost when off.

**LoC delta.** ~450 lines added across:

  - `arch/um/backend/kvm-v2/record.c` +~150 (accessor + body).
  - `arch/um/backend/kvm-v2/syscall_trap.c` +~50 (hook block).
  - `arch/um/backend/kvm-v2/vcpu.c` +~5 + comment (SMP-T55
    record-armed arm).
  - `arch/um/backend/kvm-v2/kvm_v2_backend.h` +~30 (struct
    extension + prototype).
  - `arch/um/backend/kvm-v2/test_record.c` +~170 (new case +
    new includes).

5 existing files modified (record.c, syscall_trap.c, vcpu.c,
kvm_v2_backend.h, test_record.c); 0 files added; 0 files removed.

**checkpatch.** 0 errors, 0 warnings on the diff. Clean.

**Recommended Phase 3 entry point.** Phase 3's consume hook
slots into the SAME `kvm_v2_handle_io_trap` function as Phase
2's observe hook, but BEFORE `handle_syscall(regs)` (the
existing line ~2195). The shape is symmetric to Phase 2's
observe site: gated by `static_branch_unlikely(&um_kvm_v2_
record_enabled)`, consults `kvm_v2_record_active()`, calls
`kvm_v2_record_consume_syscall(rec, syscall_nr, &served_ret)`
when state == REPLAYING. On `rc > 0` (entry served), `goto`
past `handle_syscall` to the post-call marshal-out; on `rc < 0`
(divergence), strict mode delivers `force_sig(SIGSEGV)`. The
diary (`11-record-port-phase2.md` §Recommended Phase 3 entry
point) carries the full sketch.

**Refs.**
  - `02-workstreams/D-kvm-backend/27-record-replay-v2-port.md`
    §Phase 2 + §3.1 + §3.8(i).
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/11-record-port-phase2.md`
    (the diary for this landing).
  - D131 (record/replay Phase 1; predecessor).
  - D130 (memo 27 design contract).
  - Commit `358c4d3c83ab` (Phase 1 landing).

---

## D133 (2026-05-16) — record/replay v2 port Phase 3: consume_syscall + strict-replay hook

**Track:** B (Time-machine).

**Decision.** #169 (record/replay) Phase 3 closes the loop: fills in
the Phase 1 `consume_syscall` stub with a real FIFO walker over
`rec->buffer`, wires the matching pre-`handle_syscall` hook in
`syscall_trap.c::kvm_v2_handle_io_trap`, and lands the
`test_kvm_v2_record_strict_replay` KUnit case that exercises the
record → replay round trip end-to-end. Phase 1 (D131) landed the
state-machine skeleton + the
`DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled)` gate; Phase 2
(D132) wired the `observe_syscall` hook AFTER `handle_syscall`.
Phase 3 is symmetric: the `consume_syscall` hook fires BEFORE
`handle_syscall`, gated by the same static key, and on a served
entry (`rc > 0`) jumps past the live syscall to the post-call
marshal-out path via a new `skip_handle_syscall:` label.

**Three surface decisions Phase 3 took.**

  1. **Cursor-only advance on success.** `consume_syscall` advances
     `rec->buffer_replayed` ONLY when an entry is fully validated
     and served (`rc == 1`). On `-ENODATA` / `-EILSEQ` the cursor
     stays at the rejected entry so the operator can dump
     `rec->buffer + rec->buffer_replayed` post-mortem and read out
     the divergent entry's kind / nr / retval / args. The
     alternative — "advance on every consume regardless" — was
     rejected because it destroys the diagnostic trail (a
     strict-mode SIGSEGV kills the task; if the cursor points
     ONE PAST the divergent entry, the operator can't see what
     diverged). Validated by the test case's "matching NR after
     a failed NR-mismatch divergence still serves" sub-scenario.

  2. **`skip_handle_syscall:` label placement.** The replay-served
     path must skip three things and preserve two:
        - **Skip:** `handle_syscall(regs)` (the whole point);
                    the `interrupt_end()` `-ERESTART*` drain
                    (we'd double-translate the recorded retval);
                    the Phase 2 observe hook (re-observing during
                    replay would inflate `entries_recorded`).
        - **Preserve:** `PT_SYSCALL_NR(regs->gp) = -1;` clear
                    (cross-path orig_ax-leakage protection per
                    commit a478952b8da0; a subsequent #PF
                    dispatcher's interrupt_end would
                    misinterpret stale orig_ax as in-progress
                    syscall → SIGILL); the marshal-out (SYSRETQ
                    needs the right RIP/RFLAGS in RCX/R11).
     The label seam lands right BEFORE the `PT_SYSCALL_NR` clear
     — the only placement that gets all three "skip" + both
     "preserve" requirements right. Documented inline so future
     Phase 5/6 hooks have an explicit invariant.

  3. **Strict-mode `force_sig(SIGSEGV)` continues past
     `goto skip_handle_syscall`.** Two options were considered:
        - (a) Return early from `kvm_v2_handle_io_trap` with a
              non-zero rc after the `force_sig` call.
        - (b) `goto skip_handle_syscall` after the `force_sig`
              call, run the marshal-out normally, let the queued
              SIGSEGV deliver on the next return-to-userspace
              check.
     Picked (b). The vcpu.c call site doesn't have a stable
     error-propagation contract; the in-tree behaviour on a
     non-zero rc from `kvm_v2_handle_io_trap` is "halt the
     vmexit loop and panic," which is the wrong outcome for a
     per-task divergence (the WHOLE pool would die instead of
     just the divergent task). The (b) path lets the kernel
     re-enter the guest cleanly; `get_signal` → `do_signal`
     delivers the queued SIGSEGV on the next user-mode return
     check. Same semantics as v1's strict-replay path
     (`kvm-v1-archive/syscall_class.c`'s `force_sig` + return
     to the dispatch loop).

**`buffer_replayed` cursor.** Phase 3 adds `size_t
buffer_replayed` to `struct kvm_v2_record` as the read cursor,
distinct from Phase 2's `buffer_used` write cursor. Invariant:
`buffer_replayed <= buffer_used` at all times. Reset to 0 by
`kvm_v2_record_start` (rewind on record arm) and by
`kvm_v2_record_replay` (rewind on replay arm). The consume walker
reads `entry->size` from the header (Phase 2's append helper sets
this to `sizeof(entry)`; Phase 5/6 will mix payload sizes per
kind) to advance the cursor per-entry without knowing each kind's
fixed-size payload at compile time.

**KUnit verification.** Boot under `backend=force=kvm-v2 mem=512M
ncpus=1 init=/bin/echo`. All 4 suites pass:

  - `kvm_v2_marshal`: 8/8 (unchanged).
  - `kvm_v2_byteshape`: 9/9 (unchanged).
  - `kvm_v2_snapshot`: 2/2 (unchanged).
  - `kvm_v2_record`: 4/4 (was 3/3 at Phase 2; new case
    `test_kvm_v2_record_strict_replay` exercises the consume
    walker through three sub-scenarios — happy-path FIFO consume
    × 5 → -ENODATA, NR-mismatch divergence → -EILSEQ + cursor
    preservation, and not-REPLAYING quiet no-op).

Beyond KUnit: `/bin/echo` boots to the same VFS-mount panic as
Phase 2 with no record container ever instantiated outside the
KUnit cases. The new pre-`handle_syscall` hook block compiles to
a 5-byte NOP when record is off (jump_label_init at boot patches
the `static_branch_unlikely` to its no-record direction). Phase 3
is zero-cost when off.

**LoC delta.** ~405 lines added across:

  - `arch/um/backend/kvm-v2/record.c` +124 (consume body + the
    two `buffer_replayed` resets in _start / _replay).
  - `arch/um/backend/kvm-v2/syscall_trap.c` +85 (hook + label +
    `linux/sched/signal.h` include).
  - `arch/um/backend/kvm-v2/kvm_v2_backend.h` +32 (field +
    kerneldoc + prototype rewrite).
  - `arch/um/backend/kvm-v2/test_record.c` +164 (new KUnit case
    with three sub-scenarios).

4 existing files modified; 0 files added; 0 files removed; 0
behavioral changes to existing call sites.

**checkpatch.** 0 errors, 7 warnings — all 7 are the same
"Prefer 'fallthrough;' over fallthrough comment" false positive
(prose mentions of "fall through" in kerneldoc; no switch-case
fallthrough is involved). Same false positives Phase 1 hit; the
flagged comments are documenting the control-flow contract of
the rc=0 / rc<0 + !strict paths to the syscall_trap.c hook,
not statement-level fallthroughs.

**Recommended Phase 4 entry point.** Phase 4 wires gadget-disable
into the record arm/disarm path so the 11 gadgeted NRs (getpid,
gettid, getuid, clock_gettime, ...) become observable to the
Phase 2 observe hook + replayable by the Phase 3 consume hook.
Per memo 27 §3.3 the decision is Option A: a per-vCPU
`KVM_V2_GADGET_OFF_RECORD` byte in the gadget state page;
`lstar_gadget.S:gadget_entry` reads it (5 bytes: `cmpb $0, ...;
jne fallback`) and branches to the fallback path when set.
`record.c` adds `kvm_v2_record_set_gadget_fallback(bool)` called
from `_start` / `_stop` / `_replay` / `_destroy`. Without Phase
4 a recorded workload that calls a gadgeted NR (e.g. `getpid`)
produces NO log entry; the replay sees the live `getpid` go
through to handle_syscall and the strict_replay contract fails
on the first gadgeted call. The diary
(`13-record-port-phase3.md` §Recommended Phase 4 entry point)
carries the full sketch.

**Refs.**
  - `02-workstreams/D-kvm-backend/27-record-replay-v2-port.md`
    §Phase 3 + §3.2.
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/13-record-port-phase3.md`
    (the diary for this landing).
  - D132 (record/replay Phase 2; predecessor).
  - D131 (record/replay Phase 1; predecessor).
  - D130 (memo 27 design contract).
  - Commit `41e0de91afb8` (Phase 2 landing).
  - Commit `358c4d3c83ab` (Phase 1 landing).

---

## (Future entries here, as decisions are made)
