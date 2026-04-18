# A-01.8 — Open question resolutions

Date: 2026-04-17
Owner: claude-code session
Inputs: A-01.0 through A-01.7

## Scope

Resolves the four open questions in `01-ops-table.md` (the A-01 task
spec) and the four open questions in the workstream A README. Each
resolution is propagated into `arch/um/include/asm/backend.h` (already
done in A-01.3) and into the user-facing `backend-contract.rst`
(A-01.9, next).

A summary entry is appended to
`04-risks/decisions-log.md` as decision **D8** (mirroring the format
of D1–D7).

---

## A-01 task questions (from `01-ops-table.md`)

### Q1: Are ops synchronous or can they return a "completion to poll later" handle?

**Resolution: All ops are synchronous.**

Reasoning: A-01.4/.5/.6 confirmed every existing call site, and every
KVM forward-looking call, is either trivially synchronous or already
synchronizes internally before returning. No call site benefits from
async semantics within Layer 1.

Where async behavior is needed (host I/O, device emulation), the
existing kernel infrastructure (`os_*_epoll`, irq subsystem, virtio)
handles it *outside* the ops table. Layer 1 stays sync; Layer 2
adds runtime tracing async-friendly via static keys.

Encoded in: `asm/backend.h` op signatures (no completion-token
arguments).

### Q2: Do ops carry a `void *backend_private` or is per-backend state global?

**Resolution: Global per-backend singleton; per-mm state in
`struct mm_id`; per-thread state in `task->thread`.**

Reasoning: A-01.5 and A-01.6 both showed every cross-op channel can be
satisfied by:

- `struct mm_id` (per-mm; gains 2 fields for KVM, ~16 bytes total)
- `task->thread` (per-thread; gains a vCPU descriptor union member)
- Backend module-private static state (per-backend singleton)

No `void *backend_private` was needed in either sketch. A pointer
slot would force allocator round-trips and hide ownership; this
explicit shape avoids both.

`mm_id` field ownership is documented in `backend-contract.rst`.

Encoded in: `asm/backend.h` (no `backend_private` field).

### Q3: How do we handle ops that one backend wants to inline (seccomp's `syscall_dispatch`, hot path) vs ops where the indirect call is fine (rare debug ops)?

**Resolution: Single-backend-only Kconfig builds inline via macros;
multi-backend builds use indirect call through `um_backend`. Five
ops are classified HOT.**

The five HOT ops:
1. `run_userspace`
2. `mm_map`
3. `mm_unmap`
4. `context_switch`
5. `read_clock_ns`

The remaining 13 ops are cold; they always go indirect (cost is
noise relative to their per-call frequency).

Encoded in: `asm/backend.h` (`um_backend_dispatch()` macro), Kconfig
gating (`CONFIG_UM_BACKEND_*_ONLY`) — Kconfig wiring is A-04.

### Q4: Versioning: do we expose a contract version to the kernel for runtime introspection?

**Resolution: Yes — `u32 contract_version` in `struct um_backend_ops`,
plus `UM_BACKEND_CONTRACT_VERSION` macro in the header.**

Policy:
- **Major bump on op signature changes** (breaking). Backends must
  be ported.
- **No bump on op additions** (non-breaking). New ops are appended
  to `struct um_backend_ops`; old backends report `NULL` and the
  dispatch layer treats unimplemented ops as `-ENOSYS`.

Initial value: `UM_BACKEND_CONTRACT_VERSION = 1`.

Encoded in: `asm/backend.h`.

---

## Workstream A README questions

### Q1 (README): Do we expose backend selection via boot param, Kconfig, or both?

**Resolution: Both.** Kconfig sets default and capability; boot
param overrides.

- Kconfig fragments: `CONFIG_UM_BACKEND_PTRACE`,
  `CONFIG_UM_BACKEND_SECCOMP`, `CONFIG_UM_BACKEND_KVM`,
  `CONFIG_UM_BACKEND_DYNAMIC` (multi). The set of compiled-in
  backends is fixed at build time.
- Boot param: `backend=auto|ptrace|seccomp|kvm` with optional
  `backend=force=<kind>`.
- Default in dynamic builds: `auto` (probe in order: KVM → seccomp
  → ptrace; first success wins).
- Legacy `seccomp=on/auto/off` boot param remains as an alias for
  one transitional release; deprecation noted in
  `Documentation/admin-guide/kernel-parameters.txt`.

Encoded in: A-04 Kconfig design + `arch/um/kernel/backend.c` arbiter
(written in A-02).

### Q2 (README): What's the abort policy if `force=kvm` and KVM is unavailable?

**Resolution: Panic.** No silent fallback.

Rationale: silent fallback in security-research/fuzz contexts can
mask reproducibility bugs. If the user explicitly forced a backend,
they want to know it isn't available.

`backend=auto` (the default) does fall back through the priority
order without panicking. Only `force=` panics.

Encoded in: `init_backend()` arbiter (impl in A-02);
`asm/backend.h` `struct um_backend_args.force`.

### Q3 (README): How granular is the ops table?

**Resolution: 18 ops in 5 categories** (A-01.2):
- Lifecycle/trap: 4
- Memory: 4
- Scheduling: 4
- Time: 3
- Debug/introspection: 3

Slightly above the architecture-doc target of "~15 ops" but well
below the "too fine; dispatch dominates" boundary. Each op has a
clear cost class (HOT or cold) and a clear set of consumers.

Encoded in: `asm/backend.h`.

### Q4 (README): Can we add ops without bumping a contract version?

**Resolution: Yes** (same as A-01.3 Q4 above).

---

## Implications for downstream tasks

- **A-01.9** (`backend-contract.rst`): codify the per-op semantics
  with sync/cost-class annotations and document `mm_id` field
  ownership.
- **A-01.10** (LKML memo): present the 18-op table and the 4-decision
  resolutions in concrete form.
- **A-02** (ptrace refactor, 6 wk): consume `init_backend()`,
  `um_backend_dispatch()`, and the ops table; migrate every site
  in the inventory.
- **A-03** (seccomp wrap, 4 wk): produce `arch/um/backend/seccomp/`
  with the 18 ops; share with ptrace via `arch/um/backend/common/`.
- **A-04** (Kconfig, 2 wk): define the four `UM_BACKEND_*_ONLY` /
  `UM_BACKEND_DYNAMIC` modes; build single-backend inlining.
- **A-05** (conformance, 4 wk): one kunit test per op + cross-
  backend equivalence + perf regression.
- **A-06** (documentation, 2 wk): expand `backend-contract.rst`
  with worked examples and KGDB/KVM upgrade paths.
- **A-07** (perf CI, 2 wk): cycle-level CI gates for the 5 HOT ops.

## Decision log entry (to append in A-01.11)

See appended entry "D8: Backend ops table — 18 ops, 5 hot, sync,
contract-versioned" added to `04-risks/decisions-log.md` in subtask
A-01.11.
