# Profile: fuzz

**One-line:** Coverage-guided fuzzer-optimized; <50 ms restart;
KCOV always on.

## Intended user

- syzkaller / kAFL / similar fuzzers.
- syz-bisect users.
- Fuzzing CI pipelines.

## Defining features

- Backend: seccomp (consistent perf in CI environments).
- Layer 2: KCOV gate compiled in, default ON. Snapshot/forkserver
  hooks compiled in.
- Sanitizers: KASAN compiled in (catch the bugs we're fuzzing
  for).
- Tracers: none (overhead).
- Debug surfaces: minimal (no mconsole, no KGDB).
- Snapshot/forkserver: yes; this is the speed lever.
- Time-travel: no.

## Kconfig fragment

```
CONFIG_UM_BACKEND_SECCOMP=y
CONFIG_UM_BACKEND_PTRACE=n      # not used in fuzz
CONFIG_UM_BACKEND_KVM=n         # nested virt in CI typically; seccomp wins
CONFIG_UM_BACKEND_SECCOMP_ONLY=y  # inline; smallest binary

CONFIG_UM_HOOKS=y
CONFIG_UM_HOOKS_DEBUGFS=y       # for fuzzer tooling
CONFIG_UM_FUZZ_HOOKS=y          # snapshot/forkserver hooks

CONFIG_KCOV=y
CONFIG_KCOV_ENABLE_COMPARISONS=y
CONFIG_KASAN=y
# CONFIG_KCSAN — see fuzz-deep
# CONFIG_KMSAN — heavy; opt-in
# CONFIG_KFENCE — yes
CONFIG_KFENCE=y

# CONFIG_FUNCTION_TRACER is not set
# CONFIG_KPROBES is not set
# CONFIG_MCONSOLE is not set
# CONFIG_KGDB is not set

# Debug info: minimal (just enough to symbolize crashes)
CONFIG_DEBUG_INFO=y
# CONFIG_DEBUG_INFO_DWARF5 is not set
```

## Cost

- Binary size: ~80 MB (KASAN + KCOV)
- Cold boot: ~500 ms (only matters once per session)
- Snapshot restore: <50 ms (the steady-state cost)
- Syscall: ~550 ns

## The fuzz loop

```
$ uml-forkserver ./linux-fuzz --ready-point=postinit > parent.sock
$ syz-manager -config uml-fuzz.cfg --vm.uml.parent=parent.sock
... syzkaller forks N children per iteration; KCOV recorded ...
... crash → KASAN report captured → syzkaller stores corpus ...
```

## Validation

- syzkaller using `vm/uml` backend (C-08) achieves >1000 iter/s
- KCOV records edges per syscall; coverage growing
- KASAN-positive testcase produces a usable report
- Forkserver restart <50 ms (C-09 deliverable)

## What this profile is NOT

- Not a debug environment — no ftrace, no kprobes, no mconsole.
- Not for race detection — KCSAN is in fuzz-deep.
- Not for production — KASAN cost matters.
