# D-01: KVM platform design

**Status:** in progress (2026-04-23) — spike 01 landed + measured,
full design memo to follow.
**Effort:** 4 weeks
**Dependencies:** A-01 (ops table)
**Blocks:** D-02..D-06

## Spike 01 result (2026-04-23)

Bare `KVM_RUN` → `VMEXIT` → userspace round-trip costs
**~4.7 µs / ~17500 cycles median** on a Skylake-SP
(Xeon W-2123 @ 3.6 GHz, 1000 iterations, tight distribution
p10=17510, p90=17590). Full methodology + raw numbers + a
go/no-go breakdown for the D-workstream's vision-level
assumptions in `spikes/01-getpid-roundtrip/README.md`.

**What changes downstream:**

- The `00-vision.md` "~100 ns syscall" line is misleading for
  the naive D backend. `~100 ns` is what gVisor's
  kvm-platform-with-systrap delivers via a ring-0 in-guest
  gadget that skips the VMEXIT for most syscalls. The bare
  `KVM_RUN` path — what the minimum-viable D backend lands —
  pays the full VT-x exit cost and delivers ~1-5 µs.
- 4.7 µs is still a **4× improvement** over the seccomp
  backend's ~20 µs per guest syscall, so the D workstream
  is still worth shipping. The "~100 ns" figure becomes a
  **Q7-Q8 aspirational target** predicated on a systrap-
  equivalent gadget layer (D-04 ring transitions work),
  not an initial-bring-up promise.
- The design memo (below, in progress) names the systrap
  gadget explicitly as a follow-on; first-phase D ships a
  straightforward backend that hits the ~5 µs floor and
  wins on the merits.

See `spikes/01-getpid-roundtrip/` for the spike harness +
full discussion. Methodology: real-mode guest executing a
single `hlt`, 1000 iterations, rdtsc bracketed around
`KVM_RUN`. LSTAR-trap variant attempted first but tripled
into `KVM_EXIT_SHUTDOWN` without a real GDT/TSS; the VMEXIT
cost we're measuring is a CPU property independent of guest
mode, so we simplified.

## Goal

A complete design memo for `um_backend_kvm`. Adapt gVisor's KVM
platform model to UML's needs. Identify everywhere we deviate
from gVisor and why.

## Approach

1. Study gVisor's KVM platform end-to-end
   (`pkg/sentry/platform/kvm/`).
2. Map every gVisor mechanism to UML equivalent:
   - `Machine` → UML KVM context
   - `vCPU` → KVM vCPU per UML vCPU
   - `addressSpace` → UML guest mm_struct
   - bouncing thread → host-mode UML kernel thread
3. Identify deviations:
   - gVisor is in Go; UML is in C — affects allocator,
     thread mgmt
   - gVisor has Sentry as the kernel; UML has real Linux —
     more arch code to handle inside the guest
   - gVisor has userland-managed page tables; UML has
     kernel page tables already
4. Write the memo: `02-workstreams/D-kvm-backend/design-memo.md`.

## Deliverable

- `02-workstreams/D-kvm-backend/design-memo.md` (~10 pages)
- Map of gVisor mechanisms → UML equivalents
- Deviation list with rationale
- Sequence diagram: syscall trap path
- Sequence diagram: page fault path
- Sequence diagram: I/O path

## Validation

- Hypervisor expert reviews (look for someone with KVM internals
  experience)
- gVisor team reaches out to confirm we're not misreading their
  design
- All workstream A maintainers sign off on the ops table fit

## Open questions

- **Q1**: Single KVM guest with multiple vCPUs, or multiple KVM
  guests for SMP? (Plan: single guest, multiple vCPUs. Standard.)
- **Q2**: How does timekeeping work — host TSC, KVM clock,
  emulated PIT? (Plan: KVM clock if available, host TSC fallback.)
- **Q3**: Does the KVM guest need a BIOS / EFI / boot loader?
  (Plan: no. We construct the guest's initial state directly.
  No firmware boot.)

## Risk

The design memo is the highest-leverage document in workstream
D. If we get the design wrong, 5 months of work follow into a
dead end.

**Mitigation:**
- Long review (4 weeks).
- Spike: prototype the simplest possible "KVM guest that returns
  from MSR_LSTAR" before committing to the full design.
