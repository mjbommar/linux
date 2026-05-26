# A-07 perf baselines — per-host file convention

This directory holds the committed perf baselines that
`uml-perf-compare.sh` validates current runs against. The
files are named `perf-baseline-<cpu-slug>.json` — one per
CPU model that has ever committed a baseline.

Why per-host: cycle counts aren't portable across CPU
generations. An Intel Xeon E3-1225 v6 (Kaby Lake, 3.3 GHz)
boots UML `/bin/true` in ~200M cycles; an Intel Xeon W-2123
(Skylake-SP, 3.6 GHz) takes ~370M cycles for the same
workload, not because one kernel is slower than the other
but because IPC, cache, and microcode differ. Cross-host
absolute-cycle comparison yields spurious "regressions" —
the cross-host confusion documented in decisions-log D47 /
D49 cost real investigation time before we fixed the tooling.

## Filename slug

The `uml-perf-capture.sh` / `uml-perf-compare.sh` scripts
both contain a `baseline_slug_for_cpu()` shell function that
maps a `/proc/cpuinfo model name` line to a stable slug:

  Intel(R) Xeon(R) CPU E3-1225 v6 @ 3.30GHz
    → xeon-e3-1225-v6
  Intel(R) Xeon(R) W-2123 CPU @ 3.60GHz
    → xeon-w-2123
  12th Gen Intel(R) Core(TM) i7-12700K
    → 12th-gen-core-i7-12700k
  AMD EPYC 9654 96-Core Processor
    → epyc-9654-96-core-processor

Slug rules: lowercase, strip `(R)` / `(TM)` marks, strip
`@ <freq>GHz`, strip vendor prefix words (`intel`, `amd`,
`cpu`), collapse non-alphanumeric to `-`, trim, cap at 40
characters.

## How to add a baseline for your host

```
# Set governor + pin to cpu0, then:
bash Documentation/virt/uml/redesign/scripts/uml-perf-capture.sh
# → writes perf-baseline-<your-cpu-slug>.json

# Commit it:
git add Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/perf-baseline-*.json
git commit -s -m "um: backend: add perf baseline for <your CPU>"
```

The capture script refuses to run unless the pinned CPU
(default cpu0, override with `UML_PERF_CPU=N`) has
cpufreq governor `performance`. This is the load-bearing
gate: a `powersave` baseline is worse than no baseline.

## How comparison picks a file

`uml-perf-compare.sh` reads `/proc/cpuinfo`, slugs the local
CPU, and loads `perf-baseline-<slug>.json`. If no matching
file exists:

- If a legacy un-slugged `perf-baseline.json` is present,
  the script points at that and refuses to run (comparing
  someone else's baseline is meaningless — see D49). The
  legacy file is kept only during migration.
- Otherwise, the script prompts the user to capture a
  baseline for their host.

The absolute-cycle comparison (same-host-only) and the
backend-ratio comparison (cross-host-portable — added in
commit `7a7d365c5872`) both run against the selected
per-host file. Ratios of `SECCOMP / PTRACE` and
`DYN_seccomp / DYN_ptrace` are the invariant-I2-relevant
signal and are the one to trust for any
is-seccomp-losing-its-advantage question.

## What's in each file

```json
{
  "schema_version": 1,
  "captured_at": "<ISO-8601 UTC>",
  "tree_head": "<git rev-parse --short HEAD>",
  "host": {
    "cpu": "<model name>",
    "ncpu": <nproc>,
    "gcc": "<version>",
    "glibc": "<version>",
    "kernel": "<uname -r>",
    "perf_cpu": <pinned CPU index>,
    "cpufreq_governor": "performance"
  },
  "backends": [
    {"backend": "PTRACE_ONLY",  "iters": 10, "cpu": 0,
     "governor": "performance",
     "cycles": {"p25": ..., "p50": ..., "p75": ...},
     "instructions": {"p25": ..., "p50": ..., "p75": ...},
     "task_clock_us": {"p25": ..., "p50": ..., "p75": ...}},
    ...
  ]
}
```

## Current baselines in tree

| slug | CPU | date | tree_head |
|---|---|---|---|
| `xeon-e3-1225-v6` | Intel Xeon E3-1225 v6 (Kaby Lake, 4 CPUs) | 2026-04-22 | `23c5bc7d17ed` |

## When to refresh a baseline

- After the Q1 runner surfaces a real regression that the perf
  comparator agrees is real (not cross-host noise) and you've
  fixed or justified it — refresh the baseline in the same
  series as the fix. Include the `captured_at` date, governor,
  cpu-pin, and host in the commit message so the bump is
  auditable.
- After a significant host reconfiguration (microcode update,
  thermal recalibration). Note the reason in the commit body.
- **Never** refresh a baseline to silence a regression you
  haven't understood; that defeats the gate's entire purpose.

## See also

- `../../04-risks/decisions-log.md` D47, D48, D49 — the
  cross-host-confusion investigation and the three decisions
  that shaped this convention.
- `../../scripts/Q1-README.md` — companion quality checks
  (compile side).
- `./07-perf-ci.md` — A-07 workstream doc.
