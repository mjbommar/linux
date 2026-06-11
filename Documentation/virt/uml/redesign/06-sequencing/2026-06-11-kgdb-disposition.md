# KGDB Disposition

Date: 2026-06-11

Branch: `next`

## Purpose

The original UML v2 vision named KGDB as part of the research/debug surface,
and older redesign profile notes still described KGDB as present in the
`research` and `time-travel` profiles. The current source tree does not support
that claim.

This note records the current disposition so the active docs do not promise an
unimplemented debugger surface.

## Current Source Evidence

Current UML does not select the architecture capability KGDB requires:

```text
arch/um/Kconfig*: no select HAVE_ARCH_KGDB
```

The live UML profile fragments also do not enable KGDB:

```text
arch/um/configs/profiles/: no CONFIG_KGDB=y
```

The backend contract already treats `read_guest_regs` and `write_guest_regs` as
cold backend ops, but they are currently reserved for future KGDB/debugger
integration rather than wired to a working KGDB frontend.

## Documentation Change

The redesign profile docs now match the implementation:

- `research` keeps mconsole, debugfs, debug info, tracing, kprobes, BPF JIT,
  and sanitizers as the active debug surface, but describes KGDB as deferred.
- `time-travel` keeps its deterministic clock/debug surface, but describes KGDB
  as deferred.
- the profile matrix marks KGDB as absent for those profiles.
- the live functionality inventory marks KGDB `Deferred-not-present`.

## Revive Criteria

KGDB can re-enter the completion claim only after a future implementation slice
adds and validates:

- UML `HAVE_ARCH_KGDB` support;
- backend `read_guest_regs` / `write_guest_regs` integration for the supported
  backend set;
- a KGDB transport decision that fits UML process and mconsole constraints;
- a smoke test that proves a debugger can connect, stop the guest, inspect
  registers, and resume.

Until then, host-side GDB attach, mconsole, debugfs, tracing, kprobes, and BPF
JIT are the supported UML research/debug surfaces.
