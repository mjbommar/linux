# The three layers, in detail

This is the architectural backbone. Every other document in
this plan depends on these layers behaving as described.

> **Current state (updated 2026-04-18):** Layer 1 is implemented
> and shipping; see `arch/um/include/shared/backend.h` for the
> authoritative struct definition and `Documentation/virt/uml/
> backend-contract.rst` for the semantics spec. The Layer 1
> section below retains the original design narrative. Where the
> pre-implementation sketch and the delivered contract diverge
> (some op renames, slightly different args block, no NULL/-ENOSYS
> fallback), the delivered shape wins; see notes inline below.
> Layers 2 and 3 are still design descriptions — workstreams B
> and C will deliver them.

## Layer 1: Backend ops table

### The struct

The delivered `struct um_backend_ops` lives in
`arch/um/include/shared/backend.h` (shared rather than `asm/`
because USER-side TUs in `arch/um/os-Linux/` need to deref it in
DYNAMIC builds — see D11 in `04-risks/decisions-log.md`). It has
18 ops in 5 categories; see `Documentation/virt/uml/backend-contract.rst`
for per-op semantics. The summary:

```c
/* arch/um/include/shared/backend.h (excerpt; see file for full def) */

struct um_backend_args {
    enum um_backend_kind requested;  /* 0 = auto */
    bool                 force;      /* panic if requested unavailable */
    const char          *runtime_opts;
};

struct um_backend_ops {
    const char           *name;
    enum um_backend_kind  kind;
    u32                   contract_version;

    /* Lifecycle and trap (4) */
    int  (*probe)(void);
    int  (*init)(const struct um_backend_args *args);
    void (*shutdown)(void);
    void (*run_userspace)(struct uml_pt_regs *regs);       /* HOT */

    /* Memory (4) */
    int  (*mm_attach)(struct mm_id *id);
    void (*mm_detach)(struct mm_id *id);
    int  (*mm_map)(struct mm_id *id, unsigned long va,     /* HOT */
                   unsigned long len, int prot,
                   int phys_fd, u64 offset);
    int  (*mm_unmap)(struct mm_id *id,                     /* HOT */
                     unsigned long va, unsigned long len);

    /* Scheduling (4) */
    int  (*thread_create)(struct task_struct *p,
                          void *stack, void (*handler)(void));
    int  (*thread_start_idle)(void *stack, struct thread_struct *t);
    void (*context_switch)(struct task_struct *prev,       /* HOT */
                           struct task_struct *next);
    int  (*ipi_send)(int cpu, int vector);

    /* Time (3) */
    u64  (*read_clock_ns)(void);                           /* HOT */
    int  (*set_timer)(int cpu, u64 deadline_ns,
                      enum um_timer_mode mode);
    u64  (*read_persistent_clock_ns)(void);

    /* Debug / introspection (3) */
    void (*init_thread_regs)(unsigned long *gp, unsigned long *fp);
    int  (*read_guest_regs)(struct task_struct *t, struct pt_regs *regs);
    int  (*write_guest_regs)(struct task_struct *t,
                             const struct pt_regs *regs);
};
```

Notable deviations from the original sketch (all captured as
D-entries in `04-risks/decisions-log.md`):

- `syscall_dispatch` + `page_fault` folded into `run_userspace`
  (one trap-loop iteration; faultinfo travels on regs).
- `map_user`/`unmap_user` renamed `mm_map`/`mm_unmap` and take
  `struct mm_id *` rather than `struct mm_struct *`.
- `host_io_submit` dropped — virtio-uml uses shared host-service
  APIs (`os_*_file`/epoll) directly; no backend abstraction needed.
- `set_timer` takes a mode tag (`UM_TIMER_{DISABLE,ONE_SHOT,PERIODIC}`)
  instead of being three separate ops.
- Lifecycle ops added: `probe`, `init`, `shutdown` (lifecycle
  unification into the ops table is in progress; boot probes
  currently still live in `os_early_checks()`).
- `read_guest_regs`/`write_guest_regs` take task pointers (not
  cpu int) — better fit for KGDB's per-task semantics.

### The selection mechanism

Two builds:

**Single-backend, inlined.** `CONFIG_UM_BACKEND_SECCOMP=y` and no
others. Then:

```c
/* arch/um/include/asm/backend.h */
#if defined(CONFIG_UM_BACKEND_SECCOMP_ONLY)
# define um_backend_dispatch(op, ...) seccomp_##op(__VA_ARGS__)
#elif defined(CONFIG_UM_BACKEND_KVM_ONLY)
# define um_backend_dispatch(op, ...) kvm_##op(__VA_ARGS__)
#elif defined(CONFIG_UM_BACKEND_PTRACE_ONLY)
# define um_backend_dispatch(op, ...) ptrace_##op(__VA_ARGS__)
#else
# define um_backend_dispatch(op, ...) um_backend->op(__VA_ARGS__)
#endif
```

In single-backend builds the backend functions are direct calls
(0 indirect-call overhead, ~5 cycles savings, smaller binary).
Sandbox profile uses this — minimum TCB, no flexibility paid for.

**Multi-backend, indirect.** `CONFIG_UM_BACKEND_DYNAMIC=y` and at
least two of the three backends compiled in. Then `um_backend` is
selected at boot by `init_backend()` based on:

1. Boot param `backend=ptrace|seccomp|force=<kind>`.
2. `backend=auto` (default) defers to the `seccomp=` legacy alias
   (preserves prior default: ptrace unless seccomp was requested).
   A future release may flip this to "prefer seccomp if available"
   (see D15; maintainer-visible behavior change, held for now).
3. `force=` panics if the requested backend isn't compiled in or
   its probe fails.

Indirect calls cost ~5-10 cycles (modern CPUs predict them well
when the target is stable, which ours is — `um_backend` doesn't
change after init).

### The contract

Every backend must:

- Populate every op in `um_backend_ops` — no NULL fields.
  (Cold ops that aren't yet meaningful return `-EOPNOTSUPP` for
  int-returning ops; see D14. The dispatch macro is a plain
  function-pointer call and can't synthesize `-ENOSYS`.)
- Be self-contained: `#include` no other backend's headers.
- Document its cost model in `arch/um/backend/<kind>/README.md`
  (or inline in the backend TU comments).
- Pass the backend conformance test suite (see
  `02-workstreams/A-backend-abstraction/05-contract.md` and the
  KUnit suite under `arch/um/backend/contract/`).

### What lives in Layer 1 vs above

In Layer 1: anything that depends on the trap mechanism. Syscall
entry/exit, page fault delivery, IPI mechanism, low-level context
switch, hardware-clock read.

NOT in Layer 1: anything that depends on what we *do* with the
trap. Tracing, coverage, scheduling policy, time-travel logic,
device emulation. Those are layer 2 (gated) or arch-neutral kernel
code (untouched).

## Layer 2: Static-key hot-path gates

### The pattern

```c
/* arch/um/include/asm/um-hooks.h */

#include <linux/static_key.h>

DECLARE_STATIC_KEY_FALSE(um_trace_syscalls);
DECLARE_STATIC_KEY_FALSE(um_kcov_enabled);
DECLARE_STATIC_KEY_FALSE(um_time_travel_active);
DECLARE_STATIC_KEY_FALSE(um_kfence_sample);
DECLARE_STATIC_KEY_FALSE(um_record_replay);
DECLARE_STATIC_KEY_FALSE(um_perf_dispatch);
DECLARE_STATIC_KEY_FALSE(um_sanitize_paranoid);

static __always_inline void um_on_syscall_entry(struct pt_regs *regs)
{
    if (static_branch_unlikely(&um_trace_syscalls))
        __um_trace_syscall_entry(regs);
    if (static_branch_unlikely(&um_kcov_enabled))
        __um_kcov_record(regs->ip);
    if (static_branch_unlikely(&um_record_replay))
        __um_record_event(EV_SYSCALL_ENTRY, regs);
    if (static_branch_unlikely(&um_perf_dispatch))
        __um_perf_event(PERF_SYSCALL, regs->orig_ax);
    /* hot path falls through with N JITted NOPs */
}

static __always_inline void um_on_syscall_exit(struct pt_regs *regs)
{
    if (static_branch_unlikely(&um_trace_syscalls))
        __um_trace_syscall_exit(regs);
    if (static_branch_unlikely(&um_record_replay))
        __um_record_event(EV_SYSCALL_EXIT, regs);
}
```

### Cost model

| Gate state | Cost per gate | Notes |
|---|---|---|
| Off, JITted | ~0.3 ns | One 5-byte NOP |
| Off, before JIT init | ~1 ns | Branch predicted, well |
| On, hook trivial | ~10-50 ns | KCOV: ~10 ns; trace: ~50 ns |
| On, hook expensive | ~100-500 ns | record-replay: ~200 ns |

Five gates, all off: ~1.5 ns total per syscall. Compare to
~100 ns trap cost on KVM backend: layer 2 adds <2% overhead when
all hooks are off.

Five gates, all on: ~150-1000 ns added per syscall. Acceptable in
research/fuzz profiles where you've explicitly opted in.

**Selective tracing**: gates can be combined with per-syscall
filters (bpftrace-style). Trace gate flips on for `openat` only,
other syscalls untraced. Cost: 50 ns added on `openat`, 0.3 ns
added on everything else. This is the killer property — you don't
have to choose between "fast" and "observable" because observability
is a runtime decision per-event.

### Where the gates live

| Hook | Inserted at | Why |
|---|---|---|
| `um_on_syscall_entry` | Every backend's `run_userspace` | Single point of all guest syscalls |
| `um_on_syscall_exit` | Same | Symmetric |
| `um_on_page_fault` | Every backend's `run_userspace` (faultinfo path) | Memory access trace |
| `um_on_context_switch` | Every backend's `context_switch` | Schedule trace |
| `um_on_irq_entry` | Generic IRQ entry path | Interrupt trace |
| `um_on_clock_read` | `read_clock_ns` | Time-travel hook |

### Runtime control

```
/sys/kernel/debug/um/
├── backend                          ro   "kvm" | "seccomp" | "ptrace"
├── hooks/
│   ├── trace_syscalls               rw   0|1
│   ├── kcov_enabled                 rw   0|1
│   ├── time_travel_active           rw   0|1
│   ├── kfence_sample                rw   0|1
│   ├── record_replay                rw   0|1
│   ├── perf_dispatch                rw   0|1
│   └── sanitize_paranoid            rw   0|1
├── trace_syscall_filter             rw   "openat,read,write" or "*"
├── kcov_buffer_size                 rw   bytes
└── stats                            ro   per-hook cost summary
```

### What lives in Layer 2 vs above

In Layer 2: the gates themselves and their toggle infrastructure.
The slow-path implementations of each gate (`__um_trace_syscall_entry`)
live wherever makes sense (kernel/trace/, kernel/kcov.c, etc.) —
they're called by the gate but not part of it.

NOT in Layer 2: the *consumer* of the trace data. ftrace ring
buffers, KCOV's coverage buffer, record-replay's event log are
all separate subsystems that the gate slow paths feed.

## Layer 3: Compile-time wraps

### What this is

This is the existing kernel sanitizer / instrumentation
infrastructure. Nothing new. UML just needs to:

1. `select HAVE_ARCH_KASAN` (already done, x86_64 only — extend)
2. `select HAVE_ARCH_KMSAN` (new)
3. `select HAVE_ARCH_KCSAN` (new)
4. `select HAVE_ARCH_KFENCE` (new)
5. `select HAVE_FUNCTION_TRACER` (new)
6. `select HAVE_KPROBES` (new)
7. `select HAVE_EBPF_JIT` (new)
8. `select HAVE_ARCH_KGDB` (new)

Each is a separable port (see `02-workstreams/C-profiles-and-gaps/`).

### Composition with layers 1 and 2

Layer 3 is **completely orthogonal** to layers 1 and 2. KASAN
wraps memory accesses at compile time; the wrap fires regardless
of which backend is active or which static keys are set. KCSAN
inserts watchpoints in the same way.

This is why the architecture works. The three layers don't fight
each other:

- Layer 1 changes how you trap (~100 ns vs ~1 µs).
- Layer 2 changes what you observe per trap (cheap when off,
  expensive when on, runtime-controlled).
- Layer 3 changes how every memory access is checked (compile-
  time, paid by every access regardless).

## Putting it together

A `prod-fast` syscall:
- Layer 1: KVM trap, ~100 ns.
- Layer 2: 5 gates, all NOP. ~1.5 ns.
- Layer 3: no wraps. ~0 ns.
- Total: ~101 ns. Same source tree as the rest.

A `research` syscall on the same source tree:
- Layer 1: seccomp trap, ~300 ns.
- Layer 2: trace + kprobes gates active, ~80 ns.
- Layer 3: KASAN check on every access in the syscall handler,
  ~5-10 ns/access × 10 accesses = ~50-100 ns.
- Total: ~500 ns. ~5× slower, full visibility.

A `fuzz` syscall mid-fuzz-iteration:
- Layer 1: seccomp trap, ~300 ns.
- Layer 2: KCOV gate active, ~10 ns.
- Layer 3: KASAN, ~50-100 ns.
- Total: ~400 ns. Coverage recorded for the fuzzer.

Same code, three radically different runtime behaviors. That's
the architecture.

## What can go wrong

See `conflicts.md` for the three things that genuinely don't
compose, and `04-risks/` for things that compose in principle
but might break in practice.
