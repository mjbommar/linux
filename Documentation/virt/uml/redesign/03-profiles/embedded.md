# Profile: embedded

**One-line:** ptrace-only; no host KVM or seccomp dependency;
runs anywhere Linux runs.

## Intended user

- Old kernel hosts (pre-3.5, before SECCOMP_FILTER).
- Restricted environments where seccomp isn't available.
- BSD jails / Solaris containers (some have ptrace but not
  Linux seccomp).
- Anyone who wants UML on a host kernel where the
  modern backends don't work.

## Defining features

- Backend: ptrace ONLY (single-backend inlined for size).
- All Layer 2 hooks: NOT COMPILED.
- All Layer 3 wraps: NOT COMPILED.
- Most debug surfaces: off.
- mconsole: yes (it's the only management interface here).
- 64-bit only.

## Kconfig fragment

```
CONFIG_UM_BACKEND_PTRACE=y
CONFIG_UM_BACKEND_PTRACE_ONLY=y    # only backend, inlined

CONFIG_UM_HOOKS=n
# CONFIG_KASAN is not set
# Debug surfaces minimal; mconsole is the management path
CONFIG_MCONSOLE=y
# CONFIG_KGDB is not set
# CONFIG_KCOV is not set
```

## Cost

- Binary size: ~20 MB (smaller than research, larger than
  sandbox — mconsole adds some)
- Boot: ~800 ms (ptrace is the slow boot path)
- Syscall: ~2000 ns (40× native)
- RAM: ~30 MB

## Why this profile exists

It is the answer to "I have an old/weird host where
SECCOMP_FILTER isn't available". Without this profile, those
users have no UML. The cost is keeping the ptrace backend
maintained in the source tree, which we do anyway as a fallback.

The backend abstraction makes this profile cheap to ship: it's
just `prod-fast` with a different backend selected.

## Validation

- Boots on Linux 3.0 host (pre-SECCOMP_FILTER)
- Boots on a host without `/dev/kvm`
- LTP runtest/syscalls passes (slowly)

## What this profile is NOT

- Not fast. The ptrace path is what we're moving away from in
  every other profile; this one keeps it as a compatibility
  fallback.
- Not the default. `prod-fast` and `research` should be users'
  first choice.
