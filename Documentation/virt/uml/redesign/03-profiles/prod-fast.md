# Profile: prod-fast

**One-line:** Smallest, fastest UML; production target; no
observability.

## Intended user

- Production deployments where UML is a runtime, not a
  research tool.
- Container-isolation use cases where you want UML's
  user/kernel split for defense-in-depth but care about
  throughput.
- Comparison baseline ("is feature X actually slow?").

## Defining features

- Backend: KVM if available (bare metal); seccomp otherwise
  (nested virt, containers).
- All Layer 2 hooks: NOT compiled in.
- All Layer 3 wraps (KASAN/KMSAN/KCSAN/KFENCE): NOT compiled.
- mconsole, KGDB, KCOV, ftrace, kprobes, BPF JIT: NOT compiled.
- 32-bit support: removed (per stop-doing list).
- SMP: yes (kernel-side; per-process userspace).

## Kconfig fragment (sketch)

```
CONFIG_UM_BACKEND_KVM=y
CONFIG_UM_BACKEND_SECCOMP=y
CONFIG_UM_BACKEND_PTRACE=n
CONFIG_UM_BACKEND_DYNAMIC=y      # auto-select KVM > seccomp at boot

# Layer 2 — gates are compile-time absent (savings)
CONFIG_UM_HOOKS=n

# Layer 3
# CONFIG_KASAN is not set
# CONFIG_KMSAN is not set
# CONFIG_KCSAN is not set
# CONFIG_KFENCE is not set
# CONFIG_UBSAN is not set

# Tracers
# CONFIG_FUNCTION_TRACER is not set
# CONFIG_KPROBES is not set
# CONFIG_BPF_JIT is not set
# CONFIG_KCOV is not set

# Debug surfaces
# CONFIG_MCONSOLE is not set
# CONFIG_KGDB is not set
# CONFIG_DEBUG_INFO_DWARF5 is not set

# SMP
CONFIG_SMP=y
CONFIG_NR_CPUS=8
```

## Cost

- Binary size: <30 MB
- Boot to userspace: ~300 ms
- Syscall: ~80 ns (KVM) / ~300 ns (seccomp)
- RAM overhead per guest: ~30 MB

## Validation

- LTP runtest/syscalls passes
- A network throughput benchmark (vector_transport tap)
  reaches ~10 Gbit
- Boot time measured <500 ms
- syscall benchmark <100 ns on bare metal, <500 ns nested

## What this profile is NOT

- A debugging environment. Use `research`.
- A fuzzing environment. Use `fuzz`.
- A locked-down sandbox. Use `sandbox`.
