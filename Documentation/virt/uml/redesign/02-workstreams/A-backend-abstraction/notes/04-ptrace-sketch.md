# A-01.4 — Ptrace backend sketch

Date: 2026-04-17
Owner: claude-code session
Inputs: A-01.0 (survey), A-01.1 (inventory), A-01.2 (categories),
A-01.3 (`arch/um/include/asm/backend.h`).

## Goal

Map every op in `struct um_backend_ops` to its ptrace implementation,
sourced from existing skas/ptrace code or marked as needing a new
glue function. **No new C source yet** — this is the validation pass
to confirm the table is implementable without per-backend extensions
or side channels.

## Per-op mapping

Legend:
- **EXIST** — function already exists; rename or thin wrap.
- **WRAP**  — exists but needs a small glue wrapper for the new signature.
- **EXTRACT** — exists inside a `using_seccomp` branch in shared code; lift the ptrace branch out into its own function.
- **NEW**   — small new helper required.

### Lifecycle and trap

| Op | Existing ptrace impl | Action | Source location(s) |
|---|---|---|---|
| `probe` | `start_up.c::start_ptraced_child()` (lines 41–144 — the big "can we ptrace at all" boot probe) | EXTRACT | os-Linux/start_up.c:41 |
| `init` | one-time setup is implicit today (signal handlers in `init_new_thread_signals`, ptrace TLS setup) | NEW (gather existing scattered init into one entry point) | os-Linux/process.c, os-Linux/signal.c |
| `shutdown` | `halt_skas` / `reboot_skas` | WRAP (drop the `_skas` name; ptrace_shutdown calls existing) | os-Linux/skas/process.c:881,903 |
| `run_userspace` | `userspace()` — *the* trap loop, but the whole `if (using_seccomp)` ptrace branch (lines 647–741) is what we want | EXTRACT | os-Linux/skas/process.c:546–784 |

### Memory

| Op | Existing ptrace impl | Action | Source location(s) |
|---|---|---|---|
| `mm_attach` | `start_userspace()` ptrace branch (lines 489–514: waitpid + PTRACE_SETOPTIONS) | EXTRACT | os-Linux/skas/process.c:442 |
| `mm_detach` | `os_kill_ptraced_process(pid, 1)` | WRAP (rename, keep impl) | os-Linux/process.c:53 |
| `mm_map` | `map()` calling `do_syscall_stub()` ptrace branch (lines 99–115 of skas/mem.c) | EXTRACT | os-Linux/skas/mem.c:234 + 99–115 |
| `mm_unmap` | `unmap()` — symmetric | EXTRACT | os-Linux/skas/mem.c:267 |

### Scheduling

| Op | Existing ptrace impl | Action | Source location(s) |
|---|---|---|---|
| `thread_create` | `new_thread()` — sets up jmp_buf + handler entry | WRAP | os-Linux/skas/process.c:793 |
| `thread_start_idle` | `start_idle_thread()` | WRAP | os-Linux/skas/process.c:819 |
| `context_switch` | `switch_threads()` (longjmp/setjmp pair) | WRAP (signature change: take task_struct *, call switch_threads on `task->thread.switch_buf` internally) | os-Linux/skas/process.c:805 |
| `ipi_send` | `os_send_ipi(cpu, vector)` (sends SIGRT to host thread) | WRAP | os-Linux/smp.c:92 |

### Time

| Op | Existing ptrace impl | Action | Source location(s) |
|---|---|---|---|
| `read_clock_ns` | `os_nsecs()` (clock_gettime CLOCK_MONOTONIC_RAW) | WRAP | os-Linux/time.c:103 |
| `set_timer` | dispatch on `mode`: `os_timer_set_interval` / `os_timer_one_shot` / `os_timer_disable` | NEW (small `switch (mode)` wrapper around the existing 3 functions) | os-Linux/time.c:61,77,95 |
| `read_persistent_clock_ns` | `os_persistent_clock_emulation()` | WRAP | os-Linux/time.c |

### Debug

| Op | Existing ptrace impl | Action | Source location(s) |
|---|---|---|---|
| `init_thread_regs` | `get_safe_registers(gp, fp)` | WRAP (rename) | os-Linux/registers.c:37 |
| `read_guest_regs` | `ptrace(PTRACE_GETREGS, pid, …)` (currently inline in trap loop, lines 700–704) | EXTRACT into a helper called by both KGDB future code *and* the trap loop | os-Linux/skas/process.c:700 |
| `write_guest_regs` | `ptrace(PTRACE_SETREGS, pid, …)` (line 669) | EXTRACT (symmetric) | os-Linux/skas/process.c:669 |

## Side-channel check

Per the gating rule in `01-ops-table.md`:

> If either implementation requires "and a side channel for X", the
> ops table is wrong.

Walking the ptrace impl above, the only data passed between ops is via:

1. **`struct mm_id`** — the per-mm handle. Carries `pid`, `stack`. Both
   ops table-visible and required by ptrace. ✅ no side channel.
2. **`struct uml_pt_regs`** — the per-thread register file with
   embedded `faultinfo`. The trap loop populates it from
   `PTRACE_GETREGS` + `get_skas_faultinfo`. Kernel reads it. ✅ no
   side channel.
3. **`task_struct->thread.switch_buf`** — the jmp_buf for context
   switch. Lives inside `task_struct`, not on the side. ✅
4. **`exec_regs[]` / `exec_fp_regs`** — boot-time-captured ptrace
   register template, used by `get_safe_registers`. Module-private
   global; not crossing op boundaries. ✅

**Verdict: ptrace fits the contract with no side channels.**

## Files that move into `arch/um/backend/ptrace/` (per A-02)

Refactor target structure:

```
arch/um/backend/ptrace/
├── Makefile
├── ptrace_backend.c         /* struct um_backend_ops ptrace_ops = { ... }; */
├── trap.c                   /* run_userspace, get_skas_faultinfo, handle_trap */
├── mm.c                     /* mm_attach (ptrace branch of start_userspace),
                                mm_detach, mm_map, mm_unmap (ptrace branches
                                of do_syscall_stub) */
├── thread.c                 /* thread_create, thread_start_idle,
                                context_switch — all the jmp_buf machinery */
├── time.c                   /* set_timer dispatch wrapper */
├── debug.c                  /* read_guest_regs, write_guest_regs */
├── probe.c                  /* the boot ptrace probe extracted from start_up.c */
└── stub_load.c              /* userspace_tramp ptrace half */
```

Note: `arch/um/os-Linux/skas/` *as a directory* goes away. Its
contents split between the two backend dirs and the shared host
services directory (`arch/um/os-Linux/` proper, which keeps
`os_*` for file/sigio/irq/futex/timer plumbing).

Estimated LoC migrated into ptrace backend: ~700 (the ptrace branches
of `userspace()`, the ptrace half of `start_userspace`, ptrace half of
`do_syscall_stub`, plus jmp_buf machinery from `switch_threads` etc.).

## Cost-model notes

The ptrace backend's `run_userspace` cost is dominated by:

- `PTRACE_SYSEMU + waitpid + PTRACE_GETREGS + PTRACE_SETREGS` per
  syscall round-trip: ~4 syscalls × ~300 ns = ~1.2 µs minimum.
- Add 2 host signals per page fault for SIGSEGV plumbing.

These are the ~1–5 µs syscall overhead from `00-vision.md`. The op
abstraction adds ~5 cycles indirect call in multi-backend builds; in
single-backend ptrace-only build it's a direct call (zero overhead).

Document this cost model in `arch/um/backend/ptrace/README.md` per the
contract requirement in `three-layers.md`.

## Things that change for ptrace

1. `using_seccomp = 0` branches are extracted into ptrace impls,
   removing the runtime check.
2. `os_kill_ptraced_process` becomes `ptrace_mm_detach` for the
   per-mm case; standalone helper-process kills keep
   `os_kill_process` (already exists, generic).
3. `start_userspace` is split in half: the ptrace half goes into
   `arch/um/backend/ptrace/mm.c::ptrace_mm_attach`; the seccomp half
   goes into `arch/um/backend/seccomp/mm.c::seccomp_mm_attach`.

## Validation criterion

Per A-01 spec:
> Three implementation sketches (ptrace, seccomp, KVM) fit the
> contract without per-backend extensions to the core ops.

This sketch shows ptrace fits: every op maps to existing or trivially
new code, no side channels needed.

Confirmed for ptrace. Seccomp sketch: A-01.5. KVM sketch: A-01.6.

## Open questions specific to ptrace impl (defer to A-02)

- **Q-ptrace-1:** The boot ptrace probe in `start_up.c:41–244` is huge
  and does host capability sniffing (PTRACE_SYSEMU support, etc.).
  Does `ptrace_probe()` block until full probe completes, or can it be
  split into "fast probe" (can we even ptrace?) + "deep probe"
  (capability matrix)? Performance impact is small either way.
- **Q-ptrace-2:** Should `ptrace_run_userspace` use `PTRACE_SYSEMU`
  (current) or `PTRACE_SYSCALL`? Today: SYSEMU lets the kernel emulate
  the syscall without the guest actually executing it. Stays SYSEMU.
- **Q-ptrace-3:** Singlestepping is currently observed via
  `singlestepping()` (TIF_SINGLESTEP). Does the ops table need
  `set_singlestep` as per A-01.2 considered? Verdict deferred — likely
  no, the flag-based mechanism works for both ptrace and seccomp.

## Next subtasks

- A-01.5: same exercise for seccomp.
- A-01.6: same for KVM (a forward-looking sketch since no code yet).
- A-01.7: walk the inventory and verify every site has a destination.
