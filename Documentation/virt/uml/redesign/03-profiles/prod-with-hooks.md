# Profile: prod-with-hooks

**One-line:** prod-fast plus the option to flip on tracing at
runtime.

## Intended user

- Production where you might need to debug an incident without
  rebuilding.
- Research deployments where you want fast steady state but
  reserve the right to introspect.
- The "killer combo" — what makes the static-key architecture
  pay off.

## Defining features

Same as prod-fast except:

- All Layer 2 hooks: COMPILED IN, default OFF.
- Slow-path handlers for each hook: linked in.
- debugfs interface: enabled.
- Bookkeeping for runtime gate stats.

Not added: sanitizers (still off — adding KASAN here would force
every prod user to pay the wrap cost). Tracers (kprobes/ftrace
JIT-patches text — adds attack surface for prod use).

## Kconfig fragment

```
# Inherits from prod-fast.config

CONFIG_UM_HOOKS=y
CONFIG_UM_HOOKS_DEBUGFS=y
CONFIG_KCOV=y                # gate exists; default off
# kprobes/ftrace stay off — too invasive for prod
```

## Cost

- Binary size: ~32 MB (slow paths added; ~2 MB)
- Boot: ~300 ms (same as prod-fast)
- Syscall: ~85 ns (~5 ns per gate with all NOPs)
- RAM: ~30 MB

## The killer use case

```
$ make ARCH=um uml/prod-with-hooks
$ ./linux ubd0=rootfs.img
... boot, run workload ...
[user observes problem]
$ echo 1 > /sys/kernel/debug/um/hooks/trace_syscalls
$ echo "openat,read" > /sys/kernel/debug/um/trace_syscall_filter
... next 1000 syscalls of openat/read are in the ring buffer ...
[user diagnoses]
$ echo 0 > /sys/kernel/debug/um/hooks/trace_syscalls
... back to ~85 ns per syscall ...
```

No rebuild. No reboot. Investigate live and revert.

## Validation

- All prod-fast benchmarks
- Plus: KCOV flippable in <1 ms; coverage flowing
- Trace gate flippable; trace events appear
- After flip-on-flip-off cycle, syscall cost returns to baseline
- Stress test: flip gates 1000×/sec for 10 minutes, no kernel
  oops

## What this profile is NOT

- Not for fuzzing — `fuzz` profile has snapshot/forkserver
  optimization.
- Not for research — `research` profile defaults to all hooks
  on, sanitizers compiled, debug surfaces available.
- Not for sandboxing — debugfs is a host-introspection
  primitive.
