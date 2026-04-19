# Q1 quality-bar automation

Runs and tracks the Phase Q1 checks from
`05-validation/a-plus-quality-plan.md` against a UML profile:

| Check  | What it does                                                  |
|--------|---------------------------------------------------------------|
| gcc    | `make ARCH=um -j4` baseline build                             |
| clang  | `make ARCH=um LLVM=1 -j4` second-compiler build               |
| sparse | `make ARCH=um C=2 CHECK=sparse -j4` static analysis sweep     |
| smatch | `make ARCH=um C=2 CHECK="$SMATCH -p=kernel" -j4` (`SMATCH=$HOME/src/uml-tools/smatch/smatch` by default) |

`checkpatch` is per-patch, run separately, and not in this script.

## Scripts

- `uml-quality-q1.sh [profile]`  
  Runs the four checks in their own clean `O=` trees under `/tmp/uml-q1/<ts>-<profile>/`. Emits a one-line OK/FAIL per check on stdout. Exit 0 if all four pass; non-zero otherwise. Default profile: `research`.
  
  Skip checks via `UML_Q1_SKIP="clang smatch"` (space-separated). Override the output dir with `UML_Q1_OUT=…`. Override jobs with `UML_Q1_JOBS=…`.

- `uml-quality-q1-baseline.sh <run-log-dir> [profile]`  
  After a known-good run, capture the warning/error set as the new baseline at `q1-baseline/<profile>/<check>.txt`. Source paths are normalized so the baselines are portable.

- `uml-quality-q1-diff.sh <run-log-dir> [profile]`  
  Compare a fresh run against the checked-in baseline. Prints regressions (new warnings, exit 1), improvements (warnings gone, exit 2), or both (exit 3). Exit 0 if the run matches the baseline.

## Typical workflow

After a clean checkout:

    bash Documentation/virt/uml/redesign/scripts/uml-quality-q1.sh research
    # ... ~30 minutes; logs in /tmp/uml-q1/<ts>-research/
    bash Documentation/virt/uml/redesign/scripts/uml-quality-q1-diff.sh \
        /tmp/uml-q1/<ts>-research research

If the diff shows regressions, fix them and rerun. If it shows improvements, lower the baseline:

    bash Documentation/virt/uml/redesign/scripts/uml-quality-q1-baseline.sh \
        /tmp/uml-q1/<ts>-research research

## Per-profile notes

- **research** is the recommended Q1 target — it has the largest configured surface (KASAN + UBSAN + KFENCE + FTRACE + KCOV + …) so it exercises the most code paths through each tool.
- **sandbox** is the fastest (smallest config; ~3 min for gcc alone). Use for quick mechanical checks.
- **race** is the only profile with KCSAN; run Q1 on it occasionally to catch any KCSAN-triggered build regressions.
- All profiles must pass `gcc` and `clang` builds; the `sparse`/`smatch` warning sets vary per profile (each surfaces different code).

## Cost on a 4-CPU host

Single profile (cold trees, no cache reuse between checks):

| check  | wall   |
|--------|--------|
| gcc    | ~3-10 min depending on profile |
| clang  | ~5-15 min |
| sparse | ~5-15 min |
| smatch | ~5-15 min |
| **all four (research)** | **~30-45 min** |

Logs are append-once and live under `/tmp/uml-q1/`; they are not checked in. Only the per-profile `q1-baseline/` text files are.
