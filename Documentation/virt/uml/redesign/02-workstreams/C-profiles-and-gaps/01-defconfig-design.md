# C-01: Defconfig design + skeleton

**Status:** planned
**Effort:** 3 weeks
**Dependencies:** A-04 (Kconfig backend selection)
**Blocks:** every other C task (they wire into defconfigs)

## Goal

Produce the seven+ defconfigs and a `make uml/<profile>` build
target. Each defconfig is a complete configuration; profiles are
expressible as defconfig fragments + a base config.

## Approach

1. **Base config**: `arch/um/configs/base_defconfig` containing
   what every UML build needs (architecture, init, basic
   filesystems, networking).
2. **Profile fragments**: `arch/um/configs/profiles/{research,
   fuzz, sandbox, ...}.config` containing only the diff from base.
3. **Build target**: `make ARCH=um uml/research` resolves to
   `make defconfig base + research fragment` then `make`.
4. **Document each profile** in `arch/um/Documentation/profiles/`.

## Profiles to ship

| Profile | Highlights |
|---|---|
| `prod-fast` | Best backend available, no hooks, no wraps, smallest binary |
| `prod-with-hooks` | KVM if available, hooks compiled in but off, runtime-flippable |
| `research` | All sanitizers, all tracers, mconsole, time-travel, KGDB |
| `fuzz` | KCOV, snapshot hooks, KASAN, syzkaller-friendly |
| `fuzz-deep` | fuzz + KCSAN + record-replay |
| `sandbox` | seccomp-only, no debug surfaces, minimum TCB |
| `library` | `liblinux.a` artifact, no backend |
| `embedded` | ptrace-only, no host KVM/seccomp dep |
| `time-travel` | Deterministic clock, UP, replay-friendly |

## Deliverable

- `arch/um/configs/base_defconfig`
- `arch/um/configs/profiles/*.config`
- `arch/um/Makefile` rules for `uml/<profile>` targets
- `arch/um/Documentation/profiles/*.rst` (one per profile)

## Validation

- `make ARCH=um uml/<profile>` succeeds for each profile
- Resulting kernel boots
- Each profile's defining feature works (KCOV in fuzz, mconsole
  in research, etc.)
- Profile binaries differ in expected ways (sandbox smallest,
  research largest)

## Open questions

- **Q1**: Should we use upstream's `defconfig +
  fragment` mechanism or invent our own? (Plan: use upstream's
  `KCONFIG_CONFIG=...` + merge_config.sh.)
- **Q2**: Where does the `library` profile live? It's not
  really a Kconfig — it's a different artifact. (Plan:
  separate `make ARCH=um liblinux` target; doesn't go through
  `uml/<profile>`. Documented separately.)
- **Q3**: Do we make profiles user-extensible? (Plan: yes;
  document how to write your own fragment.)

## Risk

Defconfigs drift over time. A change that adds a new Kconfig
symbol to one profile may not get the same treatment in the
others.

**Mitigation:**
- Discipline: every patch changing UM Kconfig must update all
  profile fragments
- CI: build all profiles; warn if any fails to build
