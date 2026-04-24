# D-04/D-05 follow-on: real `kvm_run_userspace` integration

**Status:** design memo (2026-04-23) — scopes the transition
from the late_initcall harness to the integrated
`run_userspace` hot path.
**Effort:** 2-3 weeks (several sub-commits; no single commit
is larger than a workstream-B lift).
**Dependencies:** D-04c (landed, LSTAR trampoline), D-04b.2b.2
(landed, UML-kernel-text execution), D-05a (landed, time ops),
D-05b (landed, ipi_send), Phase III Lifts #1b-#1f (landed,
ring-3 SYSRETQ, MMIO decode, IDT injection, A-05 contract pass).
**Blocks:** D-06 bookend benchmark (`getpid()` <100 ns on bare
metal); workstream D ship.
**Task:** #162.
**Sub-task of:** `04-ring-transition.md` — the "remaining for the
naive-backend phase-1" bullet titled "D-05 full" + everything
hidden behind today's `panic()` in `thread.c::kvm_run_userspace`.

## Goal

Replace the D-04a scaffold panic at
`arch/um/backend/kvm/thread.c:165` with an integrated loop that
makes `backend=kvm` actually execute real UML userspace
processes, not just harness microbenchmarks. When this lands,
workstream D is code-complete up to D-06's bookend.

**Today:**

```c
void kvm_run_userspace(struct uml_pt_regs *regs)
{
    ...
    rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
    panic("um: kvm run_userspace: ioctl rc=%d, exit_reason=%u (%s) — "
          "D-04b SREGS/CR3 setup pending", ...);
}
```

**After this lift:**

```c
void kvm_run_userspace(struct uml_pt_regs *regs)
{
    kvm_enter_guest(regs);          // SREGS + CR3 + GP regs from regs
    for (;;) {
        rc = KVM_RUN;
        switch (run->exit_reason) {
        case KVM_EXIT_IO:           return kvm_decode_syscall(regs, run);
        case KVM_EXIT_MMIO:         handled = kvm_decode_mmio(regs, run); continue;
        case KVM_EXIT_HLT:          kvm_handle_hlt(regs);      return;
        case KVM_EXIT_INTR:         /* host signal, reinject + continue */
        case KVM_EXIT_FAIL_ENTRY:   panic / fail;
        ...
        }
    }
}
```

Every primitive in that switch has a landed harness
counterpart. The lift's job is to lift them out of
`harness.c` (1526 LOC spike code) into per-function
production modules and wire them to `struct uml_pt_regs`.

## Decomposition

Each sub-commit is independently testable + mergeable. Order
follows data-flow: set up the guest, run it, decode exits,
return state.

### #1 — `kvm_enter_guest` (vCPU state materialization)

**Delta:** `thread.c` grows a `kvm_enter_guest(regs)` that:

1. Reads CR3 from `current->active_mm->pgd` (translated through
   the Policy A identity memslot — the machinery is already in
   `mm.c::kvm_ensure_memslot`).
2. `KVM_SET_SREGS` with long-mode setup (lifts
   `kvm_setup_harness_sregs` out of `harness.c` into
   `sregs.c::kvm_setup_production_sregs` — same function minus
   the hard-coded stub RIP).
3. `KVM_SET_REGS` populates GP regs from `regs->gp[HOST_*]` and
   seeds RIP from `regs->gp[HOST_IP]` / RSP from
   `regs->gp[HOST_SP]`.
4. `KVM_SET_MSRS` installs LSTAR (→ ring-0 bounce trampoline,
   same gadget D-04c landed).

**Test:** extend the A-05 KUnit suite with
`test_kvm_enter_guest_state` that pokes the vCPU post-enter and
asserts CR3/RIP/RSP match the passed `regs`.

**Code moves:** `harness.c` lines ~900-950 (kvm_setup_harness_sregs)
→ `sregs.c`, keep the harness wrapper as a thin adapter for
the late_initcall spike path.

### #2 — `kvm_decode_syscall` (KVM_EXIT_IO → sys_call_table)

**Delta:** `kvm_decode_syscall(regs, run)`:

1. The IO exit bucket reserved for syscall dispatch is at port
   `0xf4` in the harness (see `harness.c:900+`). Promote that
   to a named constant `UM_KVM_SYSCALL_PORT` in
   `kvm_backend.h`.
2. Read the syscall nr + args from saved vCPU regs via
   `KVM_GET_REGS`, map them into the `regs->gp[]` slots UML's
   syscall framework expects (`HOST_ORIG_RAX`, `HOST_DI`, …).
3. Return. The caller (common syscall dispatch) invokes
   `sys_call_table[nr]` as on every other backend; that path
   is already tested by Phase III Lift #1f's A-05 contract
   pass.

**Test:** A `getpid()` selftest under `backend=kvm`. The A-05
contract suite already asserts dispatch correctness against
the ops table; we're adding end-to-end validation that the
IO-exit decode drops a syscall into the real dispatch table.

**Code moves:** `harness.c` lines ~1030-1050 (1b IO-exit
decode) → `thread.c::kvm_decode_syscall`.

### #3 — `kvm_decode_mmio` (KVM_EXIT_MMIO → fault path)

**Delta:** reuse Phase III Lift #1d's landed MMIO decode
(`harness.c:1200+`). The lift already routes an MMIO exit into
`arch/um/kernel/trap.c::handle_page_fault` equivalent; this
sub-commit just moves the code out of the harness + wires it
through `regs`.

**Test:** Accessing an unmapped guest page from userspace
produces SIGSEGV in the guest process (not a host panic).
Selftest:
`tools/testing/selftests/um/kvm-fault-smoke/` (new, ~40 LOC).

### #4 — `kvm_handle_hlt` (KVM_EXIT_HLT → idle path)

**Delta:** when a guest userspace process executes a long
`pause` or when the UML kernel itself halts (`cpu_idle()`
calls `safe_halt`), KVM_RUN returns `KVM_EXIT_HLT`. Translate
that to returning from `run_userspace` so UML's scheduler can
run another task. Re-entering the loop on the next schedule
is a fresh KVM_RUN.

**Test:** `yield`-loop in a guest userspace process that
makes progress (100 iterations in <10 ms); verifies the
scheduler regains control between KVM_RUNs.

### #5 — `kvm_handle_intr` (KVM_EXIT_INTR → signal reinject)

**Delta:** host-side SIGALRM / SIGIO / SIGCHLD delivered
during KVM_RUN surface as `KVM_EXIT_INTR`. Let the host
signal handler run (UML's own signal dispatch), then re-enter
KVM_RUN. Subtlety: the guest's own signal delivery (via IDT
injection) is a *different* path, already landed in Phase III
Lift #1e.

**Test:** a guest userspace process spinning in a loop while
a host-side `timer_fn` expires. Ensure UML's timer IRQ fires
without the guest needing to yield.

### #6 — hot-path optimizations

Once #1-#5 are in and the A-05 suite + new smoke tests pass
on `backend=kvm`, measure against the D-06 bookend target.
Expected path-length from current harness measurements:

- LSTAR round-trip (spike 07): ~270 cyc boost-locked.
- IO-exit syscall decode adds ~800-1200 cyc for VMEXIT.
- Total expected: ~1000-1500 cyc syscall → ~300-450 ns on
  4 GHz silicon.

If we land within a 2× margin of D-06's <100 ns target, the
gadget feasibility memo (07-systrap-gadget-feasibility.md,
D61) takes over as the next optimization phase.

### #7 — Fallback wiring (D-05 completion)

Separate sub-lift but tightly coupled: if
`KVM_EXIT_FAIL_ENTRY` or repeated `KVM_EXIT_INTERNAL_ERROR`
fires, currently we panic. Replace with the nested-virt
fallback path already scoped in
`05-nested-virt-fallback.md`: re-probe, log, fall back to
seccomp for the affected thread. Keep prod-fast's "KVM failure
acceptable" invariant (the plan's stated D-failure-mode).

## Risks + mitigations

- **CR3 ↔ mm lifetime.** Guest CR3 points at
  `current->active_mm->pgd`. If the mm is freed while KVM_RUN
  is blocking, KVM reads freed memory. D-03b's `mm_attach`/
  `mm_detach` already owns the mm reference; the lift just
  needs to ensure `kvm_enter_guest` runs under the same mm
  context the caller expects. **Mitigation:** borrow the
  ptrace backend's mm-pinning pattern (via `get_task_mm`) for
  the KVM_RUN duration; release on exit.

- **Signal races.** Host signal during KVM_RUN → EINTR/INTR
  exit. If the signal handler re-enters `run_userspace`
  recursively before the outer ioctl returns, we stack. **
  Mitigation:** block SIGALRM/SIGIO/SIGCHLD around KVM_RUN;
  delivery happens between iterations, same pattern the
  seccomp backend uses for its trap loop.

- **KVM ABI variance.** KVM_API_VERSION bumps or kernel-side
  struct layout changes could break us. **Mitigation:** the
  existing D-03a `/dev/kvm` probe already checks
  `KVM_GET_API_VERSION`; expand it to fail-fast with a clear
  "KVM API mismatch, falling back" message rather than a
  runtime panic.

- **Nested-virt slowness.** On some hosts nested KVM is slower
  than seccomp. D-05 is the formal mitigation; this lift
  should wire the detection hook even if the full
  auto-fallback logic lands separately.

- **`struct pt_regs` ↔ `uml_pt_regs` impedance.** UML's
  per-backend register struct layout differs from bare-metal
  x86. The harness already handles this for the spike cases;
  the lift needs to add a centralized
  `kvm_regs_to_uml(regs, kvm_regs)` / `uml_to_kvm_regs(...)`
  helper so per-exit-reason code doesn't replicate the
  marshalling.

## Validation strategy

### Selftests

New (in `tools/testing/selftests/um/`):

- `kvm-smoke/` — boot UML with `backend=kvm`, exec
  `/bin/true`, assert clean exit. Exercises #1-#2 end-to-end.
- `kvm-fault-smoke/` — guest userspace dereferences an
  unmapped page, assert SIGSEGV (not host panic).
- `kvm-yield-smoke/` — 100 `sched_yield()` from guest,
  assert UML scheduler progresses.

Existing (extend):

- `userspace-smoke/` — already runs under seccomp; add a
  `backend=kvm` variant of the same workload.
- A-05 contract KUnit suite — already passes under
  `CONFIG_UM_BACKEND_KVM_ONLY` (Phase III Lift #1f); extend
  to exercise the integrated path, not only the op-table
  dispatch.

### Benchmarks

- `getpid()` round-trip cycle count (D-06 bookend). Target
  <100 ns bare metal; acceptable 2× margin for v1 with the
  gadget retrofit (memo 07) tracked as the follow-on
  optimization.
- Boot time to userspace under `backend=kvm` (matching the
  seccomp baseline from C-10 launcher-smoke).

### Regression safety

Every sub-commit runs:

1. `make ARCH=um` clean build, all three backends selectable.
2. A-05 KUnit contract under `UM_BACKEND_*_ONLY` for all three
   single-backend builds.
3. `launcher-smoke` + `userspace-smoke` + `umlctl-smoke`
   selftests under their current backends (default + fuzz +
   research).
4. `backend=kvm` sub-lifts land behind a `default n`
   `CONFIG_UM_BACKEND_KVM_INTEGRATED` until #6 measurements
   clear the D-06 gate; the harness path stays available via
   `CONFIG_UM_BACKEND_KVM_HARNESS=y` for regression probes.

## Open questions to resolve during implementation

1. **Single-vCPU vs per-thread vCPU.** Today the harness
   re-uses vcpu0 for every spike. Real `run_userspace` is
   called per-task; do we stick with one vCPU serialized
   through a mutex, or open a vCPU per-UML-task? gVisor does
   per-task. Cost trade-off: KVM vCPU allocation is ~1 ms
   one-shot, serialization is ~0.5 µs per entry. For UML's
   single-threaded-per-mm model, one-vCPU is probably right.
   **Resolution:** decide in sub-commit #1; single-vCPU with
   per-mm serialization is the default recommendation.

2. **`current` during KVM_RUN.** When KVM_RUN is blocking in
   the host kernel, `current` still points at the UML task
   that called it. But from the UML kernel's perspective,
   *who* is `current` — the host task, or the guest task?
   Today UML conflates them (same `task_struct`). KVM_RUN
   needs to preserve that invariant so signal delivery
   doesn't get confused. **Resolution:** seccomp backend's
   current handling is the reference; replicate the pattern.

3. **Memslot lifetime vs process lifetime.** The current
   lifecycle registers one giant memslot on first KVM_RUN.
   For long-lived UML processes that exec different binaries,
   we'd want `mm_map`/`mm_unmap` (D-03d) to update the
   memslot view. It already does for single-page updates.
   **Resolution:** lands as part of #3 (MMIO decode).

## Cross-references

- `thread.c::kvm_run_userspace` — the panic this memo
  replaces.
- `harness.c` — spike code the sub-commits lift from.
- `04-ring-transition.md` — sibling D-04 memo; this one picks
  up from its "Remaining for the naive-backend phase-1"
  bullet list.
- `05-nested-virt-fallback.md` — D-05 fallback path (#7).
- `06-conformance.md` — D-06 bookend; this lift unblocks it.
- `07-systrap-gadget-feasibility.md` — post-v1 optimization
  via D61's GO decision.
- `tools/testing/selftests/um/` — existing selftest harness
  model for new kvm-*-smoke tests.
- decisions-log entries D49-D57 + D60-D61 — D-workstream
  rationale + gadget disposition; D65 (next entry) will
  record this memo's scope + sub-commit plan.

## Status flip criteria

This memo leaves "design memo" and becomes "implementation
in flight" when:

1. Sub-commit #1 (kvm_enter_guest) lands with its A-05 test
   extension.
2. Decisions-log D65 records the implementation kickoff with
   owner + target quarter.
3. `tools/testing/selftests/um/kvm-smoke/` directory exists
   with a run-script.

When sub-commits #1-#6 all land + D-06 bookend runs clean,
this memo rolls up into `04-ring-transition.md`'s
"completed" block and `06-conformance.md` picks up the
bookend narrative.
