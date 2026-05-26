# Data flow: a syscall through every layer

Concrete sequences for a single guest `getpid()` call under each
profile. Numbers are rough order-of-magnitude.

## Profile: prod-fast (KVM backend, no hooks, no wraps)

```
Guest userspace process (ring 3, inside KVM guest)
    syscall instruction
        ↓ (hardware: VMEXIT to ring 0 inside the same KVM guest)
        ↓ ~100 cycles (~30 ns)
UML kernel ring-0 handler (still inside KVM guest)
    um_on_syscall_entry(regs)
        ├─ static_branch_unlikely(&um_trace_syscalls)   → NOP
        ├─ static_branch_unlikely(&um_kcov_enabled)     → NOP
        ├─ static_branch_unlikely(&um_record_replay)    → NOP
        ├─ static_branch_unlikely(&um_perf_dispatch)    → NOP
        └─ ~5 NOPs total                                ~1.5 ns
    um_backend_dispatch(run_userspace, regs)
        → kvm_run_userspace (inlined; single-backend build)
        → do_syscall_64(regs)
            → sys_getpid()
                → return current->pid                    ~5 ns
    um_on_syscall_exit(regs)
        ├─ static_branch_unlikely(&um_trace_syscalls)   → NOP
        ├─ static_branch_unlikely(&um_record_replay)    → NOP
        └─ ~3 NOPs total                                ~1 ns
        ↓ (hardware: VMENTER back to ring 3)
        ↓ ~100 cycles (~30 ns)
Guest userspace process

Total: ~70 ns trap + ~5 ns work + ~2.5 ns gates = ~80 ns
Achievable with current hardware. Within striking distance of
gVisor KVM platform's measured ~1200 ns (their Sentry path is
heavier).
```

## Profile: research (seccomp backend, trace+kprobes on, KASAN on)

```
Guest userspace process (a host process)
    syscall instruction
        ↓ (host kernel: SECCOMP_RET_TRAP → SIGSYS to stub)
        ↓ ~200 ns
UML stub (in the same host process)
    SIGSYS handler reads regs from shared memory
        ↓ futex_wake to UML kernel thread
        ↓ ~100 ns
UML kernel thread (separate host process)
    um_on_syscall_entry(regs)
        ├─ static_branch_unlikely(&um_trace_syscalls)   → JMP to slow path
        │   __um_trace_syscall_entry(regs)
        │       record into trace ring buffer            ~50 ns
        ├─ static_branch_unlikely(&um_kcov_enabled)     → NOP
        ├─ static_branch_unlikely(&um_record_replay)    → NOP
        └─ ~80 ns total
    um_backend_dispatch(run_userspace, regs)
        → seccomp_run_userspace (indirect call, multi-backend)
        ~10 ns
        → do_syscall_64(regs)
            → kprobe_pre_handler(syscall entry kprobe)   ~30 ns
            → sys_getpid()
                each memory access wrapped by KASAN
                  __asan_load4(&current->pid)            ~10 ns
                return current->pid                      ~5 ns
            → kprobe_post_handler                        ~30 ns
    um_on_syscall_exit(regs)
        ├─ static_branch_unlikely(&um_trace_syscalls)   → JMP
        │   __um_trace_syscall_exit(regs)                ~50 ns
        └─ ~50 ns total
        ↓ futex_wake stub
        ↓ ~100 ns
UML stub
    write regs back to shared memory
    return from SIGSYS handler                            ~100 ns
        ↓
Guest userspace

Total: ~200 + 100 + 80 + 10 + 5 + 60 + 10 + 50 + 100 + 100
     = ~715 ns
~10× slower than prod-fast. Full visibility: every syscall in
the trace buffer, every memory access KASAN-checked, kprobes
fired at chosen points.
```

## Profile: fuzz (seccomp backend, KCOV on, snapshot hooks compiled)

```
Guest userspace process
    syscall
        ↓ SECCOMP_RET_TRAP → SIGSYS                     ~200 ns
UML stub
    futex_wake UML kernel thread                         ~100 ns
UML kernel thread
    um_on_syscall_entry(regs)
        ├─ trace_syscalls          → NOP (fuzz: off)
        ├─ kcov_enabled            → JMP
        │   __um_kcov_record(regs->ip)                   ~10 ns
        ├─ record_replay           → NOP
        └─ ~10 ns total
    seccomp_run_userspace + do_syscall_64
        kcov_remote_start (per-handler instrumentation)
        sys_getpid + KASAN wraps                         ~20 ns
        kcov_remote_stop
    um_on_syscall_exit                                   ~10 ns
        ↓ wake stub                                      ~100 ns
UML stub
    return                                                ~100 ns
        ↓
Guest userspace

Total: ~550 ns
Coverage recorded for the syzkaller fuzzer. KASAN catches memory
bugs. Snapshot hooks compiled in but key off (snapshot is taken
at iteration boundaries, not per-syscall).
```

## Profile: sandbox (seccomp-only, no hooks compiled, no sanitizers)

```
Guest userspace process
    syscall
        ↓ SECCOMP_RET_TRAP → SIGSYS                     ~200 ns
UML stub
    futex_wake UML kernel thread                         ~100 ns
UML kernel thread
    um_on_syscall_entry(regs)
        (in this build, the gates are not even compiled in;
         the inline expands to nothing)
                                                         ~0 ns
    seccomp_run_userspace (single-backend, inlined)   ~0 ns
    do_syscall_64
        sys_getpid                                       ~5 ns
    um_on_syscall_exit                                   ~0 ns
        ↓ wake stub                                      ~100 ns
UML stub
    return                                                ~100 ns
        ↓
Guest userspace

Total: ~505 ns
Minimum TCB, no observability. Suitable for running untrusted
workloads where you want UML's user/kernel split for isolation
but don't care about debug surfaces.
```

## Profile: library (no backend, direct call)

```
Host application (any process)
    lkl_sys_getpid()
        → enters liblinux.a directly
        → do_sys_getpid()
            → sys_getpid()
                → return current->pid                    ~5 ns
        → returns to caller                              ~1 ns

Total: ~6 ns
No trap. No backend. No layers. The caller is responsible for
treating the kernel as a library — synchronization, no
reentrant kernel state changes, etc. This is LKL's model.

Bugs that depend on user/kernel boundary code (copy_from_user
TOCTOU, signal delivery races, unaligned-access faults) are
unreachable in this mode. That's the price of the speed.
```

## Profile: time-travel (seccomp backend, time-travel hook on)

```
Guest userspace process
    syscall (clock_gettime)
        ↓ SECCOMP_RET_TRAP                              ~200 ns
UML stub                                                 ~100 ns
UML kernel thread
    um_on_syscall_entry                                  ~50 ns (trace on)
    seccomp_run_userspace
        sys_clock_gettime
            ktime_get
                um_backend->read_clock_ns()
                    static_branch_unlikely(&um_time_travel_active) → JMP
                    → time_travel_get_time()
                        coordinator query (Unix socket)  ~500 ns
                        return deterministic_time
            ~600 ns total
    um_on_syscall_exit                                   ~50 ns
        ↓ stub wake + return                             ~200 ns

Total: ~1200 ns for clock_gettime (slowest syscall in this profile)
Most syscalls don't read the clock; their overhead is ~715 ns
like research mode.
```

## Comparison

| Profile | Backend | Hooks | Wraps | getpid (ns) | Slowdown vs native |
|---|---|---|---|---|---|
| prod-fast | KVM | none | none | ~80 | 1.6× |
| prod-with-hooks (off) | KVM | compiled, off | none | ~85 | 1.7× |
| research | seccomp | trace+kprobes | KASAN | ~715 | 14× |
| fuzz | seccomp | KCOV | KASAN | ~550 | 11× |
| fuzz-deep | seccomp | KCOV+replay | KASAN+KCSAN | ~1200 | 24× |
| sandbox | seccomp-only | none compiled | none | ~505 | 10× |
| library | direct call | none | none | ~6 | 0.12× (faster) |
| embedded | ptrace | none | none | ~2000 | 40× |
| time-travel | seccomp | trace+tt | KASAN | ~715-1200 | 14-24× |

(Native getpid: ~50 ns)

The architecture's value is visible in the table: one source tree
spanning 50 ns (library) to 2000 ns (embedded), with the production
target near native and the fuzzing target competitive with the
fastest existing fuzzing platforms.

## What this proves

1. The three layers compose without internal conflict.
2. The cost model is predictable per-profile.
3. Profile selection can be defconfig-driven.
4. The "have your cake and eat it" property holds: prod-with-hooks
   pays ~5 ns to retain the option of flipping tracing on at
   runtime.

What this doesn't prove (yet):

1. That the implementation actually achieves these numbers.
   Validation in `05-validation/benchmarks.md`.
2. That maintainers will accept the layering. Risk in
   `04-risks/political-lkml-acceptance.md`.
3. That the KVM backend is feasible to build. Workstream D's job.
