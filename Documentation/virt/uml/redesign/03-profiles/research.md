# Profile: research

**One-line:** Maximum visibility; debugger-friendly; sanitizers
on; slow but useful.

## Intended user

- Kernel developers debugging UML or testing changes.
- Security researchers reproducing CVEs (the project owner's
  use case).
- Anyone running LTP or kselftest under UML for development.
- Educators showing how the kernel works.

## Defining features

- Backend: seccomp (faster than ptrace, debugger-compatible).
- All Layer 2 hooks: compiled in; trace and kprobes default ON.
- Sanitizers: KASAN + UBSAN compiled in (KMSAN optional).
- Tracers: ftrace, kprobes, BPF JIT all on.
- Debug surfaces: mconsole and full debug info. KGDB is deferred:
  current UML does not select `HAVE_ARCH_KGDB` and this profile's
  real Kconfig fragment does not enable `CONFIG_KGDB`.
- Time-travel: optional, off by default; one Kconfig away.
- 64-bit only.

## Kconfig fragment

```
CONFIG_UM_BACKEND_SECCOMP=y
CONFIG_UM_BACKEND_PTRACE=y    # fallback for old hosts
CONFIG_UM_BACKEND_KVM=y       # if user wants to test under KVM
CONFIG_UM_BACKEND_DYNAMIC=y

CONFIG_UM_HOOKS=y
CONFIG_UM_HOOKS_DEBUGFS=y

CONFIG_KASAN=y
CONFIG_UBSAN=y
CONFIG_KFENCE=y
# CONFIG_KMSAN: opt-in (heavy)
# CONFIG_KCSAN: opt-in (use fuzz-deep if you want)

CONFIG_FUNCTION_TRACER=y
CONFIG_DYNAMIC_FTRACE=y
CONFIG_FUNCTION_GRAPH_TRACER=y
CONFIG_KPROBES=y
CONFIG_BPF_JIT=y
CONFIG_BPF_SYSCALL=y

CONFIG_MCONSOLE=y
CONFIG_DEBUG_INFO=y
CONFIG_DEBUG_INFO_DWARF5=y
CONFIG_FRAME_POINTER=y
CONFIG_GDB_SCRIPTS=y

# Default-on hooks
# (set via boot param or sysctl, not Kconfig — runtime)
```

## Cost

- Binary size: ~120 MB (sanitizers + debug info dominate)
- Boot: ~600 ms
- Syscall: ~715 ns
- RAM: ~80 MB

## Validation

- syzkaller crash reproducer for a known kernel CVE reproduces
  in <30 s
- KASAN positive: synthesize a known-bad allocation; KASAN
  reports
- ftrace function tracer captures a function call
- kprobes inserts at `do_sys_open` and fires
- mconsole responds to ping
- gdb attach to UML process and stop on `do_fork` breakpoint

## What this profile is NOT

- Not a fuzzing target — sanitizers slow the iteration loop.
  Use `fuzz` for syzkaller, `fuzz-deep` for KCSAN.
- Not for production — far too slow.
- Not for sandboxing — debug surfaces are escape primitives.

## Mode toggles supported within this profile

- Time-travel: `time-travel=1` boot param turns on; promotes
  to time-travel-research effectively
- KGDB: not currently available in UML; use host-side GDB attach
  plus mconsole/debugfs while KGDB remains deferred.
- mconsole: always on, socket at boot path
