# x86 UML Ptrace And TLS Regset Cleanup

Date: 2026-06-11

Branch: `next`

## Purpose

This slice removes a small set of old active-source cleanup markers while
turning the most important one into a correctness fix. The affected files are
normal x86 UML code, not redesign archive material:

- `arch/x86/um/syscalls_64.c`
- `arch/x86/um/ptrace_64.c`
- `arch/x86/um/ptrace.c`
- `arch/x86/um/asm/ptrace.h`
- `arch/x86/um/tls_32.c`

## Changes

`PTRACE_ARCH_PRCTL` now operates on the traced task. The helper already accepts
a `struct task_struct *`, but the implementation read and wrote `current` for
`ARCH_SET_FS`, `ARCH_SET_GS`, `ARCH_GET_FS`, and `ARCH_GET_GS`. The ptrace
caller passes the child task, so the helper now uses that task's register state.

The inherited `XXX` comment on `PTRACE_ARCH_PRCTL` was removed because the
current helper no longer calls host ptrace. The old inline `asm/prctl.h` include
comment was also removed.

The 32-bit TLS-regset TODO was replaced with an `NT_386_TLS` user regset. The
new regset uses UML's existing TLS entry cache and ptrace TLS helpers rather
than introducing a second representation. UML discovers the host TLS slot base
at runtime, so the regset callbacks write the correct `entry_number` values
directly instead of using x86's constant regset `.bias` initializer.

## Validation

Passed:

```text
git diff --check
git diff -- arch/x86/um/syscalls_64.c arch/x86/um/ptrace_64.c \
  arch/x86/um/ptrace.c arch/x86/um/asm/ptrace.h arch/x86/um/tls_32.c | \
  scripts/checkpatch.pl --strict --no-tree -
rg -n "\b(TODO|FIXME|HACK|XXX|temporary|workaround)\b" \
  arch/x86/um/syscalls_64.c arch/x86/um/ptrace_64.c \
  arch/x86/um/ptrace.c arch/x86/um/asm/ptrace.h arch/x86/um/tls_32.c
make ARCH=um -j$(nproc) arch/x86/um/syscalls_64.o \
  arch/x86/um/ptrace_64.o arch/x86/um/ptrace.o
```

The targeted scan returns no active TODO/XXX-style cleanup markers in the
touched files.

## i386 Build Limitation

The new TLS regset is under `CONFIG_X86_32`, so a detached clean i386 UML
worktree was created and the current patch was applied there. `defconfig`
completed, but the object build stopped during UML arch preparation:

```text
/usr/include/stdio.h:28:10: fatal error: bits/libc-header-start.h: No such file or directory
```

That failure occurs while compiling `arch/x86/um/user-offsets.c`, before Kbuild
reaches the touched TLS-regset objects. It indicates the host is missing the
32-bit libc development headers needed for `SUBARCH=i386` UML builds. The code
still needs an i386 compile on a host with multilib headers before this item can
be counted as fully validated.

## Remaining Follow-Up

Run the same i386 object build on a host with 32-bit libc headers:

```text
make ARCH=um SUBARCH=i386 O=<clean-output> defconfig
make ARCH=um SUBARCH=i386 O=<clean-output> -j$(nproc) \
  arch/x86/um/ptrace.o arch/x86/um/tls_32.o
```

If that passes, add this item to the final integration matrix as closed.
