# D-01: KVM platform design

**Status:** in progress (2026-04-23) — spike 01 landed + measured,
full design memo to follow.
**Effort:** 4 weeks
**Dependencies:** A-01 (ops table)
**Blocks:** D-02..D-06

## Spike 01/02 result (2026-04-23)

Bare `KVM_RUN` → `VMEXIT` → userspace round-trip measured
across eight hosts (four Intel generations + AMD Zen 4). Full
per-host table in `measurements.md`; the takeaway:

- **Skylake-era Intel (2015-2017): 4.7-6.1 µs / 17500-20600
  cycles.** Flat across Skylake-S / Kaby Lake / Skylake-SP.
- **AMD Zen 4 (2023, Ryzen 7 7840HS): ~12000 cycles, 2.5-6 µs
  depending on P-state clock.** Generational improvement
  over Skylake but not the Alder Lake i9 floor.
- **Alder Lake i5-12600K: ~11700 cycles, 2.6 µs.** Cycle
  count near Zen 4, clock high, so ns is competitive.
- **Alder Lake i9-12900K: ~3800 cycles, 780 ns.** Step-
  change below the rest of the inventory — within one order
  of magnitude of the ~100 ns vision line *without* any
  gadget work.

**Cycle count is silicon-bounded, ns is clock-bounded.**
Three Zen 4 hosts (s5/s6/s7) showed cycle counts within 2%
of each other at clocks spanning 2.0-5.0 GHz; the ns values
fanned out accordingly.

**What changes downstream:**

- Naive KVM backend's realistic performance envelope:
  **~5 µs on old Intel, ~2.5 µs on Zen 4 or Alder Lake i5
  at boost, ~800 ns on Alder Lake i9.** Every point is a
  ≥4× win over the seccomp baseline (~20 µs per guest
  syscall) — 25× on the i9.
- The ~100 ns vision target is still aspirational and
  needs either (a) silicon newer than what we measured,
  or (b) a systrap-equivalent gadget layer that handles
  common syscalls in-guest without a VMEXIT. Likely both.
  But the gap went from "50× away" (Spike 01 alone) to
  "~8× on Alder Lake i9" — much more tractable.
- First-phase D is genuinely worth shipping on the merits.
  4-25× over seccomp across the whole silicon range, with
  observability-over-QEMU-KVM as the secondary argument.
  D-04 systrap gadget becomes a clear second-phase
  deliverable rather than a prerequisite for the first.

See:

- `spikes/01-getpid-roundtrip/README.md` — spike harness
  + raw per-host readouts + vision-alignment discussion.
- `measurements.md` — durable timing-data log, intended to
  extend over the life of the D workstream with new spikes
  and real-implementation benchmarks.

Methodology caveats (same as Spike 01): real-mode guest
executing a single `hlt`, 1000 iterations, rdtsc bracketed
around `KVM_RUN`. LSTAR-trap variant attempted first but
tripled into `KVM_EXIT_SHUTDOWN` without a real GDT/TSS; the
VMEXIT cost we're measuring is a CPU property independent
of guest mode, so the simplification doesn't change the
numbers.

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
