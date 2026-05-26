# A-01.0 — survey of existing skas/seccomp code paths

Date: 2026-04-17
Owner: claude-code session
Inputs read: arch/um/{Kconfig,Makefile-skas}, arch/um/include/shared/os.h,
arch/um/include/shared/skas/{skas,mm_id,stub-data}.h,
arch/um/kernel/{process,trap,tlb,um_arch}.c,
arch/um/kernel/skas/{mmu,process,syscall,uaccess,stub,stub_exe}.c,
arch/um/os-Linux/skas/{mem,process}.c,
arch/um/os-Linux/{process,signal,start_up,main,time,smp}.c

## The headline finding

The current tree **already contains the two backends** the redesign wants
to formalize. They diverge at runtime via a single global flag,
`using_seccomp`, set at boot in `os-Linux/start_up.c` after probing host
seccomp support. There are 17 grep hits for `using_seccomp` across 7
files — that is the entire informal "ops table" today.

| `using_seccomp` site | File | What changes |
|---|---|---|
| `start_up.c:477,488` | os-Linux | initial probe + assignment |
| `signal.c:228` | os-Linux | signal-stack setup |
| `process.c:185` | os-Linux | child process setup |
| `skas/mem.c:47,96,131,201,245` | os-Linux | per-syscall stub-fd plumbing |
| `skas/process.c:298,303,474,487,525,593,751` | os-Linux | trap loop branches |
| `kernel/skas/mmu.c:106` | kernel | mm teardown extra socket close |

A-01's job is therefore *not* to invent a backend abstraction from
nothing — it's to lift this existing branch into a typed `struct
um_backend_ops`, then add KVM as a third entry without changing the
shape of the table.

## Existing layering

There are already three layers, just unnamed:

1. **`os.h` family** — host OS primitives. ~150 `os_*` functions in
   `include/shared/os.h` covering file, mem, process, signal, time, irq,
   sigio, smp. **Backend-neutral**: every backend needs them. These
   become "shared host services," not part of the ops table.
2. **`skas/skas.h` family** — guest-trap primitives. 8 declarations:
   `handle_syscall`, `current_mm_id`, `current_mm_sync`,
   `current_stub_stack`, `using_seccomp`, `new_thread_handler`,
   `initial_jmpbuf_{lock,unlock}`. **Backend-specific today**.
3. **Stub binaries** — `kernel/skas/stub.c` + `stub_exe.c`. Loaded into
   the guest process. Both ptrace and seccomp modes use the same stubs;
   only the trap-delivery mechanism differs.

## Hot paths (where layer-2 static-key gates will land later)

- **Syscall dispatch**: `kernel/skas/syscall.c::handle_syscall()` — 74
  lines, called from both modes via `userspace()` in
  `os-Linux/skas/process.c`. Single chokepoint. **Ideal site for the
  layer-2 hook in workstream B.**
- **Page fault**: `kernel/trap.c::segv_handler()` →
  `segv()` → `handle_page_fault()`. Single chokepoint. Both modes.
- **Context switch**: `kernel/process.c::__switch_to()` →
  `switch_threads()` (longjmp-based). Implementation lives in
  os-Linux/skas; both modes share.
- **Trap loop**: `os-Linux/skas/process.c::userspace()` — 360+ lines,
  the per-thread trap loop. The bulk of the `using_seccomp` branches
  live here. **This is the function the ops table most needs to split.**

## Backend-relevant primitives to lift into `um_backend_ops`

Working list (refined in A-01.2):

| Category | Current symbol(s) | File(s) |
|---|---|---|
| trap.init / shutdown | `start_userspace`, `os_kill_ptraced_process`, futex teardown | os-Linux/skas/process.c, kernel/skas/mmu.c |
| trap.syscall_dispatch | `userspace()`, `handle_syscall()` | os-Linux/skas/process.c, kernel/skas/syscall.c |
| trap.page_fault delivery | `segv_handler` (signal-driven), seccomp-trap variant | kernel/trap.c, os-Linux/signal.c, os-Linux/skas/process.c |
| mm.map / mm.unmap | `map()`, `unmap()`, `syscall_stub_flush`, `syscall_stub_alloc` | os-Linux/skas/mem.c |
| mm.context_init / destroy | `init_new_context`, `destroy_context` | kernel/skas/mmu.c |
| schedule.context_switch | `switch_threads`, `start_idle_thread`, `new_thread` | os-Linux/skas/process.c |
| schedule.ipi_send | `os_send_ipi` (CONFIG_SMP) | os-Linux/smp.c |
| time.read_clock_ns | `os_nsecs`, time-travel hooks | os-Linux/time.c, kernel/time.c |
| time.set_timer | `os_timer_*` | os-Linux/time.c |
| io.host_io_submit | implicit — virtio-uml uses raw `os_*` file ops; no abstraction yet | drivers/* |
| debug.read/write_guest_regs | `get_safe_registers`, `ptrace_setregs` (ptrace-only today) | os-Linux/skas/process.c, kernel/skas/uaccess.c |

That's ~11 categories, suggesting an ops table of roughly 13–15 entries
once trap.init and mm.context_init expand to one each per phase
(create/start/stop/destroy). Within the 15-op target from A-01 Q3.

## Things that look backend-relevant but aren't

- `os.h` file/sigio/irq APIs: host-side, used identically by all
  backends. Stay in os-Linux, don't enter the ops table.
- Time-travel logic in `kernel/skas/syscall.c`: layer-2 (gated)
  behavior, not layer-1 trap mechanism. Will move under a static key in
  workstream B.
- `kernel/skas/uaccess.c::copy_chunk_to_user()` etc: uses `current_mm_id()`
  which is backend-neutral. Stay as-is; ops-table only changes how the
  underlying `map()` is delivered.

## Things missing today that the ops table needs

- **No backend probe API.** `using_seccomp` is set imperatively in
  `start_up.c` after `try_seccomp()`. Needs `(*probe)()` per backend
  and an `init_backend()` arbiter consuming boot params (Q1 in
  01-ops-table.md).
- **No backend-private state hook.** `using_seccomp` plus per-`mm_id`
  fields (`sock`, `syscall_fd_*`) carry seccomp state today. Need to
  decide if these stay on `mm_context` (current shape) or move behind
  the ops table. **Decision deferred to A-01.8 Q2** — leaning toward
  global per-backend singleton + opaque-handle in `mm_context`.
- **No ops-table-level conformance.** Today, "the seccomp path works"
  is validated by booting and running. A-05 (conformance suite) will
  formalize this once the table exists.

## What this enables for the next subtask (A-01.1 inventory)

The inventory pass should grep for:

- All call sites referencing the symbols in the table above.
- All `using_seccomp` / `PTRACE_*` / `secure_computing` mentions in
  arch/um/.
- All `clone(... CLONE_VFORK | CLONE_VM ...)` and stub-loading patterns.
- All reads/writes of `mm_id->{pid,sock,syscall_*}` outside the
  per-backend implementation files.

Estimated landing target: ~50 sites per the spec; 17 are already
flagged by `using_seccomp` alone, the rest will come from the implicit
direct calls to skas-specific functions.

## Out of scope for A-01 (deferred)

- The actual refactor to ops-table dispatch (A-02, 6 wk).
- Single-backend Kconfig inlining (A-04).
- Conformance test infrastructure (A-05).
- Anything in workstream B/C/D.
