# Phase J validation — design memo (2026-05-07)

> Companion to `phase-J-pilot-2026-05-05.md` (the rig that exists today,
> 240/240 = 100% on three short workloads). This memo specifies what
> still needs building to flip STATUS row J from PENDING to DONE.
>
> Scope: design only. No code, no shell scripts, no kernel changes. A
> reviewer six months from now must be able to implement from this
> memo without re-reading the pilot's git history.

## 1. TL;DR

The J-pilot covers three things Phase J needs and leaves three open:

- **Covered:** parallel boot harness (`umlctl gate loop`), thermal
  protection, per-iteration PASS/FAIL classification, two-backend
  side-by-side sweep, scoreboard.jsonl convention.
- **Not covered:**
  1. **24-hour continuous operation.** `run-pilot.sh` is a one-shot
     driver — wall-clock budget, restart-on-fail, rolling Wilson CI,
     hourly summary, persistent artefact tree all absent.
  2. **Tier 1 / 2 / 3 workloads.** The five pilot templates exercise
     kernel surfaces (MM, IPC, fork, build); they do not yet exercise
     the third-party Python ecosystem (`requests`/`cryptography`/
     `numpy`), pip-driven installs, or HTTP loopback servers, all of
     which the redesign explicitly calls out as the merge gate's
     planned width expansion.
  3. **LTP curation.** ~1500 LTP tests; many UML-hostile (network
     namespaces, kernel modules, hardware probes). Need a curated
     KEEP/SKIP list and a runner template before LTP can be folded in.

Three concrete deliverables follow: **(a)** a daemon-mode wrapper
(`run-soak-daemon.sh`) extending the existing rig; **(b)** Tier 1/2/3
templates with explicit pre-conditions and acceptance criteria;
**(c)** an LTP curation framework + initial skip list. Sections 2-4
specify each. Section 5 names the bar for "Phase J DONE." Sections
6-7 cover risks and references.

## 2. Daemon-mode wrapper

### 2.1 Decision: new file, not a `--daemon` flag on `run-pilot.sh`

`run-pilot.sh` is a one-shot driver tuned for "smoke a workload and
report a CSV." Bolting daemon semantics onto it would conflate two
audiences — the pilot operator (interactive, short loop, single CSV)
and the long-soak operator (background, signal-driven, append-only
artefact tree). Conflation is what bug 1 from the pilot
(pipefail-loss in the CSV column) was: two responsibilities sharing
one piece of bash.

**Decision:** ship `tools/testing/selftests/um/soak/run-soak-daemon.sh`
as a sibling. It calls the same per-template runner logic (refactored
out of `run-pilot.sh` into a sourceable `_lib.sh` if convenient, or
duplicated for clarity — the rig is small enough that DRY is not
load-bearing). Both drivers stay independently runnable; both share
the thermal-throttle helper, the template substitution code, and the
`umlctl gate loop` invocation pattern. The daemon driver adds, on top:
a wall-clock budget, workload rotation, restart-on-fail, persistent
scoreboard, rolling summary, and explicit stop conditions.

### 2.2 CLI + environment interface

```
run-soak-daemon.sh [--budget-sec N] [--workloads w1,w2,...]
                   [--workers W] [--iters-per-rotation M]
                   [--out PATH] [--fail-threshold-pct PCT]
                   [--fail-threshold-window N]
                   [--continue-on-fail-threshold]
                   [--dry-run]
```

| Flag                              | Env var                  | Default        | Purpose |
|-----------------------------------|--------------------------|----------------|---------|
| `--budget-sec N`                  | `SOAK_BUDGET_SEC`        | `86400` (24 h) | Wall-clock budget. Daemon stops when `now() - start >= N`. |
| `--workloads w1,w2,...`           | `SOAK_WORKLOADS`         | `memcheck,iocheck,stress-ng,cpython-soak,kbuild-tiny` | Rotation order; daemon cycles through this list. |
| `--workers W`                     | `SOAK_WORKERS`           | `2`            | Parallel workers per `umlctl gate loop`. Same semantics as `run-pilot.sh -W`. |
| `--iters-per-rotation M`          | `SOAK_ITERS`             | `10`           | Iters per workload per rotation pass. |
| `--out PATH`                      | `SOAK_OUT`               | `phase-J-soak-<ISO>` under `$PWD` | Artefact tree root. |
| `--fail-threshold-pct PCT`        | `SOAK_FAIL_THRESH_PCT`   | `5`            | Per-workload rolling-window failure-rate threshold. |
| `--fail-threshold-window N`       | `SOAK_FAIL_THRESH_WIN`   | `50`           | Window size (iterations). |
| `--continue-on-fail-threshold`    | `SOAK_CONTINUE_ON_THRESH`| unset          | If set, log + alert but do not stop the soak when threshold trips. |
| `--dry-run`                       | —                        | unset          | Print rotation plan + first-pass commands; do not invoke `umlctl`. |

Inherited (unchanged) from `run-pilot.sh`:

| Env var               | Default | Purpose |
|-----------------------|---------|---------|
| `UML_KERNEL`          | (must be set) | UML kernel binary to soak. |
| `UMLCTL`              | `/home/mjbommar/bench-bundle/bin/umlctl` | umlctl path. |
| `THERMAL_PAUSE_C`     | `88`    | Pause when max CPU sensor ≥ this. |
| `THERMAL_RESUME_C`    | `75`    | Resume when max CPU sensor ≤ this. |
| `COOLDOWN`            | `30`    | Seconds between workloads. |

### 2.3 Workload rotation

Round-robin across `--workloads`, sweeping both backends per workload
per rotation. One rotation = N workloads × 2 backends × M iters per
worker × W workers. With defaults that's 5 × 2 × 10 × 2 = 200
iterations per rotation.

Pseudo-loop (formal spec; not code):

```
start_ts = now()
rotation_idx = 0
while now() - start_ts < SOAK_BUDGET_SEC:
    for workload in SOAK_WORKLOADS:
        for backend in [kvm-v2, seccomp]:
            if stop_requested(): break_all()
            thermal_check()                    # lifted from run-pilot.sh
            iterate_one(workload, backend, rotation_idx, M, W)
            cooldown(COOLDOWN)
            check_threshold(workload, backend) # may set stop_requested()
    rotation_idx += 1
write_final_summary()
```

Pinning `seccomp` into the rotation as a persistent control is
deliberate: drift between v2 and seccomp under load is the
highest-signal failure mode the merge gate was designed to surface.
Dropping seccomp from rotation to "save time" defeats that. The
elapsed-seconds penalty is roughly 2× over a pure-v2 soak; budgeted.

### 2.4 Thermal throttle handling

Lift `read_max_temp_c()` and `thermal_check()` from `run-pilot.sh`
verbatim; same `THERMAL_PAUSE_C` / `THERMAL_RESUME_C` knobs; same
fallback chain (`k10temp`/`coretemp`/`zenpower`/`cpu_thermal` →
`thermal_zone`). The pilot's W=4 → 92 °C observation under
short-burst MM workloads predicts that a 24-hour MM-heavy mix at
W=2 may still trip the throttle on poorly-cooled enclosures; that's
expected and the throttle is the correct response.

Daemon-specific addition: log every throttle pause/resume to
`scoreboard.jsonl` as a `gate=thermal` row (so post-soak analysis
can attribute lost wall-clock to throttling, separately from
workload time).

### 2.5 Restart-on-fail semantics

The daemon **never stops on a single iteration FAIL**. The whole
point is to rack up volume; stopping on first failure converts a
soak into a spot check. Concretely:

- `umlctl gate loop` returns exit code reflecting its own failures
  (timeout / panic / non-zero exit from init.sh). The daemon
  unconditionally appends per-iteration scoreboard rows and
  proceeds to the next workload.
- A guest-side panic (umlctl recognises the kernel-panic line via
  `--fail-marker`) is logged, the bundle is preserved (umlctl gate
  loop already does this via `--out`), and the next rotation
  continues.
- A host-side crash (`umlctl` itself dies, `gate loop` returns
  non-zero from a transport error rather than from a guest verdict)
  is handled identically: log to `scoreboard.jsonl` with a
  `host_error` flag, sleep `COOLDOWN`, continue.
- Only the threshold check (§2.8) can stop the soak; all other
  failures are logged and absorbed.

### 2.6 Per-iteration scoreboard schema

Reuse `tools/testing/selftests/um/scoreboard.jsonl` — same file, same
format used by `umlctl gate run` and consumed by `umlctl gate diff`.
Adding a new gate label (`phase-J-soak-<workload>`) keeps the soak
rows separate from the regular gate rows so `gate diff` doesn't
confuse cumulative soak counters with per-commit gate counters.

Per-iteration row schema (one JSON object per line; fields beyond
the existing scoreboard schema are additive):

```json
{
  "ts": "2026-05-08T14:23:11.456789Z",
  "gate": "phase-J-soak-cpython-soak",
  "backend": "kvm-v2",
  "commit": "82df9571eb25",
  "branch": "umlctl-deploy",
  "kernel": "/home/mjbommar/src/uml-builds/uml-smp-t41fix/linux",
  "host": "server7",
  "run_id": "01KQEY9CA0VZKRP8EYMA1H2MMX",

  "soak_run_id": "phase-J-soak-2026-05-08T0900Z",
  "rotation_idx": 17,
  "iter_idx_within_workload": 412,
  "iter_idx_global": 3420,
  "workload": "cpython-soak",
  "workers": 2,
  "iter_within_worker": 8,
  "worker_idx": 1,

  "rc": 0,
  "duration_ms": 73214,
  "verdict": "PASS",
  "signal": null,
  "panic": false,
  "timeout": false,
  "host_error": false,

  "max_temp_c_pre": 71,
  "max_temp_c_post": 84,
  "throttle_paused_sec": 0,

  "init_log_relpath": "logs/cpython-soak-kvm-v2-3420.log"
}
```

Field notes:

- `gate` is `phase-J-soak-<workload>` so each workload's rows can be
  filtered with a single grep / jq.
- `run_id`, `host`, `commit`, `branch`, `kernel` come from
  `umlctl gate`'s existing scoreboard row builder. Daemon must
  populate them by calling `umlctl gate run` for each iteration —
  OR — reach into `umlctl gate loop`'s artefact tree and synthesise
  the row from `_loop.log` + the per-worker init.log.
- `verdict` enumerates `PASS|FAIL|TIMEOUT|PANIC|HOST_ERROR|SKIP`.
  `umlctl gate loop` already classifies into PASS/FAIL/TIMEOUT via
  pass-marker / fail-marker / per-iter timeout; daemon refines
  FAIL into PANIC (kernel-panic regex tripped) or generic FAIL
  otherwise.
- `iter_idx_within_workload` is the running counter used as input
  to the Wilson CI computation.
- `init_log_relpath` is the relative path under `$SOAK_OUT/` to the
  per-iteration init.log copy. umlctl gate loop already writes
  these under its `--out`; daemon just records the path.

### 2.7 Per-workload Wilson 95 % CI tracker

For each (workload, backend) tuple maintain running counters
`(n, k)` = `(total iters, passes)`. Wilson 95% confidence interval
for the success probability `p̂ = k/n` is:

```
denom = 1 + z²/n,                   z = 1.959964 (95% two-tailed)
center = (p̂ + z²/(2n)) / denom
half  = (z / denom) * sqrt(p̂(1-p̂)/n + z²/(4n²))
[lower, upper] = [center - half, center + half]
```

Daemon updates `(n, k)` on each scoreboard row append; recomputes
the CI; writes the rolling table to `summary.md` (§2.9). No
external numpy / scipy dep — closed-form arithmetic, ~10 lines of
awk or python3 stdlib.

### 2.8 Stop conditions

Stop when **any** of:

- (a) `now() - start_ts >= SOAK_BUDGET_SEC` — clean stop. Final
  summary written.
- (b) Operator SIGTERM or SIGINT received — daemon installs a trap
  that sets `stop_requested=true`, finishes the in-flight
  `umlctl gate loop` invocation (do **not** kill it mid-iteration —
  partial classification corrupts the scoreboard), writes final
  summary, exits.
- (c) Per-workload rolling-window failure rate exceeds threshold,
  i.e. for some (workload, backend) tuple, the last
  `SOAK_FAIL_THRESH_WIN` iterations show `>SOAK_FAIL_THRESH_PCT %`
  FAIL. Default: >5% over rolling 50 iters → STOP + alert.
  - "Alert" = print a banner to stderr + write a `THRESHOLD_TRIPPED`
    file to `$SOAK_OUT/`. CI integrations may watch for that file.
  - `--continue-on-fail-threshold` overrides (c) — useful for
    "we know cpython-soak flakes at 8% on this kernel, soak the
    other workloads anyway."

The threshold check **only stops the soak**; it does not delete or
quarantine artefacts. Post-mortem reads `$SOAK_OUT/scoreboard.jsonl`
and the preserved init.logs.

### 2.9 Output artefact tree

```
phase-J-soak-2026-05-08T0900Z/
├── config.json                  # snapshot of all env vars + CLI args
│                                # at start; lets a reviewer reproduce
├── scoreboard.jsonl             # one row per iteration (§2.6)
├── summary.md                   # rolling Wilson-CI table (§2.10)
├── THRESHOLD_TRIPPED            # written iff stop condition (c) fired
└── logs/
    ├── memcheck-kvm-v2-0.log    # per-iteration umlctl init.log
    ├── memcheck-kvm-v2-1.log
    ├── ...
    ├── cpython-soak-seccomp-3420.log
    └── ...
```

`config.json` schema:

```json
{
  "soak_run_id": "phase-J-soak-2026-05-08T0900Z",
  "start_ts": "2026-05-08T09:00:00Z",
  "host": "server7",
  "kernel": "/home/mjbommar/src/uml-builds/uml-smp-t41fix/linux",
  "commit": "82df9571eb25",
  "branch": "umlctl-deploy",
  "umlctl_version": "0.x.y",
  "stress_ng_version": "0.20.01",
  "args": {"budget_sec": 86400, "workers": 2, "iters_per_rotation": 10,
           "workloads": ["memcheck","iocheck","stress-ng",
                         "cpython-soak","kbuild-tiny"],
           "fail_threshold_pct": 5, "fail_threshold_window": 50},
  "env": {"THERMAL_PAUSE_C": 88, "THERMAL_RESUME_C": 75, "COOLDOWN": 30}
}
```

`summary.md` is rewritten at end of every workload phase and on
SIGUSR1 (so an operator can `kill -USR1 $pid` to force an update).
Format:

```
# Phase J soak summary — 2026-05-08T09:00Z

Start:    2026-05-08T09:00:00Z
Now:      2026-05-08T15:30:11Z   (06:30:11 elapsed)
Budget:   24:00:00 (27.1% consumed)

| workload      | backend | n    | pass | fail | rate    | Wilson 95% CI         |
|---------------|---------|------|------|------|---------|-----------------------|
| memcheck      | kvm-v2  | 480  | 480  | 0    | 100.00% | [99.20%, 100.00%]     |
| memcheck      | seccomp | 480  | 480  | 0    | 100.00% | [99.20%, 100.00%]     |
| iocheck       | kvm-v2  | 480  | 480  | 0    | 100.00% | [99.20%, 100.00%]     |
| iocheck       | seccomp | 480  | 480  | 0    | 100.00% | [99.20%, 100.00%]     |
| stress-ng     | kvm-v2  | 480  | 478  | 2    | 99.58%  | [98.50%, 99.88%]      |
| stress-ng     | seccomp | 480  | 480  | 0    | 100.00% | [99.20%, 100.00%]     |
| cpython-soak  | kvm-v2  | 240  | 240  | 0    | 100.00% | [98.43%, 100.00%]     |
| cpython-soak  | seccomp | 240  | 240  | 0    | 100.00% | [98.43%, 100.00%]     |
| kbuild-tiny   | kvm-v2  | 80   | 79   | 1    | 98.75%  | [93.27%, 99.78%]      |
| kbuild-tiny   | seccomp | 80   | 80   | 0    | 100.00% | [95.43%, 100.00%]     |

Throttle pauses: 4 events, 312s total
Last 50 iters per (workload, backend): all under threshold.
```

### 2.10 Rotation log + crash dumps

Every workload phase appends a one-liner to `summary.md`'s
"Rotation log" tail section: timestamp, workload, backend, pass/fail
counts, elapsed, max temperature seen. Useful for spotting a
"workload N suddenly slowed to 2x" trend without reading 86400
scoreboard rows.

If a per-iteration FAIL is a guest-side kernel panic, the daemon
also copies the bundle's `init.log` + the matching umlctl events.jsonl
(if any) into `logs/panics/<rotation>-<workload>-<backend>-<iter>/`
to make post-mortem one `cd` away.

## 3. Tier 1 / 2 / 3 integration

Tier definitions match the STATUS.md "merge gate widens for Phase J"
section. Each tier adds workloads that must be wired into the
daemon-mode rotation. Implementation per tier follows the same
template-toml-template + `umlctl gate loop` pattern the pilot
established.

### 3.1 Tier 1 — pytest on host-installed Python libs

**Workload definition.** In-guest `pytest --tb=short -x <module>`
against `requests`, `cryptography`, and `numpy` already installed
in `/usr/lib/python3/dist-packages/` on the host (verified:
`requests 2.32.5`, `cryptography 46.0.5`, `numpy 2.3.5` present).

Concrete invocation (per template):

```
python3 -m pytest --tb=short -x \
    --pyargs requests.tests cryptography.tests numpy.tests \
    -k "not network and not slow" \
    --timeout=300
```

The `-k "not network and not slow"` is a defensive filter; numpy's
test suite has slow elements that don't fit a soak iteration. Each
package's tests are its own pytest session if the combined session
is too slow — favour breadth (all three packages every iteration)
over depth (full numpy.tests).

**Pre-conditions.**

- **Host:** `python3-pytest` from apt (verified candidate
  `9.0.2-4`; not currently installed). One-time
  `apt-get install python3-pytest` adds pytest to host's
  `/usr/lib/python3/dist-packages/` which the guest mounts via
  hostfs. No additional steps inside guest.
- **Guest:** PYTHONPATH already pointed at the right hostfs paths
  by the existing cpython-soak template's env block; reuse.
- **Kernel:** no rebuild needed; same `UML_KERNEL` as the pilot.

**Expected wall-clock per iteration.** Empirical estimate from
upstream CI: requests/cryptography ~30 s each; numpy ~120 s. Budget
210 s per iteration; daemon `timeout_secs = 360` to leave headroom.
At W=2 / M=10 that's ~35 min per workload-rotation under v2 (and
similar under seccomp).

**Templates to add.**

- `tools/testing/selftests/um/soak/tier1-requests.toml.template` —
  single phase invoking `pytest --pyargs requests.tests`. Reports
  PYTEST_OK / PYTEST_FAIL via the same `&& echo ... || (... ; exit 1)`
  pattern that closed pilot bug 1.
- `tier1-cryptography.toml.template` — same shape, different
  `--pyargs` target.
- `tier1-numpy.toml.template` — same shape; with the `-k "not slow"`
  filter built in. Larger `[runtime] mem` (1 G or 1.5 G) — numpy
  tests allocate large arrays.

Skeleton (non-code description): each is a copy of
`cpython-soak.toml.template` with the `pytest` phase's `cmd`
replaced and the `[instance].name` updated to `soak-tier1-<pkg>-…`.

**Failure modes + classification.**

| Mode                                         | Daemon classification | Notes |
|----------------------------------------------|-----------------------|-------|
| `pytest` exits 0                             | PASS                  | Standard. |
| `pytest` exits 1 (test failures)             | FAIL                  | A genuine v2 regression vs seccomp is the whole point of Tier 1. |
| `pytest` exits 2 (collection error)          | FAIL                  | Means the host install is broken — operator should fix, not the kernel. Still surface as FAIL so the rig isn't silently green when it's actually unconfigured. |
| `pytest` SIGSEGV / SIGABRT / SIGILL          | FAIL (PANIC=false, signal=N) | This is the original "Python C-extension import" P0 surface; flag as PANIC=false because the kernel didn't panic, but record the signal. |
| Iteration timeout (no `REPRO_DONE`)          | TIMEOUT               | Likely numpy slow-path; raise `--timeout=N` if persistent. |
| Pre-existing seccomp failure (parity)        | (recorded as FAIL on both backends) | If both backends FAIL the same test, that's a host-install / pytest-version issue, not a v2 regression. Daemon does not auto-classify as SKIP — operator triages via the parity column in `summary.md`. |

### 3.2 Tier 2 — pip install + pytest

**Workload definition.** `pip install <pkg> && pytest <pkg>`
inside the guest, exercising tarball download, dependency
resolution, wheel install, and post-install pytest in one phase.
Initial target packages: `httpx` (async HTTP client; small),
`pyyaml` (binary extension; small), `pendulum` (datetime; small,
has its own pytest battery).

**Network strategy decision.** Four options for getting Python
packages into the guest:

| Option | Pros | Cons | Verdict |
|--------|------|------|---------|
| (a) tap+NAT via umlctl | Closest to "real" pip install; exercises virtio-net + UML net stack | Requires `CONFIG_UML_NET_VECTOR=y` rebuild + iptables NAT setup + DNS. Couples Tier 2 to host network reachability (CI runs offline). | NO. Couples too tightly. |
| (b) Local pypi mirror (bandersnatch) | Reproducible; reusable across many hosts; no host network at run-time. | Bandersnatch full mirror is ~12 TB; partial mirror needs explicit package-list curation. Operator-time setup cost is high. Still needs guest networking. | NO. Overkill for ~10 packages. |
| (c) Prefetched wheels + `pip install --no-index --find-links` | No guest networking; deterministic; reproducible offline; minimal disk. | Operator must `pip download <pkg> -d wheelhouse/` once per version; refresh on upgrade. Also needs `python3-pip` apt-installed on host. | NO. `uv` does this strictly better. |
| **(d) `uv run --with <pkg>` with offline cache** | Single statically-linked binary (no apt-get); 10-100× faster than pip; native `UV_OFFLINE=1` mode; isolated venv per invocation (sidesteps the cryptography-then-numpy import-chain pathology we hit in Tier 1); `~/.cache/uv` is hostfs-shared with guest so warming once on the host primes every iteration. | New dependency (curl-installed at `~/.local/bin/uv`). | **YES.** |

**Decision: option (d) — `uv` with offline cache.** Rationale:
strictly dominates option (c) on every axis the memo's
discipline rules care about (no apt dependency, no wheelhouse
hygiene burden, faster, and `uv`'s isolated venv naturally
sidesteps the Tier-1 numpy-after-cryptography import-chain
pathology that's still tracked as a separate UML-environment
follow-up). `uv` ships as a single statically-linked Rust
binary; install on the host with `curl -LsSf
https://astral.sh/uv/install.sh | sh` and it lives at
`~/.local/bin/uv`, visible inside the guest via hostfs at the
same path.

Bootstrap (one-time, host-side; documented in rig README):

```
# Install uv on host (one-time):
curl -LsSf https://astral.sh/uv/install.sh | sh
export PATH="$HOME/.local/bin:$PATH"

# Warm the cache for the Tier 2 packages (still online, one-time):
UV_CACHE_DIR=/var/lib/uml-soak/uv-cache \
  uv run --with httpx --with pyyaml --with pendulum --with pytest \
  python -c "import httpx, yaml, pendulum, pytest; print('warmed')"
```

After bootstrap the cache is filled; subsequent iterations run
fully offline with `UV_OFFLINE=1`.

Per-iteration in-guest invocation (template):

```
UV_OFFLINE=1 UV_CACHE_DIR=/var/lib/uml-soak/uv-cache \
  /home/mjbommar/.local/bin/uv run --with httpx --with pytest \
    python -m pytest --pyargs httpx -k "not network" --timeout=120 -x \
  && echo TIER2_OK || (echo TIER2_FAIL; exit 1)
```

**Pre-conditions.**

- **Host:** `uv` installed (one-time curl); bootstrap step
  above. Map `/var/lib/uml-soak/uv-cache` and `~/.local/bin/uv`
  into the guest via hostfs (both readable from any cwd, no
  guest writes needed at run time when `UV_OFFLINE=1`).
- **Guest:** No network needed when `UV_OFFLINE=1`. `uv`
  resolves all packages from the cache; iteration starts a
  fresh isolated venv so package-state from a previous Tier-1
  iteration cannot contaminate this one (the bug behind the
  `cryptography → numpy` import-chain failure that Tier 1
  currently works around by dropping numpy).
- **Kernel:** no rebuild needed.

**Expected wall-clock per iteration.** Per package
(post-warm): ~1-3 s venv setup + ~30 s pytest ≈ 35 s. Three
packages serially ≈ 105 s. Budget timeout 300 s. (vs ~120 s
for pip's path, so `uv` is net ~15 s faster per iteration —
amortises ~30 % off a 24 h soak's Tier 2 wall-clock.)

**Templates to add.**

- `tier2-uv-httpx.toml.template`
- `tier2-uv-pyyaml.toml.template`
- `tier2-uv-pendulum.toml.template`

Same skeleton; each has a single `tier2-uv` phase invoking the
`uv run --with <pkg>` chain. The bootstrap "clear `/tmp/site-
packages` + PYTHONPATH" dance from the wheelhouse approach is
unnecessary — `uv` manages its own ephemeral venv per
invocation.

**Tier-1 numpy-after-cryptography bug — likely moot under `uv`.**
The bug filed as #17 is: in UML hostfs, `import cryptography`
then later (even in a fresh Python subprocess) `import numpy`
trips a spurious `numpy.__config__` ImportError. Reproduces on
both kvm-v2 and seccomp. Under `uv run --with numpy`, every
invocation runs in a fresh isolated venv with its own site-
packages; the cross-process state-carryover that triggers the
bug shouldn't materialise. Worth re-trying numpy in Tier 1
under a `uv` invocation as a side-effect of switching Tier 2
to `uv` — may close #17 by-construction.

**Failure modes + classification.**

| Mode | Classification |
|------|----------------|
| pip install rc=0 + pytest rc=0 | PASS |
| pip install rc!=0 (wheel missing / corrupted) | FAIL — operator triage; means wheelhouse is stale. |
| pip install rc=0, pytest rc!=0 | FAIL — real signal. |
| Iteration timeout | TIMEOUT |
| SIGSEGV/SIGILL inside C extension load | FAIL (signal recorded) — the original P0 class on a fresh package. |

### 3.3 Tier 3 — Django / FastAPI loopback server

**Workload definition.** Run a tiny HTTP server inside the guest;
hit it from the host via TCP over a TAP interface; verify a known
response payload + record round-trip latency. Tests virtio-net,
the UML network stack, full Python stack with C extensions
(asyncio, ssl), and process longevity (server stays up across N
requests within a single boot).

Two sub-templates, one per framework:

- `tier3-django` — Django 5.x, single view returning `{"ok": true}`
  + a view that loads cryptography, a view that does a numpy
  matmul. Ensures the C-extension stack stays alive under load.
- `tier3-fastapi` — FastAPI + uvicorn, three endpoints with
  matching surfaces. Async path exercises asyncio + uvicorn worker
  pool — different syscall mix than Django's WSGI sync.

**Pre-conditions.**

- **Host packages:** `python3-django` (apt, candidate
  `3:5.2.9-0ubuntu4`; not installed), `python3-fastapi` (`0.118.0-1`;
  not installed), `python3-uvicorn` (`0.38.0-1`; not installed). One
  -time `apt-get install python3-django python3-fastapi
  python3-uvicorn`.
- **Kernel rebuild:** `CONFIG_UML_NET_VECTOR=y` (or virtio-net via
  the `umlctl up` TAP path — preferred; Tier 3 uses the launcher's
  `[network] mode = "tap"` config). Operator runs once:

  ```
  cd ~/src/uml-builds/uml-smp-t41fix
  ./scripts/config --enable CONFIG_UML_NET_VECTOR
  make ARCH=um O=$(pwd) olddefconfig
  make ARCH=um O=$(pwd) -j8
  ```

  Verify with `grep CONFIG_UML_NET_VECTOR=y .config` → must show `=y`.

- **Host networking:** umlctl already provisions tap0 + iptables
  NAT when an Umlfile says `[network] mode = "tap"` (see
  `tools/uml/uml-launcher/src/backend/net.rs`). The daemon driver
  does NOT need to add new network logic — `umlctl up` handles it.

- **Guest:** Django/FastAPI from hostfs `/usr/lib/python3/dist-packages/`,
  same path discipline as Tier 1.

**Canary test.** Per iteration:

1. Phase 1 (in-guest): start server bound to `0.0.0.0:8080`,
   background. Wait for socket-up via a `nc -z 127.0.0.1 8080`
   poll loop with 30 s timeout. Mark phase OK.
2. Phase 2 (in-guest): from inside the guest, hit
   `http://127.0.0.1:8080/health` 100 times serially with curl;
   verify all return HTTP 200 + the expected JSON body.
3. Phase 3 (host-side, run by daemon between umlctl up and
   umlctl stop): from host, hit
   `http://<guest-tap-ip>:8080/health` 100 times via curl; record
   p50/p95 round-trip latency; verify HTTP 200 on all 100.
4. Phase 4 (in-guest): kill server, cleanup, `echo REPRO_DONE rc=0`.

**Expected wall-clock per iteration.** Server startup ~3 s + 200
HTTP round trips at <50 ms each ≈ 15 s. Budget 60 s timeout.

**Templates to add.**

- `tier3-django.toml.template` — has `[network] mode = "tap"`,
  multi-phase init that starts gunicorn, runs the in-guest curl
  battery, exposes a port. Daemon driver coordinates the host-side
  curl battery via the existing `umlctl up --wait-for` mechanism
  (wait for `SERVER_READY` log line, then run host curls, then
  `umlctl stop`).
- `tier3-fastapi.toml.template` — same shape with uvicorn.

**Failure modes + classification.**

| Mode | Classification |
|------|----------------|
| Server starts, all 200 host requests + 100 guest requests succeed | PASS |
| Server fails to bind (EADDRINUSE) | FAIL — host-side leak; rerun should fix. Daemon retries once before recording FAIL. |
| Server starts; some HTTP requests fail (e.g. 500 from server) | FAIL (server-fault) |
| Server starts; some HTTP requests fail at TCP layer (RST / timeout) | FAIL (network-fault) — points at virtio-net / UML net stack. |
| Server SIGSEGV mid-soak (uvicorn worker crash) | FAIL (signal recorded) |
| Iteration timeout | TIMEOUT |

Tier 3 is the highest-value Phase J workload because it exercises
the most surfaces (kernel net stack, Python C extensions, asyncio,
fork+exec for uvicorn workers, long-lived process state) in one
boot. It's also the most likely to surface real bugs in the
network path that Tier 1/2 cannot.

## 4. LTP curation strategy

LTP at `git://github.com/linux-test-project/ltp` is ~1500 tests
across `kernel/`, `fs/`, `mem/`, `net/`, `hugetlb/`, `containers/`,
`ipc/`, `mca/`, `numa/`, `pty/`, `sched/`, `syscalls/`, etc. Many
of these were written for bare-metal kernels and assume facilities
UML cannot have (loadable kernel modules, KASLR, hardware probes,
real network namespaces with bridges). Running the full suite is
not the right baseline; curating to a UML-realistic subset is.

LTP is **not** in apt on this host (verified: `apt-cache policy ltp`
returns nothing). The operator must `git clone && make install`
once. Document the bootstrap step in the rig README; do not
auto-fetch in the daemon.

### 4.1 Categorisation framework

Top-level LTP test directories partitioned three ways:

| Bucket | Directories (top-level) | Rationale |
|--------|-------------------------|-----------|
| **KEEP** | `syscalls/` (most), `ipc/`, `mem/` (most), `pty/`, `sched/`, `mqns/syscalls/`, `signal/` (test-tree subset) | These exercise syscalls UML supports correctly. They are the highest-signal "Linux-API correctness" coverage we can run; v2 vs seccomp drift here would be a real bug. |
| **SKIP** | `kernel/` (loadable-module tests), `cve/` (many require specific kernel versions / features), `containers/` (full namespace setup; UML-hostile), `controllers/` (cgroup v1 specific; UML cgroup is partial), `hugetlb/` (UML mem model has no hugepages by default), `tracing/` (ftrace/kprobes; UML CONFIG turned off in our build), `power_management/`, `dma_thread_diotest/`, `numa/`, `fs/fsstress/` (xfstests-tier; hostfs short-circuits), `network/` (network-namespace + bridge setup), `realtime/` (PREEMPT_RT specific) | Either the test surface doesn't exist in UML, or it depends on host config we don't grant guests, or it tests through interfaces UML implements differently (e.g. hostfs short-circuit for filesystem tests). |
| **TRIAGE-LATER** | `fs/` (the non-fsstress parts; some pass on hostfs, some need real block devices), `mem/oom*` (interesting but UML OOM handling is a separate workstream), `ipc/posix_*` (a subset; needs validation that UML exports the right `/dev/shm` semantics), `mca/` (machine-check architecture; partly emulable) | Worth running but each test needs case-by-case review. Skip for the initial Phase J wiring; revisit after v2 lands. |

### 4.2 Skip-list seed (~30 entries)

A first-pass deny list, by LTP test name. The daemon's LTP runner
template passes this as `runltp -S <skipfile>`:

```
# Loadable kernel modules — UML doesn't load modules
add_key01
add_key02
add_key03
add_key04
add_key05
keyctl01
request_key01
request_key02

# Hardware probes / I/O ports — UML has no /dev/mem-style access
mem02
read_all
ioctl08
ioperm01
iopl01

# Network namespace setup (would need umlctl tap)
netns_breakns
netns_netlink
netns_sysfs

# Hugepages / hugetlb — UML doesn't expose hugetlbfs by default
hugemmap01
hugemmap02
hugemmap03
hugeshmat01
hugeshmctl01
hugeshmget01

# Realtime / PREEMPT_RT — UML is CONFIG_PREEMPT_VOLUNTARY
sched_priority_01
prio-pre

# Container / namespace heavy — out of scope for guest-internal LTP
clone_setns
unshare01
unshare02

# Filesystem-tier tests that hostfs short-circuits
mount01
mount02
mount03
umount01
umount02

# CVE-specific tests targeting features UML doesn't have
cve-2017-2671   # SCTP-specific
cve-2017-7374   # F2FS-specific
cve-2018-13405  # inode-permission edge in real FS
```

This is a seed, not a final list. Expanded post-merge as we learn
which tests reliably fail on UML for environment reasons (vs. real
bugs). The goal of the seed is to get to a "first run that
completes" so we have a baseline to triage.

### 4.3 LTP harness shape

LTP integrates as one more soak template (`tier-ltp.toml.template`),
*not* as a separate driver. Same `umlctl gate loop` shape; same
artefact tree; same scoreboard rows. Key differences:

- **Per-iteration is one full LTP run (with skiplist applied), not
  one LTP test.** runltp's own pass/fail counters become the
  iteration's verdict input. A representative invocation:

  ```
  /opt/ltp/runltp -S /etc/ltp-skiplist -p -q \
      -l /tmp/ltp.log -o /tmp/ltp.out -d /tmp \
      -f syscalls,ipc,mem,pty,sched
  ```

- **Verdict is a ratio, not a binary.** The daemon parses runltp's
  summary table:

  ```
  Total Tests:        842
  Total Failures:     7
  Total Skipped:      31
  ```

  Iteration scoreboard row carries `pass`, `fail`, `skip`,
  `conf_skip` as separate counters. Iteration-level PASS/FAIL
  classification: PASS iff `fail == 0` (and total > 0). Otherwise
  FAIL.

- **Acceptance is calibrated to LTP's known UML-incompat
  baseline.** Phase J doesn't require LTP at 99.5%; that's not
  realistic. It requires LTP at *the same headline* as a clean
  seccomp run on the same kernel, ±1%. If kvm-v2 LTP fail-count
  is within ±1% of seccomp's, that's parity. If kvm-v2 fails
  tests that seccomp passes, that's a v2 regression and a real
  Phase J finding.

- **LTP runs every Nth rotation, not every rotation.** A full LTP
  run at ~30 min wall-clock would dominate the daemon's budget.
  Schedule it once per rotation-pass-number divisible by 8 (so
  ~once every 4-6 hours under default settings); the workload-
  rotation list gets a synthetic `tier-ltp` entry that the daemon
  treats specially (skip unless rotation_idx % 8 == 0).

### 4.4 LTP integration vs deferral

This memo specs LTP wiring; it does not require LTP to **pass** by
the time STATUS row J flips to DONE. Empirically, LTP on UML has
known failure modes unrelated to v2; demanding 99.5% on LTP would
be moving the goalposts away from "is v2 a correct backend" toward
"is UML a correct kernel for LTP," which is a separate question.

The acceptance criterion (§5) accordingly: **LTP runner integrated,
skip list documented, headline ratio recorded — but the headline
ratio itself is informational, not a gate.**

## 5. Phase J DONE — acceptance criteria

Flip STATUS row J from PENDING to DONE when **all** of:

1. **Daemon-mode 24-hour soak completes cleanly on the five pilot
   workloads.** Specifically:
   - `run-soak-daemon.sh --budget-sec 86400 --workloads
     memcheck,iocheck,stress-ng,cpython-soak,kbuild-tiny`
     completes without operator intervention.
   - For every (workload, backend) tuple in that soak, observed
     pass-rate ≥ 99.5% AND Wilson 95% lower bound ≥ 99.0%.
   - The soak's `summary.md` is committed to
     `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-soak-results-<ISO>.md`
     as the headline record.

2. **Tier 1 wired and one full rotation pass per package.** Each
   of `tier1-requests`, `tier1-cryptography`, `tier1-numpy` has at
   least 40 iterations (W=2 × M=20) with pass-rate ≥ 99.0% AND
   Wilson 95% lower bound ≥ 95.0%. (Lower bar than the 5-workload
   soak — these third-party libraries have their own flakes; we
   want to confirm v2 doesn't add to them, not that they're
   bug-free.)

3. **Tier 2 wired and one full rotation pass per package.** Same
   bar as Tier 1 for `tier2-pip-httpx`, `tier2-pip-pyyaml`,
   `tier2-pip-pendulum`.

4. **Tier 3 wired and one full rotation pass per framework.**
   Same bar for `tier3-django`, `tier3-fastapi`. Additionally:
   the host-side curl battery records p50 round-trip latency
   <100 ms (loopback budget; well above what virtio-net should
   exhibit even under UML).

5. **LTP runner integrated, skip list documented, headline ratio
   recorded.** No specific pass-rate gate — just that the runner
   exists, runs, and produces a comparable v2 vs seccomp number.
   Initial finding gets its own follow-up tracking issue if v2
   diverges materially from seccomp on LTP.

6. **No new SMP-T<n> regressions opened by the soak.** If the soak
   surfaces a new failure class (analogous to T57 from the pilot),
   that is itself a Phase J finding and gates the row flip until
   the new T<n> is at least characterised (memo + STATUS row).

The current open T-tickets (T55, T57) do **not** gate Phase J:

- T55 (perf-py-startup ratio) is a perf gate, not a correctness
  gate. The fix plan in state-audit/23 closes it; that work is
  parallel to Phase J validation.
- T57 (stress-ng `--vm` SIGILL) is excluded from the soak's
  stress-ng template per the pilot decision; reasonable because
  the iteration's classification is independent of that disabled
  stressor. T57 closure is tracked separately.

## 6. Risk register

Top three risks to Phase J completion, ranked by likelihood.

### 6.1 Thermal throttle on Zen 4 under sustained MM load

The pilot ran W=2 for 12 minutes and topped out at 86 °C, just
under the 88 °C pause threshold. A 24-hour soak at the same W=2
will accumulate longer thermal-saturation windows; a poorly cooled
enclosure may trip pause/resume frequently, eating wall-clock
budget. Possible mitigations (in order of preference):

- **Lower W to 1 on memcheck / iocheck / kbuild-tiny.** These are
  the MM-heavy workloads; halving worker count halves heat output
  on those phases without touching the IPC stressors.
- **Raise `THERMAL_PAUSE_C` to 90 on hosts with stronger cooling.**
  Document that the threshold is an operator knob, not a
  correctness invariant.
- **Schedule MM-heavy and IPC-heavy workloads alternately** so
  cooldown happens during the IPC phase. The pilot's rotation
  order already trends this way (memcheck/iocheck first, stress-ng
  middle); documented as the canonical order.

Probability: high (will trigger on most laptops; rare on rack
servers). Impact: low (graceful — the throttle works, soak just
takes longer wall-clock to hit budget). Acceptance: don't gate on
absence of throttle; gate on the throttle's resume working.

### 6.2 SMP-T57 not yet fixed

`stress-ng --vm --verify` SIGILLs on kvm-v2 (state-audit/24 has the
22/38 vm-method bisect). The pilot disables `--vm` from the
stress-ng IPC profile; this design carries that exclusion forward.
The risk is that **the same control-flow corruption class shows
up in another workload** — perhaps in cpython-soak's heaviest
mmap-using tests — and the soak surfaces it as an anomalous
flake-rate.

Mitigation: the per-workload Wilson CI rolling table is exactly
the instrument for spotting this. If e.g. cpython-soak suddenly
shows 3-5% v2 FAIL with seccomp at 100%, that's the same bug class
emerging — file as a new SMP-T<n>, characterize from preserved
init.logs, *do not* claim Phase J done with the divergence open.

Probability: medium (T57 is a real bug; whether other workloads
hit the same surface is unknown). Impact: medium (would delay
Phase J close pending T-ticket characterization, but soak data
itself is still useful).

### 6.3 SMP-T55 perf regression makes startup-heavy soaks slow

T55 (perf-py-startup ratio walked from 0.444 → 1.250 over four
days) means kvm-v2 is now *slower* than seccomp on bare Python
startup. cpython-soak does many regrtest module imports per
iteration, each a Python startup. kbuild-tiny does many gcc forks
per iteration, each pays a similar always-`KVM_GET_FPU` cost.

Translated to the daemon: cpython-soak iterations may be 1.5-2x
slower than the pilot's 60-90 s budget under v2; kbuild-tiny may
push past 5 minutes per iteration. Effect: the 24-hour budget
yields fewer iterations, so the Wilson CI lower bound doesn't
tighten as fast as it would on a perf-clean kernel.

Mitigation:

- Increase per-iteration timeouts in the affected templates to
  match observed wall-clock at current head; document the budgets
  in the rig README so they aren't mistaken for soft gates.
- Track per-iteration `duration_ms` in scoreboard.jsonl (already
  in §2.6 schema). If duration trends up across rotations, that
  itself is a perf signal — separate from correctness, but
  recorded.
- Don't let perf debt be a correctness gate — the Phase J done
  criteria (§5) call out pass-rate, not iteration count.

Probability: high (already observed). Impact: low for correctness
(soak still runs); medium for confidence intervals (slower CI
narrowing).

### 6.4 Lower-likelihood risks (noted, not detailed)

- **TAP/iptables NAT setup races between concurrent Tier 3
  workers.** umlctl's network setup uses unique tap interface
  names per instance; `gate loop` workers shouldn't collide. If
  they do, classify as host_error and rerun.
- **Wheelhouse staleness for Tier 2.** A `pip download` from 6
  months ago may have wheels that no longer build under current
  Python. Document refresh cadence in the rig README; consider a
  monthly pre-rotation `wheelhouse-refresh.sh` cron.
- **LTP build chain on offline hosts.** runltp wants compilers,
  kernel headers, and `bison`/`flex`/`autoconf`; a stripped CI
  host may not have them. Document host requirements.

## 7. References

### Memos
- `Documentation/virt/uml/redesign/STATUS.md` — phase ledger, Phase
  J row currently PENDING (task #167).
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-pilot-2026-05-05.md` —
  pilot rig design + first 240/240 result.
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/state-audit/22-smp-t41-stress-and-perf.md` —
  T41 closure record + perf-regression first observation feeding T55.
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/state-audit/23-smp-t55-perf-regression-plan.md` —
  T55 fix plan (parallel work, not a Phase J gate).
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/state-audit/24-smp-t57-vmmethod-bisect.md` —
  T57 stress-ng `--vm` SIGILL bisect; informs the IPC-only
  carve-out kept in this design.
- `Documentation/virt/uml/redesign/06-sequencing/milestones.md` —
  M11 closed, M12 (Phase J) is the umbrella this memo serves.

### Code paths

- `tools/testing/selftests/um/soak/run-pilot.sh` — pilot driver,
  reused for thermal-throttle helper + template substitution.
- `tools/testing/selftests/um/soak/README.md` — operator notes;
  Phase J adds the `run-soak-daemon.sh` section + Tier 1/2/3
  bootstrap.
- `tools/testing/selftests/um/soak/{memcheck,iocheck,stress-ng,
  cpython-soak,kbuild-tiny}.toml.template` — five existing
  templates, baseline for the daemon's rotation.
- `tools/testing/selftests/um/soak/t57/` + `t57-vmmethod.toml.template` —
  T57 reproducer kept out-of-rotation (informational only).
- `tools/testing/selftests/um/scoreboard.jsonl` — scoreboard schema;
  daemon extends with the `phase-J-soak-<workload>` gate label
  family.
- `tools/uml/uml-launcher/src/bin/umlctl/main.rs` + `deploy.rs` —
  `umlctl gate loop` driver the daemon delegates to.
- `tools/uml/uml-launcher/src/backend/net.rs` — virtio-net TAP
  backend Tier 3 depends on; `CONFIG_UML_NET_VECTOR=y` precondition.

### Commits

- `82df9571eb25` — current `umlctl-deploy` HEAD; the soak baseline
  kernel.
- `95c95267202e` — Phase J pilot soak rig landing.
- `467aa7d142c0` — SMP-T54 (worker-fd-leak fix; pre-pilot).
- `db9170b5a7b3` — SMP-T56 (LSTAR-EINTR carve-out for gadget body).
- `602e9a27625c` — SMP-T47 (cross-task `KVM_SET_SREGS` WARN).

### Host-state findings (verified during this memo's drafting,
2026-05-07)

- `stress-ng 0.20.01-1` — installed.
- `requests 2.32.5`, `cryptography 46.0.5`, `numpy 2.3.5` — host
  dist-packages, hostfs-visible from guest.
- `python3-pytest`, `python3-django`, `python3-fastapi`,
  `python3-uvicorn` — apt candidates exist; **none currently
  installed.** Bootstrap step required.
- `ltp` / `ltp-tests` — not in apt; clone-and-build required.
- `/opt/ltp` — does not exist; LTP not previously bootstrapped on
  this host.

These pre-conditions are inputs to the operator runbook the rig
README needs to grow alongside the implementation.
