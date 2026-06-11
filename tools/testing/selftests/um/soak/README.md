# UML soak tests

This directory contains longer-running UML workloads for comparing the
seccomp and kvm-v2 backends under realistic process, I/O, Python, and
network pressure. The scripts are intended for local validation, not for
the default kselftest fast path.

## Drivers

| File | Purpose |
|------|---------|
| `run-pilot.sh` | One-shot sweep over selected workloads and backends. |
| `run-soak-daemon.sh` | Continuous rotation with scoreboard output and threshold-based stop. |
| `run-ltp-smoke.sh` | Single-suite LTP probe for validating kirk/LTP setup. |

Both main drivers substitute `{{KERNEL}}`, `{{BACKEND}}`, and
`{{SOAK_DIR}}` into the `*.toml.template` workload files before running
`umlctl gate loop`.

## Workloads

| Workload | Focus |
|----------|-------|
| `memcheck` | deterministic memory-pattern verification |
| `iocheck` | tmpfs write, fsync, readback, and checksum verification |
| `stress-ng` | futex, pipe, context-switch, and scheduler pressure |
| `cpython-soak` | curated CPython stdlib regression subset |
| `tier1-pylibs` | host-installed Python C-extension smoke tests |
| `tier2-uv-pylibs` | Python packages from a prebuilt uv virtualenv |
| `ltp-runner` | selected LTP syscall suites through kirk |
| `tier3-django` | Django HTTP loopback workload |
| `tier3-fastapi` | FastAPI/Uvicorn HTTP loopback workload |
| `django-loopback-none` | loopback control with no UML network device |

## Quick Runs

```
# All default workloads, both backends.
KERNEL=/path/to/linux \
  tools/testing/selftests/um/soak/run-pilot.sh all 4 10 600

# Fast local sweep.
KERNEL=/path/to/linux \
  tools/testing/selftests/um/soak/run-pilot.sh fast 2 5 300

# One workload.
KERNEL=/path/to/linux \
  tools/testing/selftests/um/soak/run-pilot.sh cpython-soak 1 3 240
```

`run-pilot.sh` writes a CSV summary under
`/tmp/soak-pilot-<unix-ts>/`.

## Continuous Soak

```
KERNEL=/path/to/linux \
  tools/testing/selftests/um/soak/run-soak-daemon.sh \
    --budget-sec 86400 \
    --workers 2 \
    --iters-per-rotation 5 \
    --backends seccomp,kvm-v2
```

By default, the daemon writes `uml-soak-<ISO>/` under the current
directory. Important outputs:

| File | Contents |
|------|----------|
| `config.json` | resolved command-line configuration |
| `scoreboard.jsonl` | one row per workload iteration |
| `summary.md` | rolling pass/fail table with Wilson intervals |
| `logs/` | per-run init logs and copied panic logs |

Send `SIGTERM` or `SIGINT` to stop after the current workload finishes.

## Python Package Setup

Tier 1 uses host-installed packages visible through hostfs:

```
sudo apt install python3-requests python3-cryptography python3-numpy
```

Tier 2 expects a prebuilt uv virtualenv:

```
uv venv ~/.cache/uml-soak-tier2-venv
uv pip install --python ~/.cache/uml-soak-tier2-venv/bin/python \
  httpx pyyaml pendulum numpy
```

Set `TIER2_UV_PYTHON=/path/to/venv/bin/python` when the venv lives somewhere
else.

The templates run each library in a bounded smoke test and print
`REPRO_DONE rc=0` on success.

## LTP Setup

Install `kirk` and provide an LTP tree through `LTP_ROOT`:

```
KERNEL=/path/to/linux LTP_ROOT=/opt/ltp \
  tools/testing/selftests/um/soak/run-ltp-smoke.sh syscalls
```

`ltp-suite.txt` is the default keep-list and `ltp-skip.txt` is the
deny-list seed. `ltp-triage.txt` lists suites that need one-shot local
verification before they should join the default rotation.

## Host Controls

Workload templates use conservative host-resource defaults:

| Setting | Default | Reason |
|---------|---------|--------|
| THP | off | reduce cross-run memory noise |
| `oom_score_adj` | 500 | make soak guests expendable under pressure |
| hugepages | opt-in | requires host reservation |
| cgroups | opt-in | requires a writable hierarchy |
| CPU affinity | opt-in | host-specific |

Thermal throttling is handled by the drivers before each workload. The
daemon can pause before starting a new iteration if the package
temperature is above the configured threshold.
