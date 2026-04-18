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

## (Future entries here, as decisions are made)
