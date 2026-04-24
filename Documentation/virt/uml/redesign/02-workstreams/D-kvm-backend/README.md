# Workstream D: KVM backend

**Effort:** ~6 engineer-months
**Owner role:** Hypervisor engineer (familiar with KVM internals)
**Critical path:** No — parallel to C; depends only on A
**Bookend:** `getpid()` round-trip through KVM backend in <100 ns
            measured on bare metal

**Status (2026-04-23):** spikes + harness complete. Phase III
of the post-Q1 push landed D-01 through D-06 as a working
in-kernel harness, validated end-to-end:

- D-01 / D-02: kernel-side KVM backend scaffold + /dev/kvm
  probe (commits through `121`).
- D-03b/c/d: per-mm `kvm_um` with `KVM_CREATE_VM`, one-giant-
  memslot at init (Policy A per D20), `mm_map`/`mm_unmap`
  host-side wiring.
- D-04a/b/c: vCPU create + `KVM_RUN` loop, long-mode SREGS +
  CR3/GDT/IDT/EFER, LSTAR-routed SYSCALL dispatch through the
  real `sys_call_table`.
- D-05a/b: real time ops (`read_clock_ns`,
  `read_persistent_clock_ns`, `set_timer`), `ipi_send`
  (SMP-only).
- D-06: A-05 contract KUnit suite passes 20/20 under
  `CONFIG_UM_BACKEND_KVM_ONLY` (Phase III Lift #1f).
- Phase III Lifts #1b-#1e extended the harness to cover
  ring-3 SYSRETQ entry, LSTAR round-trip, `KVM_EXIT_MMIO`
  fault decode, and `KVM_INTERRUPT` + IDT injection.

The one piece *not yet* landed is **real `run_userspace`
integration** — replacing the one-shot harness with a
sustained trap loop driving genuine UML guest processes.
That's tracked as task #162; the design memo at
`08-real-run-userspace.md` (2026-04-23) decomposes it into
seven sub-commits, and decisions-log D65 records the scope
decision. The harness already demonstrates every primitive
the integrated path needs — the lift is a lift-out-of-
harness-into-production refactor with exit-reason wiring.

Decisions-log coverage: D49–D57 + D60–D61 walk through the
shape choices; D63 sequences the upstream submission; D65
scopes task #162. `04-ring-transition.md`, `04b*-*.md`,
`07-systrap-gadget-feasibility.md`, and
`08-real-run-userspace.md` carry the per-sub-task detail.

## What this workstream produces

A third backend implementation: `um_backend_kvm`. Modeled on
gVisor's KVM platform — UML kernel runs as ring-0 inside a KVM
guest, guest userspace processes run as ring-3 in the *same* KVM
guest, syscalls trap directly into ring-0 via MSR_LSTAR.

Result: ~100 ns syscall overhead on bare metal (vs ~1-5 µs on
ptrace, ~300-500 ns on seccomp).

## Why this is the gVisor-pattern, not "another VM"

gVisor's KVM platform is *not* a VM in the QEMU sense. There's no
guest BIOS, no OS boot, no virtio devices in the traditional
sense. The KVM guest is a single shared address space with two
ring levels:

- Ring 0: UML kernel
- Ring 3: UML guest userspace process

When a guest userspace process executes `syscall`, hardware
delivers control to ring 0's `MSR_LSTAR` handler — which is the
UML kernel's syscall entry. No VMEXIT to host. ~100 cycles.

When the UML kernel needs to do host-side work (I/O, scheduling),
it executes `HLT` or VMCALL — VMEXIT happens, host-mode UML
kernel takes over.

This is the architecture documented in
`pkg/sentry/platform/kvm/machine_amd64.go` of gVisor.

## Tasks

| # | Task | Effort | Status |
|---|---|---|---|
| 01 | [KVM platform design](01-kvm-platform-design.md) | 4 wk | planned |
| 02 | [MSR_LSTAR trap path](02-msr-lstar-trap.md) | 4 wk | planned |
| 03 | [Page-table management (host vs guest)](03-page-table-mgmt.md) | 6 wk | planned |
| 04 | [Ring transitions: kernel ↔ user](04-ring-transition.md) | 4 wk | planned |
| 05 | [Nested-virt fallback to seccomp](05-nested-virt-fallback.md) | 2 wk | planned |
| 06 | [Conformance + benchmarks](06-conformance.md) | 4 wk | planned |

Total: ~24 weeks ≈ 6 EM.

## Milestones

- **Month 1**: Design memo circulated; gVisor KVM model
  understood; differences from gVisor documented
- **Month 2**: Bare-metal: KVM guest boots; UML kernel runs as
  ring-0 inside it
- **Month 3**: First syscall round-trips through MSR_LSTAR
- **Month 4**: Page-table management stable; can spawn multiple
  guest userspace processes
- **Month 5**: Conformance suite passes
- **Month 6**: Bookend benchmark: `getpid()` <100 ns

## Open questions

- **Q1**: Do we copy gVisor's design wholesale or rebuild from
  KVM API basics? (Plan: study gVisor; copy patterns liberally;
  rewrite in C for kernel context. gVisor is Apache-2; pattern
  reuse is fine.)
- **Q2**: Nested virt detection — explicit boot probe or runtime
  fallback? (Plan: probe at init; fallback to seccomp if KVM
  unavailable or in nested mode where it's slower than seccomp.)
- **Q3**: How does this interact with host KVM features (KVM
  hypercalls, paravirt clock)? (Plan: minimal — we use KVM as
  the trap mechanism, not as a paravirt environment.)
- **Q4**: SMP — does each vCPU need its own KVM vCPU? (Plan: yes;
  follow KVM standard model.)

## What success looks like

After this workstream:

- prod-fast profile achieves ~100 ns syscall on bare metal
- KVM backend passes the same conformance suite as ptrace and
  seccomp
- Profile defconfigs can select KVM backend
- Documented cost model in `arch/um/Documentation/backends/kvm.rst`

## What failure looks like

- KVM backend works but is slower than seccomp in practice
  (TLB invalidation cost > 100 ns trap savings). Plausible
  outcome on some hardware; in that case, prod-fast falls back
  to seccomp permanently.
- KVM backend can't be made to coexist with KASAN
  cleanly (KASAN's shadow mappings interact badly with EPT).
  Workaround: prod-fast doesn't include KASAN (it doesn't anyway).
- Nested-virt detection is unreliable; users in unexpected
  environments hit perf cliffs. Workaround: explicit boot
  param, default to seccomp.

## Reference

- gVisor KVM platform source:
  https://github.com/google/gvisor/blob/master/pkg/sentry/platform/kvm/machine_amd64.go
- gVisor platform guide:
  https://gvisor.dev/docs/architecture_guide/platforms/
- KVM API documentation:
  https://docs.kernel.org/virt/kvm/api.html
