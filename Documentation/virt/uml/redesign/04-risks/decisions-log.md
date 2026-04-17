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

## (Future entries here, as decisions are made)
