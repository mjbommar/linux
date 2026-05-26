# Workstream D: KVM backend

**Effort:** ~6 engineer-months
**Owner role:** Hypervisor engineer (familiar with KVM internals)
**Critical path:** No — parallel to C; depends only on A
**Bookend:** `getpid()` round-trip through KVM backend in <100 ns
            measured on bare metal

## Memo status (2026-04-27 post Stage A)

The 03-architecture-review-2026-04-27/ identified the singleton
vcpu0_fd as the architectural defect driving the cpython-parity
gate flakiness. Stage A landed (commit `7f94922a356f`), retiring
the singleton model. Several earlier memos that tracked iterative
bug-hunts within the singleton model are now SUPERSEDED:

| Memo                                | Status                                |
|-------------------------------------|---------------------------------------|
| 16-architecture-review/             | SUPERSEDED — the per-task vCPU + KVM_SET_SIGNAL_MASK answer landed in Stage A; agent-1 thru agent-4 in 03-architecture-review-2026-04-27 reach the same diagnosis with the structural plan |
| 17-multi-task-investigation/        | SUPERSEDED — Phase A-K fixes (memory ordering / FPU UAF / VCPU_EVENTS leak / dirty-flag cmpxchg etc) made the singleton model "less wrong" but couldn't remove the contract violation. Stage A removes it structurally. |
| 18-architectural-fix-plan/          | SUPERSEDED — Phase 1-3 partial-per-mm work was an incremental step toward what Stage A delivered as per-task. Per-mm IRETQ frame migration is moot; Stage B's kernel-half PGD provides per-vCPU IRETQ slots structurally. |
| 19-next-investigation-playbook/     | SUPERSEDED — Race classes A and E are closed by Stage A (per-task vCPU + KVM_SET_SIGNAL_MASK). Race classes B/C/D are Stage B's domain (TDP/EPT + memslots-per-mm — see 20-stage-b-design.md). |
| 09-shadow-pt.md                     | SUPERSEDED-PENDING — shadow PT design memo; the apparatus it describes will be deleted by Stage B (B.7-B.11). Kept for archaeology of the original D-04/D-05 design. |
| 15-direct-shadow-sync.md            | SUPERSEDED-PENDING — direct shadow sync (set_pte_at hook) is the data-path producer of Stage B's deletion. |
| 20-stage-b-design.md                | ACTIVE — current. The TDP/EPT + memslots-per-mm plan that follows Stage A. |

Memos 01-08, 10-14 are still active design docs that describe the
backend's mechanism (LSTAR trap / ring transition / etc.) and are
not affected by the Stage A→B redesign.

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

The piece *not yet* landed is **real `run_userspace`
integration** — replacing the one-shot harness with a
sustained trap loop driving genuine UML guest processes.
Tracked as task #162. Memo `08-real-run-userspace.md`
(2026-04-23) decomposes it; **memo `09-shadow-pt.md`
(2026-04-24) scopes the shadow-page-table prerequisite that
D66 identified as the fundamental architectural blocker.**
Sub-commits #1–#5a landed during the 2026-04-24 session —
state materialization, LSTAR trampoline + MSRs, KVM_RUN loop
+ syscall decode, MMIO decode + fault routing, ring-3 entry
SYSRETQ bootstrap. Memo 08's #4/#5b/#6/#7 and D-06's
`getpid()` bookend are all blocked on the shadow PT (memo 09,
task #186) because UML's pgd encoding is software-only and
not hardware-walk-compatible.

Decisions-log coverage: D49–D57 + D60–D61 walk through the
shape choices; D63 sequences the upstream submission; D65
scopes task #162; **D66 records the shadow-PT finding**.
`04-ring-transition.md`, `04b*-*.md`,
`07-systrap-gadget-feasibility.md`,
`08-real-run-userspace.md`, and `09-shadow-pt.md` carry the
per-sub-task detail.

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
