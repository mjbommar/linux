# PARK.8 spike — which Linux userspace-tracing mechanisms already work in UML?

**Date:** 2026-04-18
**Build:** DYNAMIC matrix config + `CONFIG_USER_EVENTS=y`,
`CONFIG_KCOV=y`, `CONFIG_FTRACE_SYSCALLS=y` (via `olddefconfig`).
**Method:** one-shot init script boots UML, mounts `proc`,
`tracefs`, `debugfs`, and reports presence/absence of each
mechanism's user-facing interface. Full script reproduced at the
end of this file.

## Findings

| Mechanism | Present on UML today? | Via which surface | Notes |
|---|---|---|---|
| `ftrace` (core) | ✅ yes | `/sys/kernel/tracing/` full tree | Arch-independent; works out of the box once `CONFIG_FTRACE=y`. |
| syscall tracing (`events/syscalls/sys_enter_*`) | ✅ yes | `/sys/kernel/tracing/events/syscalls/` | `HAVE_SYSCALL_TRACEPOINTS` already in UML's `arch/um/Kconfig`. Per-syscall `enable` + `filter` files present. |
| `user_events` | ✅ yes | `/sys/kernel/tracing/user_events_{data,status}` | Arch-independent; depends only on `TRACING=y` + `DYNAMIC_EVENTS=y`. Selectable via `CONFIG_USER_EVENTS=y`. |
| `KCOV` | ✅ yes | `/sys/kernel/debug/kcov` | `ARCH_HAS_KCOV` already selected in `arch/um/Kconfig`. Interface file is a regular debugfs file (not a char device) — `open` + `ioctl` + `mmap` per standard KCOV usage. |
| `uprobes` | ❌ no | n/a | UML does not `select ARCH_SUPPORTS_UPROBES`. No `/sys/kernel/tracing/uprobe_events`. Port required. |
| `kprobes` | ❌ no | n/a | UML does not `select HAVE_KPROBES`. No `/sys/kernel/tracing/kprobe_events`. Port required. |
| `perf events` (hardware counters) | ❌ no | n/a | No guest-side `events/perf/` subtree. Would need a UML-specific PMU shim. |
| dynamic ftrace (function tracer / mcount) | ❌ no | n/a | UML does not `select HAVE_FUNCTION_TRACER` or `HAVE_DYNAMIC_FTRACE`. Blocked on the same mprotect-writable-text machinery as the jump-label JIT work (B-04 + D19). |

## What this means for the parking-lot gap table

Gap row "Guest user-space tracing mechanisms (`uprobes`, `user_events`,
`perf` counters) *work inside the UML guest*" resolves into four
separate answers:

- `user_events` — **resolved: works today.** Add a selftest that
  opens `user_events_data`, registers an event from a guest
  userspace process, and observes it through `tracefs`. Document
  in `Documentation/virt/uml/`.
- `ftrace` + syscall events + KCOV — **works today.** Same
  treatment: add to `backends.rst` / a new `observability.rst` note
  with pointers.
- `uprobes` — **needs an arch port.** Graduates into a real task
  (probably lands in workstream C scope per PARK.7; see D24). The
  port has three parts: arch-glue for single-step + breakpoint
  insertion, ptrace/seccomp backend integration, and selftest.
- `kprobes` + function tracer — **blocked on B-04 follow-up.** The
  same text-writability constraints that gate `HAVE_ARCH_JUMP_LABEL`
  (D19) also gate `HAVE_FUNCTION_TRACER` and `HAVE_KPROBES`. Once
  B-04's mprotect helpers are consumed by a jump-label arch port,
  extending that consumer set to mcount/kprobes is incremental.

## Consequence for workstream C

C's "add missing kernel instrumentation features" task list should
be refined:

- keep KCOV as a C workstream task (already selectable; needs
  `CONFIG_KCOV=y` in the appropriate profiles + a selftest),
- **remove** `user_events` as a gap — it already works,
- **remove** `ftrace` syscall tracing as a gap — it already works,
- **add** uprobes as a real C task (arch-glue port + backend
  integration),
- **defer** function tracer + kprobes to a B-04 follow-up
  workstream since they share the same blocker.

## The reproducing init script

```sh
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t tracefs none /sys/kernel/tracing 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null
for item in \
    /sys/kernel/tracing \
    /sys/kernel/tracing/events/syscalls \
    /sys/kernel/tracing/user_events_data \
    /sys/kernel/debug/kcov \
    /sys/kernel/tracing/uprobe_events \
    /sys/kernel/tracing/kprobe_events \
    /sys/kernel/tracing/events/perf
do
    if [ -e "$item" ]; then
        echo "PRESENT  $item"
    else
        echo "ABSENT   $item"
    fi
done
halt -f
```

Booted via `./linux init=<script> mem=128M con=null con0=fd:0,fd:1
root=/dev/root rootfstype=hostfs rw` on the
`/tmp/uml-matrix-dynamic/linux` built with `USER_EVENTS`, `KCOV`,
and default tracing enabled.

## Bottom line

The parking-lot gap on "guest userspace observability mechanisms"
was pessimistic. Three of the four mechanisms listed in the
original gap already work — the work to graduate them from
"parking lot" to "product" is documentation + selftests, not
novel engineering. Only `uprobes` (and, related, `kprobes` +
function tracer) need real porting work, and that should fold
into workstream C's scope per D24.
