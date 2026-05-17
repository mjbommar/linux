# UML Phase J pilot soak

Multi-workload realistic stress for the UML kvm-v2 backend, run side-by-side
against seccomp for parity. Built on top of `umlctl gate-loop`.

Two drivers ship here:

- **`run-pilot.sh`** — interactive one-shot smoke: pick a workload (or "all"),
  sweep both backends, dump a CSV summary. The "smoke a workload and report"
  audience.
- **`run-soak-daemon.sh`** — long-running daemon: signal-driven, append-only
  scoreboard.jsonl + rolling Wilson-CI summary.md, configurable wall-clock
  budget, restart-on-fail, threshold-trip stop. The "run for 24h continuous"
  audience. Spec at
  `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-design-2026-05-07.md` §2.

Companion docs:
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-pilot-2026-05-05.md` — pilot design + first-pilot results
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-design-2026-05-07.md` — daemon-mode + Tier 1/2/3 + LTP design
- `Documentation/virt/uml/redesign/06-sequencing/milestones.md` — M11 (kvm-v2 100ns) is closed; this rig works toward M12 (full Phase J)

## What's in here

| File | Role |
|------|------|
| `memcheck.c` | anon-mmap pattern verifier (memtester replacement). Walking-bit + 7 constant-fill patterns, miscompare counter, exit code = total bad qwords. |
| `iocheck.c` | write/fsync/read/verify loop on tmpfs (fio replacement). Per-block deterministic key; catches pagecache / writeback / fsync ordering bugs. |
| `tier1-smoketest.py` | Tier 1 host-installed Python C-extension smoke (requests + cryptography; numpy deferred — see "Known issues" below). 0.16 s on host, 2-5 s in UML. |
| `tier2-uv-smoketest.py` | Tier 2 deterministic smoke (httpx + pyyaml + pendulum + numpy). Run from a `uv`-built persistent venv (see "Tier 2 bootstrap" below). |
| `*.toml.template` | one Umlfile per workload; `{{KERNEL}}`, `{{BACKEND}}`, and `{{SOAK_DIR}}` substituted by both drivers. |
| `run-pilot.sh` | one-shot driver: parallel `umlctl gate loop`, per-backend sweep, k10temp throttle, cooldown between workloads. |
| `run-soak-daemon.sh` | daemon driver: same spawn pattern as run-pilot, plus wall-clock budget, scoreboard.jsonl, summary.md, restart-on-fail, signal handlers (SIGTERM/SIGINT clean stop, SIGUSR1 force summary refresh). |

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
| `tier1-pylibs` | host-installed Python C-extension exercise (requests URL+JSON+headers, cryptography AES-256-CBC roundtrip on 4 KiB block) | ~5-10 s | numpy deferred — see "Known issues" |
| `tier2-uv-pylibs` | pre-built-venv pip C-extension exercise (httpx URL+JSON, pyyaml round-trip, pendulum tz math, numpy linalg+FFT) | ~2-5 s | needs Tier 2 bootstrap below; uv-managed |

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

## Running — daemon mode (long soak)

```sh
# 24 h continuous against all default workloads:
UML_KERNEL=/path/to/uml/linux \
  ./tools/testing/selftests/um/soak/run-soak-daemon.sh \
       --budget-sec 86400 --workers 2 --iters-per-rotation 10

# 1 h focused soak on Tier 1 only, custom output dir:
UML_KERNEL=... ./run-soak-daemon.sh \
       --budget-sec 3600 --workloads tier1-pylibs \
       --workers 1 --iters-per-rotation 5 \
       --out /var/tmp/phase-J-tier1-1h

# Stop early: kill -TERM <pid>; the daemon finishes the in-flight workload
# phase, writes a final summary, exits cleanly.
# Force a summary refresh without stopping: kill -USR1 <pid>.
```

The daemon writes `phase-J-soak-<ISO>/{config.json,scoreboard.jsonl,
summary.md,logs/}`. `summary.md` carries a rolling Wilson-95 % CI table
per (workload, backend); `scoreboard.jsonl` is one JSON object per
iteration with the same shape as `umlctl gate run`'s rows + soak-specific
fields (`soak_run_id`, `rotation_idx`, `iter_idx_within_workload`,
`max_temp_c_pre/post`).

Threshold-trip stop: if a (workload, backend) tuple's rolling-window
failure rate exceeds `--fail-threshold-pct` over the last
`--fail-threshold-window` iters, the daemon writes a `THRESHOLD_TRIPPED`
sentinel into the output dir and exits. CI integrations can watch for
that file. Override with `--continue-on-fail-threshold` for
known-flaky-but-soak-anyway scenarios.

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

## Tier 1 results (2026-05-07, commit 86f6910dcb3f)

`tier1-pylibs.toml.template` exercises requests + cryptography in two
sequential Python processes (~5 s/iter on Zen 4, both backends).
Verified end-to-end via `run-soak-daemon.sh`:

```
60 s budget, W=1, M=2, both backends
-> 5 rotations × 2 backends × 2 iters = 20 PASS rows
-> Wilson 95 % CI [62.34 %, 100.00 %] at n=10 per (workload, backend)
```

## Tier 2 bootstrap

Tier 2 (`tier2-uv-pylibs.toml.template`) exercises pip-installed
Python C-extensions (httpx, pyyaml, pendulum, numpy) without
network access at run time. Bootstrap once on the host:

```sh
# Install uv (single statically-linked Rust binary):
curl -LsSf https://astral.sh/uv/install.sh | sh

# Build the persistent venv at the hostfs-shared path the
# template hardcodes (~/.cache/uml-soak-tier2-venv):
uv venv ~/.cache/uml-soak-tier2-venv
uv pip install --python ~/.cache/uml-soak-tier2-venv/bin/python \
    httpx pyyaml pendulum numpy
```

Total disk: ~70 MB. The venv is hostfs-readable from inside the
guest at the same path; no network is needed at soak time. Refresh
the venv (re-run `uv pip install`) when bumping package versions.

Why a venv instead of `uv run --with <pkg>`? `uv`'s offline
resolver was unreliable inside UML — the simple-index version
walk-back didn't always find cached wheels, even with
`UV_OFFLINE=1`. A pre-built venv skips the resolver at run time
entirely. See `phase-J-design-2026-05-07.md` §3.2 commentary.

## Known issues / deferred work

- **SMP-T57** — `stress-ng --vm --verify` SIGILL on kvm-v2 only. Five
  probe cycles narrowed the suspect: ruled out IST-write,
  SYSRETQ-RCX, GPR drift, and user-stack TDP coherence. Current
  primary hypothesis: FPU/XSAVE residue (P=0.55). New cheap candidate:
  user-text-page TDP mis-mapping (P=0.30). Workload disabled in the
  IPC-only `stress-ng.toml.template`. Track at
  `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/state-audit/24-smp-t57-vmmethod-bisect.md`.

- **`tier1-pylibs` numpy deferred** — `import cryptography` followed
  later by `import numpy` reproducibly fails inside UML on BOTH
  backends with a misleading "Error importing numpy: you should not
  try to import numpy from its source directory" (the actual cause is
  `from numpy.__config__ import show_config` raising ImportError on
  the second Python invocation). NOT a v2 regression — UML-environment
  / hostfs / Python C-extension chain interaction. Current Tier 1
  rotation runs requests + cryptography only. The `tier1-smoketest.py`
  ships the numpy test for ad-hoc debugging via
  `python3 tier1-smoketest.py numpy`.

- **Tier 2 / Tier 3 / LTP runner** — designed at
  `phase-J-design-2026-05-07.md` §3.2 / §3.3 / §4 but not yet
  implemented. Tier 2 needs a host-side `pip download` bootstrap step;
  Tier 3 needs a `CONFIG_UML_NET_VECTOR=y` rebuild + tap setup; LTP
  needs the curated KEEP/SKIP list.
