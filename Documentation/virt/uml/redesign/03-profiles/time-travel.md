# Profile: time-travel

**One-line:** Deterministic clock + record-replay + UP forced;
unique to UML.

## Intended user

- Network-stack researchers (Intel WiFi team uses this).
- Distributed-systems testing where timing reproducibility
  matters.
- Anyone reproducing a crash deterministically.
- LF Edge / OpenWifi / mac80211_hwsim ecosystem.

## Defining features

- Backend: seccomp.
- Layer 2: time-travel gate compiled in, ON. Trace gate ON.
  Record-replay gate compiled in.
- Sanitizers: KASAN on.
- Tracers: ftrace on (helps debugging deterministic replays).
- mconsole: yes.
- KGDB: deferred; current UML does not select `HAVE_ARCH_KGDB`.
- Snapshot: yes (works alongside time-travel).
- SMP: forced UP at boot (per conflict C2).
- External time coordinator: optional; `time-travel=ext` mode.

## Kconfig fragment

```
CONFIG_UM_BACKEND_SECCOMP=y

CONFIG_UM_HOOKS=y
CONFIG_UM_TIME_TRAVEL_SUPPORT=y   # was already a Kconfig
CONFIG_UM_RECORD_REPLAY=y

CONFIG_KASAN=y
CONFIG_FUNCTION_TRACER=y
CONFIG_KPROBES=y
CONFIG_BPF_JIT=y

CONFIG_MCONSOLE=y
CONFIG_DEBUG_INFO_DWARF5=y

CONFIG_SMP=n                       # invariant per conflict C2
```

## Boot params

- `time-travel=on` — internal coordinator
- `time-travel=ext:<socket>` — external coordinator (multi-UML
  experiments)
- `time-travel=inf-cpu` — no CPU advancement; only logical time

## Cost

- Binary size: ~120 MB (full research-like build)
- Boot: ~600 ms
- Syscall: ~715-1200 ns (depends on whether the syscall reads
  the clock)
- RAM: ~80 MB

## The unique value

QEMU's `icount` is not equivalent to time-travel mode. UML's
time-travel:

- Synchronously advances time (events block until time has
  caught up)
- Multi-guest synchronization via Unix socket (lockstep N
  UMLs)
- Combines with network simulation (deterministic packet
  delivery)

Nothing else gives this. It's the reason this profile exists
and the reason UML beats QEMU-KVM for network-stack research.

## Validation

- Two UMLs in `time-travel=ext` lockstep produce bit-identical
  output across runs
- A captured event log replays deterministically
- mac80211_hwsim test produces stable results across runs

## What this profile is NOT

- Not fast. SMP is off and tracing is on.
- Not for production.
- Not for fuzzing — use `fuzz-deep` if you want record-replay
  for fuzzing.
