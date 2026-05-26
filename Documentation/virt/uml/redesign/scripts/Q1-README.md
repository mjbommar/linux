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

**Not included (blocked on host toolchain):** Compiler-Based
Context Analysis (`CONFIG_WARN_CONTEXT_ANALYSIS=y`) requires
**clang 22 or newer**. The host in this tree ships clang
21.x, so Context Analysis is out of scope for Q1 today —
tracked as Lift #10 in `06-sequencing/post-q1-push.md`
(Phase VII, deferred). No script plumbing needed until the
toolchain upgrade unblocks it; when that lands, a third
`*_context_analysis.sh` clang-invocation alongside
`gcc`/`clang`/`sparse`/`smatch` is the expected shape (see
`05-validation/a-plus-quality-plan.md` §"Blocked on host
toolchain").

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

- **research** is the default Q1 target — it has the largest non-KCOV configured surface (KASAN + UBSAN + KFENCE + FTRACE + kprobes + BPF JIT + …) so it exercises the most code paths through each tool. KCOV is intentionally **off** in `research` per D31 (the KCOV + FUNCTION_TRACER + stop_machine interaction wedges under load); run Q1 against `fuzz` or `fuzz-deep` when you need to cover KCOV-on build paths.
- **sandbox** is the fastest (smallest config; ~3 min for gcc alone). Use for quick mechanical checks.
- **race** is the only profile with KCSAN; run Q1 on it occasionally to catch any KCSAN-triggered build regressions.
- **fuzz / fuzz-deep** are the only profiles with KCOV; the Q1 default does not cover KCOV. Run `uml-quality-q1.sh fuzz` explicitly before touching anything in `kernel/kcov.c`, the forkserver, or the sanitizer coverage plumbing.
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

## Related quality scripts in this directory

`uml-quality-q1.sh` is the compile-side gate. Three complementary scripts live alongside it:

- `uml-boot-matrix.sh` — builds the 12-variant PTRACE_ONLY / SECCOMP_ONLY / DYNAMIC backend-dispatch matrix and asserts each boots to `init=/bin/true` with the expected backend. Use after any change touching `arch/um/kernel/backend.c`, `arch/um/os-Linux/start_up.c`, or the per-backend `lifecycle.c` files.

- `uml-cross-backend.sh` — boots PTRACE_ONLY and SECCOMP_ONLY kernels with identical boot args, diffs the normalized dmesg to assert observable equivalence between backends. Use to catch backend-specific divergence.

- `uml-perf.sh` + `uml-perf-capture.sh` + `uml-perf-compare.sh` — cycle/instruction/task-clock measurement via `perf stat` across the four backends. Output is always two things:
  - **Absolute-cycle delta** (per-backend p50 vs baseline). Same-host-only signal; meaningless across CPU generations (see `04-risks/decisions-log.md` D47/D49 for the cross-host confusion this caused).
  - **Backend-ratio delta** (`SECCOMP / PTRACE`, `DYN_seccomp / DYN_ptrace`). Cross-host-portable — cancels per-CPU IPC/cache/microcode differences. This is the signal that matches invariant I2 ("seccomp retains its advantage vs ptrace"); when interpreting runs across different dev hosts, it's the one to trust.

  All three perf scripts refuse to run unless the cpufreq governor on the pinned CPU is `performance` (env `UML_PERF_CPU=N` to pin to CPU N, default 0). Override with `UML_PERF_FORCE=1` for debug-only measurements.
