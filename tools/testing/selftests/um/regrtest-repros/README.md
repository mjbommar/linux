# regrtest-repros - fast UML/seccomp substrate reproducers

This directory contains targeted C and Python reproducers for the
substrate-level failures observed when running CPython's full
`python -m test` regrtest under seccomp UML.

The full regrtest takes ~15 minutes and exercises ~492 modules, of
which ~27 modules fail under seccomp UML (April 2026 baseline).
That signal is too coarse and too slow to drive iterative substrate
fixes. These reproducers compress the same diagnostic surface into
seconds.

## Layout

```
regrtest-repros/
+-- class-a-env/        Environmental (no tty, no /dev/ptmx, no PATH)
+-- class-b-process/    Process model (fork/exec/wait/multiprocessing)
+-- class-c-syscall/    Real UML kernel / syscall gaps
+-- class-d-structural/ Structural (ITIMER_VIRTUAL stub-child accounting, etc.)
+-- Makefile
`-- run-regrtest-repros.sh
```

Each class directory has its own Makefile and a per-class runner.
The top-level `run-regrtest-repros.sh` boots a UML kernel under
seccomp, executes every reproducer inside, and aggregates results.

## Failure-class taxonomy

- **Class A (environmental)** - Fail because PID-1 init has no
  controlling tty, no `/dev/ptmx`-capable host setup, no inherited
  PATH. Not seccomp/UML correctness bugs. Reproducers exit 0 when
  the expected env-only failure is observed; we use them to
  generate the curated `-x` skip list.

- **Class B (process model)** - Fail because UML's clone-fork-vfork
  stub-child architecture handles paths differently from a normal
  Linux kernel. Several of these may be fixed by moving worker
  ownership to the process model. Reproducers stress the same paths in
  ~50 lines of C.

- **Class C (real UML syscall gaps)** - Fail because UML doesn't
  implement (or implements incorrectly) some host syscall surface.
  Each reproducer pinpoints the specific failing call so we can
  map back to UML kernel code and patch.

- **Class D (structural)** - Need design-level work, not a small
  patch. ITIMER_VIRTUAL accounting under the seccomp stub-child
  CPU-time split is the canonical example.

## Expected outcome and PASS/FAIL semantics

Each reproducer prints exactly one of:

```
REPRO: <name> PASS         # Behavior matches expectation
REPRO: <name> FAIL <why>   # Anomalous result; substrate gap
REPRO: <name> EXPECTED_FAIL <why>  # Known broken; tracking only
```

The aggregate runner exits 0 if every PASS is observed and no
unexpected FAIL occurs. EXPECTED_FAIL transitions to PASS as we
land fixes; that's the substrate-readiness signal.

## Why this is fast

- No CPython regrtest framework overhead (no test discovery, no
  per-method setUp/tearDown).
- Each reproducer is ~50-200 lines of C, runs in <1s host-side.
- Total wall-clock target: <= 60 seconds for the entire suite.
- Compare: 15+ minutes for full `python -m test` regrtest.

## Running

```
# Build all C reproducers
make -C tools/testing/selftests/um/regrtest-repros all

# Run the suite under seccomp UML
UML_BINARY=/tmp/uml-clean/linux \
  bash tools/testing/selftests/um/regrtest-repros/run-regrtest-repros.sh
```

## Companion: `regrtest-skip.list`

`regrtest-skip.list` is the curated list of CPython regrtest modules
to `-x` skip when running `python -m test` under seccomp UML. Each
entry is annotated with the failure class and the reproducer that
exercises the same surface. Use it like:

```
SKIPS=$(grep -v '^#' regrtest-skip.list | grep -v '^$')
python -m test $(printf -- '-x %s ' $SKIPS)
```

When a substrate fix lands and a reproducer flips PASS, remove the
corresponding entry.
