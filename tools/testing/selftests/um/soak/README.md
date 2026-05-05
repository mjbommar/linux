# UML Phase J pilot soak

Multi-workload realistic stress for the UML kvm-v2 backend, run side-by-side
against seccomp for parity. Built on top of `umlctl gate-loop`.

Companion of:
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-pilot-2026-05-05.md` — design + first-pilot results
- `Documentation/virt/uml/redesign/06-sequencing/milestones.md` — M11 (kvm-v2 100ns) is closed; this rig works toward M12 (full Phase J)

## What's in here

| File | Role |
|------|------|
| `memcheck.c` | anon-mmap pattern verifier (memtester replacement). Walking-bit + 7 constant-fill patterns, miscompare counter, exit code = total bad qwords. |
| `iocheck.c` | write/fsync/read/verify loop on tmpfs (fio replacement). Per-block deterministic key; catches pagecache / writeback / fsync ordering bugs. |
| `*.toml.template` | one Umlfile per workload; `{{KERNEL}}` and `{{BACKEND}}` substituted by `run-pilot.sh`. |
| `run-pilot.sh` | driver: parallel `umlctl gate loop`, per-backend sweep, k10temp throttle, cooldown between workloads. |

The two C tools are intentionally tiny (~50 / ~80 LoC) so they build static
without external deps when an offline / restrictive host can't `apt install
memtester fio`. They cover the same correctness signals (pattern verify +
write/read coherence) that those upstream tools cover — minus the configurable
load knobs.

## Workloads

| Workload | Tests | Per-iter wall | Notes |
|----------|-------|---------------|-------|
| `memcheck` | anon-mmap CoW + addressing wiring | ~3 s | 256 MB region × 2 iters × 8 patterns |
| `iocheck` | pagecache write/read coherence + fsync ordering | ~3 s | 64 MB × 4 iters × 4 K blocks on tmpfs |
| `stress-ng` | futex/pipe/switch IPC + `--verify` | ~10 s | `--vm` disabled — see SMP-T57 |
| `cpython-soak` | Python regrtest curated subset (signal/io/mmap/fork/threadsignals/etc.) | ~60-120 s | Tight set picked to fit ~60 s under UML overhead |
| `kbuild-tiny` | tinyconfig UML kernel build (fork-storm / pipe / file I/O) | ~3-5 min | `KBUILD_OUTPUT=/tmp/build` to avoid hostfs write fan-out |

## Running

```sh
# All five workloads, sweep both backends, 4 workers × 10 iters, default
# 90 s timeout (overridden per-workload):
./tools/testing/selftests/um/soak/run-pilot.sh all 4 10

# Just the three fast workloads (memcheck/iocheck/stress-ng), W=2:
./tools/testing/selftests/um/soak/run-pilot.sh short 2 20

# Single workload at custom W/M/timeout:
./tools/testing/selftests/um/soak/run-pilot.sh stress-ng 4 50 90

# Override kernel + thermal thresholds:
UML_KERNEL=/path/to/uml/linux \
THERMAL_PAUSE_C=85 THERMAL_RESUME_C=70 COOLDOWN=60 \
  ./tools/testing/selftests/um/soak/run-pilot.sh all 2 25
```

Output goes to `/tmp/soak-pilot-<unix-ts>/<workload>-<backend>/p0_default/wN/`
plus a CSV summary at `/tmp/soak-pilot-<unix-ts>/_summary.csv`.

## Thermal protection

The driver reads `k10temp` / `coretemp` / `zenpower` / `cpu_thermal` hwmon
sensors (the `acpitz` thermal_zone is unreliable on AMD). Before each workload:

1. If max CPU temp ≥ `THERMAL_PAUSE_C` (default 88 °C), sleep until it drops to
   ≤ `THERMAL_RESUME_C` (default 75 °C).
2. After the workload, sleep `COOLDOWN` seconds.

W=2 is the default to keep half the host CPUs free; under W=4 on a 16-core Zen
4 the package can hit 92 °C inside a minute on heavy workloads. Crank W up
when the host has more cooling headroom.

## Pass/fail discipline

The pilot's hardest lesson: don't let bash hide subprocess exit codes. Two
patterns to **never** use in a phase command:

```bash
# WRONG — pipefail-loss: $? captures `tail`'s exit (always 0), masking a
# failed cmd. Produces false PASS verdicts.
cmd 2>&1 | tail; echo REPRO_DONE rc=$?

# WRONG — the trailing semicolon means the `echo` is the last command,
# so the phase wrapper sees rc=0 even when cmd failed.
cmd; echo TOOL_RC=$?
```

The right pattern (used in every template here):

```bash
cmd && echo TOOL_OK || (tail -10 /tmp/cmd.log; echo TOOL_FAIL; exit 1)
```

This propagates non-zero up to the phase wrapper, which aborts init.sh and
makes the gate-loop see a missing `REPRO_DONE rc=0` marker → counted as FAIL.

The same trap bit the gate-loop's halt path on a separate axis — see
`tools/uml/uml-launcher/src/bin/umlctl/deploy.rs`'s sysrq-`b` halt and the
SMP-T54 commit message for why `/sbin/halt` (= systemctl) couldn't be relied
on.

## First-pilot results (2026-05-05, commit 95c95267202e)

```
workload   backend  total  pass  nonpass  elapsed_sec
memcheck   kvm-v2   40     40    0        86
memcheck   seccomp  40     40    0        58
iocheck    kvm-v2   40     40    0        93
iocheck    seccomp  40     40    0        58
stress-ng  kvm-v2   40     40    0        215
stress-ng  seccomp  40     40    0        216
```

240 / 240 = 100.0 % across both backends. Wilson 95 % CI [98.5 %, 100 %].
12 min wall-clock at W=2; max package temp 86 °C.

One follow-up: SMP-T57 — `stress-ng --vm --verify` trips on kvm-v2 only
(seccomp passes). Disabled in the IPC-only stress-ng template; the failure
mode (`stress-ng: info: 0x... not readable` + exit=2) survives a vm-method
bisect that hasn't been driven yet.
