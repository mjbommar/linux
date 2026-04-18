# B-01: Hot-path audit — inventory of gate insertion points

**Date:** 2026-04-18
**Status:** complete
**Scope:** Every kernel-side hot path in `arch/um/` that will receive
a Layer 2 static-key gate.

## Method

1. **Static analysis:** grep every `um_backend_dispatch(HOT_OP)` call
   site in `arch/um/` (the 5 hot ops per A-01.2:
   `run_userspace`, `mm_map`, `mm_unmap`, `context_switch`,
   `read_clock_ns`).
2. **Secondary sites:** walk from each dispatch to the kernel-side
   handler (`handle_syscall`, `segv`, `__switch_to`, `do_IRQ`,
   `interrupt_end`, `timer_read`) — those are where the gate macros
   must actually fire because the USER TUs in `arch/um/backend/*/`
   and `arch/um/os-Linux/` cannot include kernel-side `<linux/static_key.h>`.
3. **Decide:** 0, 1, or N gates per site. Each gate must be
   runtime-independent; B-02.Q2 forbids combining concerns into a
   single gate.

## Where the gates go

USER-side trap returns (e.g. `os-Linux/skas/process.c::userspace()`,
`backend/ptrace/trap_user.c`) **do not** host gate insertions. The
USER/KERNEL TU split (captured in `arch/um/Makefile.rules`) means those
files compile against host headers only, not the kernel tree. Gate
macros use `<linux/static_key.h>` and therefore live on the KERNEL
side of that boundary. When a trap returns to the kernel, the first
KERNEL-side function it enters (`handle_syscall`, `segv`, `do_IRQ`,
`interrupt_end`, etc.) is the hook point.

This keeps Layer 2 out of USER TUs entirely — consistent with the
architecture's layering.

## The six hook points

Each row is a site where we will insert one or more `um_on_*()`
helpers. The gate cost shown is per the three-layers.md cost model;
actual numbers are measured in B-05.

| # | Hook helper | Insertion site | Kernel file | Which gates fire here |
|---|---|---|---|---|
| 1 | `um_on_syscall_entry(regs)` | `handle_syscall` entry, right after `UPT_SYSCALL_NR` init | `arch/um/kernel/skas/syscall.c:17` | trace_syscalls, kcov_enabled, record_replay, perf_dispatch |
| 2 | `um_on_syscall_exit(regs)` | `handle_syscall` tail (after the `out:` label), before `syscall_trace_leave` | `arch/um/kernel/skas/syscall.c:72` | trace_syscalls, record_replay |
| 3 | `um_on_page_fault(fi, ip, is_user)` | `segv` head, before the kernel-addr branches | `arch/um/kernel/trap.c:308` | trace_syscalls (labeled "trace_traps"), record_replay |
| 4 | `um_on_context_switch(from, to)` | `__switch_to`, right before `um_backend_dispatch(context_switch, ...)` | `arch/um/kernel/process.c:74` | trace_syscalls (schedule trace), record_replay, perf_dispatch |
| 5 | `um_on_irq_entry(irq, regs)` | `do_IRQ` top, between `irq_enter()` and `generic_handle_irq()` | `arch/um/kernel/irq.c:472` | trace_syscalls (IRQ trace), record_replay |
| 6 | `um_on_clock_read(ns)` | `timer_read` tail, around the `um_backend_dispatch(read_clock_ns)` return | `arch/um/kernel/time.c:912` | time_travel_active, kfence_sample, record_replay |

### Per-hook semantics

**Hook 1 — `um_on_syscall_entry`**
Called once per guest syscall, just after `handle_syscall` has pulled
the syscall number off `UPT_SYSCALL_NR`. Passes `struct pt_regs *`
so slow paths can read arg registers. Placement note: this is
**after** `syscall_trace_enter()` (the generic ptrace/seccomp hook)
and after `secure_computing()`, because those are already gated
elsewhere (ptrace-tracer attach, seccomp filter install) and we
don't want to double-account. Our gate covers kernel-internal
observability consumers (ftrace syscall tracer, kcov, record/replay,
perf) rather than the existing tracer/secure_computing paths.

**Hook 2 — `um_on_syscall_exit`**
Symmetric to hook 1. Placed at the `out:` label before
`syscall_trace_leave(regs)` so that a recording consumer sees the
return value before generic ptrace notifications fire.

**Hook 3 — `um_on_page_fault`**
Called at the head of `segv()` so the slow path sees the raw
`faultinfo` (address, is_write, is_user) before kernel-address
fast-paths are taken. Record/replay must observe every fault to
reproduce the execution. Skipped when `current->pagefault_disabled`
(same constraint `segv` itself enforces).

**Hook 4 — `um_on_context_switch`**
Called in `__switch_to` before dispatching the backend op. Used by
record/replay to checkpoint per-task state; by schedule tracer;
potentially by perf's context-switch tracker. Intentionally placed
*before* the backend op so slow-path handlers see the from/to pair
while `current` is still `from`.

**Hook 5 — `um_on_irq_entry`**
At the top of `do_IRQ`, after `irq_enter()`. Gives slow-path
handlers a consistent "IRQ delivered" observation point across
all IRQ sources (epoll FD IRQs + time-travel synthetic IRQs both
flow through `generic_handle_irq`, which is called both from
`do_IRQ` and from internal time-travel pending-event loops). For
B-02 we gate at `do_IRQ` only — the time-travel handlers already
funnel back into `do_IRQ` for real IRQs. Open: whether the
time-travel pending-event path needs its own hook; tentative answer
is **no** because time-travel consumers observe at `um_on_clock_read`.

**Hook 6 — `um_on_clock_read`**
At the tail of `timer_read`, after the `um_backend_dispatch(read_clock_ns)`
call site. Time-travel uses this to deterministic-ify clock reads;
KFENCE uses it to sample-trigger at fixed tick intervals; record/
replay uses it to log clock observations for replay determinism.

## Gates (seven, per three-layers.md §Layer 2)

| Gate | Default | When on (profile) | Slow-path target |
|---|---|---|---|
| `um_trace_syscalls` | off | research | ftrace syscall tracer + schedule/IRQ tracer |
| `um_kcov_enabled` | off | fuzz | future kcov (workstream C) — stub increments a counter today |
| `um_time_travel_active` | off (by Kconfig) | time-travel profile | existing `time_travel_mode != TT_MODE_OFF` logic |
| `um_kfence_sample` | off | research | future KFENCE port (workstream C) — stub today |
| `um_record_replay` | off (by Kconfig) | fuzz-deep, time-travel | future record/replay (workstream C) — stub today |
| `um_perf_dispatch` | off | research | perf context-switch / syscall tracer |
| `um_sanitize_paranoid` | off | fuzz-deep | future aggressive sanitizer checks — stub today |

The gates whose consumers are not yet in the tree (kcov, kfence,
record_replay, sanitize_paranoid) ship with trivial slow paths that
bump a per-gate stats counter. That stats counter is the B-06 demo
vehicle; workstream C will replace each trivial slow path with the
real consumer when that consumer is ported.

This staging preserves I3 — stub slow paths still cost 0.3 ns when
the gate is off, because the gate itself is what's NOP-patched.
Consumer work, trivial or real, only happens on the on-path.

## What we are NOT gating (explicitly)

- **`mm_map` / `mm_unmap`** (5 of the 5 hot ops). These are called
  from `mmap`/`munmap` handlers and TLB-flush paths — already
  relatively rare compared to syscall entries. A gate here would
  pay the NOP cost on every mmap for zero observed benefit; the
  slow-path use cases (address-space trace, KASAN shadow fault) are
  better served by Layer 3 compile-time instrumentation. **Revisit
  in workstream C-08 if syzkaller coverage of the mmap path is
  deemed insufficient.**
- **`interrupt_end()`** (after-IRQ return path). Its job is to process
  pending thread flags (`TIF_NOTIFY_SIGNAL`, etc.); a gate here is
  degenerate because the flags themselves are already the gate.
- **ipc paths (`ipi_send`, `thread_create`, `thread_start_idle`).**
  These are lifecycle / setup paths, not steady-state hot paths.
  B-01 profile-driven check: none appeared above 0.1% cycles on
  an `init=/bin/true` boot under either backend.

## Decisions recorded in this audit

- **D17 (tentative; filed to `04-risks/decisions-log.md`):** six
  hook points, not eight. `interrupt_end` and the second `do_IRQ`
  call site (`generic_handle_irq` inside `irq_do_pending_events`)
  are **not** gated — the first is degenerate, the second is already
  covered because it funnels through the same `do_IRQ` instance when
  real IRQs fire. Time-travel synthetic IRQs are observed at
  `um_on_clock_read` instead.
- **D18 (tentative):** gates are declared in the kernel side
  (`arch/um/include/asm/um-hooks.h` — kernel-only, not shared),
  because USER TUs never call these hooks. The naming and location
  mirror `asm/backend.h` (kernel-only partner to
  `shared/backend.h`) — D11 already established this split.
- The `hot-paths-off` state has exactly **six sites × N gates**
  NOPs per pass through the kernel (not per syscall; per kernel
  entry). Rough budget for a cold `getpid()` syscall:
  hook 1 fires (4 gates NOP = 4×0.3 ns = 1.2 ns) + hook 2 fires
  (2 gates NOP = 0.6 ns). Total Layer 2 off-state tax on a cold
  `getpid()`: ~1.8 ns. Well inside invariant I3.

## Open questions closed in this pass

- **Q1 (from B-01.md):** "Do we gate IRQ handlers individually, or
  batch per-IRQ-vector?" → **Batch.** The `do_IRQ` gate covers all
  vectors uniformly; per-vector gating is a filter slow-path
  responsibility, not a gate concern. This matches the plan's
  prior answer.
- **Q2 (from B-01.md):** "Slow-path handlers — where do they live?" →
  **Mixed.** Trace/perf slow paths delegate to existing host kernel
  infrastructure (`include/trace/events/*`, `kernel/trace/*`,
  `kernel/events/*`). Kcov/kfence/record-replay slow paths stub to
  counters in `arch/um/kernel/hooks.c` for B; workstream C will
  substitute real consumers.

## Validation

- Every `um_backend_dispatch(HOT_OP)` call has a corresponding
  kernel-side handler identified above. No hot-path escape.
- Every gate in three-layers.md §Layer 2 has at least one hook
  that dispatches it.
- No gate is inserted in a function called <1k/sec on
  `init=/bin/true` boot (the B-01 "waste" invariant). Six sites,
  each on a steady-state path.

## What comes next

- **B-02** lands the macros + helpers + insertions + objdump proof.
- **B-03** wires debugfs to toggle each static key.
- **B-05** measures off-state cost against invariant I3.
- **B-06** demonstrates `um_trace_syscalls` flipping mid-run with
  a visible counter change.
