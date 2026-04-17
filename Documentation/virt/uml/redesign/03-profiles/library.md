# Profile: library

**One-line:** `liblinux.a` and `liblinux.so` — Linux kernel as a
library, callable from any host process.

## Intended user

- AFL++/libFuzzer harnesses against kernel subsystems.
- Userspace fuzzers (kBdysch, Janus successors).
- Embedded testing (run a kernel subsystem in a unit-test
  process).
- Anyone who wants the speed of LKL with the maintenance of
  upstream Linux.

## Defining features

- No backend. There is no trap. Caller invokes
  `lkl_sys_open(path, flags, mode)` directly; the function
  enters the kernel implementation as a C call.
- No user/kernel ring split. The library lives in the host
  process's address space.
- No syscall trap. ~5 ns per "syscall" — function call cost.
- KASAN: optional; useful for fuzzing.
- Other sanitizers: optional.
- Time-travel, mconsole, KGDB: irrelevant (no kernel-as-process).

## Build target

```
$ make ARCH=um uml/library
... builds liblinux.a, liblinux.so ...
$ ls liblinux*
liblinux.a   liblinux.so   include/lkl_*.h
```

Linked into a host program:

```c
#include <lkl.h>

int main(void) {
    lkl_init();
    int fd = lkl_sys_openat(LKL_AT_FDCWD, "/etc/passwd", O_RDONLY, 0);
    char buf[4096];
    long n = lkl_sys_read(fd, buf, sizeof(buf));
    /* ... */
}
```

## What's reachable; what's not

- **Reachable**: any kernel code that doesn't depend on user/kernel
  boundary. VFS, networking stack, allocators, schedulers (call
  them — they think they're scheduling).
- **Not reachable**: anything that depends on `copy_from_user`
  TOCTOU, signal delivery, page-fault-driven semantics, real
  syscall entry path.

## Kconfig fragment

```
# Library mode is its own build target, not a Kconfig per se.
# But the kernel needs to know it's being built as a library:
CONFIG_UM_LIBRARY_MODE=y
# CONFIG_UM_BACKEND_* — none, no backend
# CONFIG_UM_HOOKS=n
# Sanitizers per user choice
```

## Cost

- Binary size: ~5 MB for `liblinux.so`
- "Boot": <1 ms (init kernel state in-process)
- Per-call: ~5 ns (just a function call)
- RAM: minimal; lives in caller's address space

## Validation

- An LKL-style demo program reads `/etc/passwd` via library
- A fuzzer harness against `liblinux.a` runs at >100k iter/s
- KASAN-instrumented build catches a known bug in a kernel
  subsystem

## How this differs from LKL itself

- **Same source tree** as ring-split UML. `liblinux.a` shares
  ~95% of its code with the `linux` ELF binary.
- **Maintained alongside upstream** Linux instead of forked.
- **Profile-aware**: ships as a sibling of other UML profiles,
  not as a separate project.

This is what Tazaki's "Unify LKL into UML" RFC was trying to
land in 2019. With Layer 1 + Layer 2 + Layer 3 architecture
plus a v2 attempt, it should be tractable.

## What this profile is NOT

- Not a kernel that runs guest userspace processes (that's
  ring-split mode).
- Not a substitute for QEMU-KVM or KVM-backend UML for
  realistic syscall behavior.
- Not free of pitfalls — caller is responsible for not
  re-entering the kernel from a signal handler, locking
  shared state appropriately, etc.
