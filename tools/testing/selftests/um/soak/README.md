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
| `django-loopback-none.toml.template` | Django-shaped stdlib HTTP loopback control with `network.mode = "none"`; isolates KVM-v2 process/socket/server flakes from vector2 TAP/fd setup. |
| `tier3-django.toml.template` | Tier 3 Django-shaped loopback HTTP server. Requires `CONFIG_UML_NET_VECTOR=y` kernel rebuild (see "Tier 3 bootstrap" below). |
| `tier3-fastapi.toml.template` | Tier 3 FastAPI-shaped loopback. Same pre-flight as `tier3-django`. |
| `ltp-runner.toml.template` | LTP test runner driven by `kirk` (LTP's new Python runner). Requires LTP + kirk installed under `/opt/` (see "LTP bootstrap" below). |
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
| `django-loopback-none` | stdlib HTTP server + loopback readiness/curl with no UML network device | ~5-180 s | KVM-v2/vector2 isolation control; failures here are backend workload bugs, not vector2 TAP/fd setup |
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

# Backend-isolated vector2 seccomp soak.  This is useful when KVM-v2
# is being debugged separately and the goal is replacement-gate evidence
# for the vector2 network path:
UML_KERNEL=... UMLCTL=... ./run-soak-daemon.sh \
       --budget-sec 7200 --backends seccomp \
       --workloads tier3-django-v2,tier3-fastapi-v2 \
       --workers 1 --iters-per-rotation 10 \
       --out /var/tmp/vector2-seccomp-tier3-2h

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

By default daemon mode sweeps both `kvm-v2` and `seccomp`.  Use
`--backends seccomp`, `--backends kvm-v2`, or
`--backends kvm-v2,seccomp` to narrow or restore the backend matrix.

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

## Tier 3 bootstrap

Tier 3 (`tier3-django.toml.template`, `tier3-fastapi.toml.template`)
exercises a loopback HTTP server inside the guest with the
host-side daemon hitting it via a TAP interface. Pre-flight:

```sh
# 1. Rebuild the kernel with legacy vector support:
cd ~/src/uml-builds/uml-smp-t41fix
./scripts/config --enable CONFIG_UML_NET_VECTOR
make ARCH=um O=$(pwd) olddefconfig -j
make ARCH=um O=$(pwd) -j
# Verify: grep CONFIG_UML_NET_VECTOR=y .config

# For the experimental vector v2 Tier 3 aliases, also enable:
./scripts/config --enable CONFIG_UML_NET_VECTOR_V2
./scripts/config --enable CONFIG_UML_NET_VECTOR_V2_INPROC
make ARCH=um O=$(pwd) olddefconfig -j
make ARCH=um O=$(pwd) -j
# Verify: grep -E 'CONFIG_UML_NET_VECTOR_V2(=|_INPROC=)y' .config

# 2. (Optional) Install real Django/FastAPI for richer testing.
#    The templates ship with a Python stdlib `http.server` shim that
#    exercises the same UML virtio-net path; swap to Django/uvicorn
#    once these are installed:
apt-get install python3-django python3-fastapi python3-uvicorn
# Then edit the relevant template's phase 2 cmd to invoke
# `python3 -m django runserver 0.0.0.0:8080` or `python3 -m uvicorn
# app:app --host 0.0.0.0 --port 8080` instead of the stdlib shim.
```

IP allocation: the daemon assigns per-worker /30s from
192.168.42.0/24 (worker N → host `.4N+1`, guest `.4N+2`, tap
`soak-tap<N>`). Tier 3 templates use `{{HOST_IP}}` / `{{GUEST_IP}}`
/ `{{TAP_NAME}}` / `{{NETWORK_DRIVER}}` placeholders the daemon
substitutes per worker.

By default, `tier3-django` and `tier3-fastapi` use the legacy vector
driver (`network.driver = "vector"`, guest device `vec0`, kernel arg
`vec0:transport=tap,...`). The experimental v2 workload aliases
`tier3-django-v2` and `tier3-fastapi-v2` reuse the same templates but
substitute `network.driver = "vector2"`, guest device `vec2.0`, and
kernel arg `vec2.0:transport=tap,mode=inproc,...`. The v2 aliases are
for replacement-gate evidence only; the production path remains
legacy `vec0`.

For direct `umlctl` comparisons without editing TOML, use:

```sh
umlctl up -f tier3-django.toml --network-driver vector2 --dry-run
umlctl gate loop -f tier3-django.toml --sweep network.driver=vector,vector2 -W 1 -M 30
umlctl gate loop -f tier3-django.toml --network-driver vector2 --sweep network.queues=1,2 -W 1 -M 10
```

`umlctl up --dry-run` prints a network plan with the selected guest
device and exact kernel command-line argument, so operator logs show
whether a run used legacy `vec0` or experimental `vec2.0`.

Generated guest init scripts also export `UMLCTL_NETWORK_DRIVER` and
`UMLCTL_NETDEV`, so workload phases can inspect the selected link
without hard-coding legacy or v2 device names.
For vector2 multiqueue experiments, `[network] queues = N` or
`--network-queues N` adds `queues=N` to the v2 kernel argument and uses
matching host `multi_queue` TAP setup/teardown.

Design: `phase-J-tier3-design-2026-05-14.md`. The per-worker IP
allocation carve-out in `run-soak-daemon.sh` landed in commit
`ba63d93515a5` (task #25, bash-side scope closed): the daemon
branches on workload name in `run_one_phase`, spawning
`$WORKERS` parallel `umlctl gate loop --workers 1` invocations
for tier3 workloads with per-worker `{{HOST_IP}}` /
`{{GUEST_IP}}` / `{{TAP_NAME}}` substitution. Verified under
dry-run smoke (no stray `{{...}}` placeholders in worker
TOMLs); live smoke under a `CONFIG_UML_NET_VECTOR=y` kernel
is operator pre-flight.

## LTP bootstrap

The `ltp-runner.toml.template` invokes `kirk` (LTP's upstream
Python runner; `runltp` was deprecated and removed in 2026).
LTP is not apt-installable on Ubuntu Resolute; clone from
upstream:

```sh
git clone --depth 1 https://github.com/linux-test-project/ltp /opt/ltp-src
cd /opt/ltp-src
make autotools
./configure
make -j$(nproc)
sudo make install        # → /opt/ltp (default prefix)

git clone --depth 1 https://github.com/linux-test-project/kirk /opt/kirk
# kirk is Python — runs with system python3, no compile.

# Skip-list seed (112 entries from the curation memo §3):
# operator decides whether it lives in-tree or at
# /opt/uml-soak/ltp-skip.txt. The template references
# {{SOAK_DIR}}/ltp-skip.txt by default.
```

Curation: `phase-J-ltp-curation-2026-05-14.md`. Per-cycle wall:
45 min target, 60 min hard kill. Scheduled every 8 daemon
rotations (4-6 LTP cycles per backend per 24 h soak).

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
