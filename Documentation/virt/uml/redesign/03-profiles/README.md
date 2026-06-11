# Profiles

Each profile is a defconfig fragment + documentation. They are
the units we ship.

## The matrix

| Profile | Backend default | Hooks compiled | Hooks default-on | Sanitizers | Tracers | mconsole | KGDB | Snapshot | Time-travel |
|---|---|---|---|---|---|---|---|---|---|
| [prod-fast](prod-fast.md) | KVM > seccomp | none | n/a | none | none | no | no | no | no |
| [prod-with-hooks](prod-with-hooks.md) | KVM > seccomp | all | none | none | none | no | no | no | no |
| [research](research.md) | seccomp | all | trace, kprobes | KASAN, UBSAN | ftrace, kprobes, BPF | yes | no | no | yes (opt) |
| [fuzz](fuzz.md) | seccomp | KCOV, snapshot | KCOV | KASAN | none | no | no | yes | no |
| [fuzz-deep](fuzz-deep.md) | seccomp | KCOV, snapshot, replay | KCOV, replay | KASAN, KCSAN | none | no | no | yes | no |
| [sandbox](sandbox.md) | seccomp-only | none | n/a | none | none | no | no | no | no |
| [library](library.md) | none (direct call) | n/a | n/a | KASAN (opt) | none | n/a | n/a | n/a | n/a |
| [embedded](embedded.md) | ptrace | none | n/a | none | none | yes | no | no | no |
| [time-travel](time-travel.md) | seccomp | all | trace, time-travel | KASAN | ftrace | yes | no | yes | yes (forced) |

## How to read this

- **Backend default**: which backend `init_backend()` selects when
  `backend=auto` is chosen (default).
- **Hooks compiled**: which static-key gates are present in the
  binary at all. If "none compiled", the gate's slow-path code
  isn't linked in (sandbox profile pays no TCB cost for hooks).
- **Hooks default-on**: which gates start in the on state at boot.
  Independent of compiled.
- **Sanitizers**: which compile-time wraps are active. Bigger
  binary, slower runtime, more bugs caught.
- **Tracers / mconsole / KGDB / Snapshot / Time-travel**: feature
  presence, mostly Kconfig-gated.

KGDB is currently deferred for UML. The architecture does not select
`HAVE_ARCH_KGDB`, and the live profile fragments do not enable
`CONFIG_KGDB`; older profile notes that described KGDB as present were
historical intent, not current implementation.

## Per-profile cost summary

| Profile | Binary size | Boot time | getpid (ns) | Slowdown vs native |
|---|---|---|---|---|
| prod-fast | smallest | ~300 ms | ~80 | 1.6× |
| prod-with-hooks | small | ~300 ms | ~85 | 1.7× |
| research | large | ~600 ms | ~715 | 14× |
| fuzz | medium | ~500 ms (then snapshots) | ~550 | 11× |
| fuzz-deep | large | ~700 ms | ~1200 | 24× |
| sandbox | very small | ~250 ms | ~505 | 10× |
| library | n/a (lib) | n/a (linked into host) | ~6 | 0.12× |
| embedded | smallest | ~800 ms | ~2000 | 40× |
| time-travel | large | ~600 ms | ~715-1200 | 14-24× |

(Native getpid: ~50 ns. UML host syscall path adds the trap.)

## Profile-feature compatibility

Some features only make sense in some profiles. The matrix
captures must-have / must-not-have. Combinations marked
**"prefer"** are recommended defaults users can override.

A user who wants e.g. KASAN in the fuzz profile (the table says
yes already) can build it. A user who wants mconsole in the
sandbox profile can override the Kconfig but is doing something
the security model doesn't support.

## Adding a profile

1. Create `<profile>.md` here describing it.
2. Add to the matrix above.
3. Create `arch/um/configs/profiles/<profile>.config` fragment.
4. Add `make ARCH=um uml/<profile>` rule.
5. Add CI build for it.
6. Document its intended user.

## Removing a profile

1. Document why in `04-risks/decisions-log.md`.
2. Drop the `<profile>.md` and the matrix row.
3. Drop the Kconfig fragment and Make rule.
4. Drop CI build.
5. One-release transition warning before removal.

## What profiles are NOT

- **Not a security boundary**. A user can build `prod-fast` with
  added Kconfig that re-enables mconsole; they have what they
  built. Profiles ship defaults, not enforcement.
- **Not exhaustive**. Combinations not on the matrix may build
  but aren't validated in CI. Submitting a patch that breaks an
  unlisted combination is not a regression.
- **Not stable across major versions**. Profile defaults can
  shift between releases as the architecture evolves. Pin a
  profile version if you depend on its specifics.
