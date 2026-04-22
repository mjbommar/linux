# uml-diff — semantic diff between two UML kernels

**Status:** PROPOSED — future phase. Not in the A/B/C/D plan.

The generalization of `scripts/uml-cross-backend.sh` — which
already does PTRACE_ONLY vs SECCOMP_ONLY normalized-dmesg
comparison — to arbitrary pairs of `(kernel_binary, boot_args,
workload)`.

## Ideal end-user flow

```text
$ uml-diff \
    --left  ./linux-before  --left-args  "init=/bin/ls_workload mem=256M" \
    --right ./linux-after   --right-args "init=/bin/ls_workload mem=256M"

=== boot comparison ============================================
+ 0.041s  [left]  initcall kfence_init    → +8192 bytes init.data
- 0.041s  [left]  initcall kcsan_init     → -0 bytes init.data
~ 0.084s           Memory: 457488K/548876K  (left)  /  463544K/548876K (right)
+ 0.132s  [right] Loading compiled-in X.509 certificates (new)

=== trace delta (function_graph via workload) ==================
left:  do_syscall_64 → __x64_sys_openat → … → ext4_file_open
right: do_syscall_64 → __x64_sys_openat → … → ext4_file_open_drop_write  ← new frame
```

Normalized output, line-oriented, copy-pasteable into a
pull-request description or a bug report.

## What this builds on

- `scripts/uml-cross-backend.sh` — the normalizer. Already
  strips volatile fields (timestamps, pids, addresses,
  random seeds) from dmesg before diff. uml-diff reuses the
  normalizer verbatim; the generalization is (a) two
  arbitrary `linux` binaries rather than
  PTRACE_ONLY-vs-SECCOMP_ONLY of a single tree, and (b) a
  workload-driven diff mode that captures trace output, not
  just boot dmesg.
- C-04 + C-05 tracing surface gives the richer workload
  trace the diff works over.

## Build shape

- `scripts/uml-diff.sh` in the shell-script tradition of
  the other scripts/ tools. Promote to Rust only if the
  shape outgrows shell.
- Two invocations:

  1. `uml-diff boot <left> <right>` — diffs boot dmesg only.
     The closest to what `uml-cross-backend.sh` does today;
     the minimum increment.
  2. `uml-diff workload <left> <right> -- <workload>` —
     boots both, runs `<workload>` inside, diffs dmesg +
     function_graph output.

- Normalization passes (reuse existing logic):
  - kernel version strings (`Linux version …`) → `<ver>`
  - timestamps at line-start → drop
  - PIDs → `<pid>`
  - kernel-image addresses → `<addr>`
  - random-seed output → drop
- Trace normalizer: drop `cpu` field, drop `task/tid`,
  normalize function-graph durations to buckets (`<10us`,
  `<100us`, `>=100us`), diff the resulting structured
  stream rather than raw text.
- Output: unified-diff-alike with `+`/`-`/`~` markers, and
  a JSON-structured variant (`--format json`) for
  programmatic consumers (07-uml-bisect.md is the primary
  one).

## Prior art

- **`diff --ignore-matching-lines` + `perl`** — the hand-
  rolled version everyone has written. uml-diff is the
  in-tree productization.
- **ShellCheck's `diff --normalize`** (not a thing, but the
  pattern of a normalizer-then-diff is well-known; e.g.,
  `mergetool` pre-passes).
- **Chrome's WPT results diff** — semantic-vs-byte diffing
  for web-platform tests. Similar pattern: raw output is
  noisy; structured diffing after normalization is useful.

## Use cases

- **Bisection oracle** — 07-uml-bisect.md's "output-diff
  criterion" mode calls uml-diff as its check.
- **Pull-request author's self-check** — author runs
  uml-diff between main and their branch, pastes the result
  into the PR, reviewer sees the behavioral delta without
  rebuilding.
- **Maintainer diff review** — "what did this patchset
  actually change in observable boot + runtime behavior"
  as a complement to code-diff.
- **Regression triage** — "kernel is slower since X; show
  me where the time went" — runs with function_graph
  timings.

## Non-goals

- **Source code diff.** Use `git diff`. uml-diff is
  observable-behavior diff, not source diff.
- **Binary diff of vmlinux.** Use `objdiff` or diffoscope.
- **Automatic root-cause.** uml-diff shows the delta; a
  human (or LLM) interprets it. Not trying to be a kernel
  expert system.

## Open questions

- **Q1: trace volume.** Diffing tens of megabytes of
  function_graph output is slow. Options: restrict to
  user-specified subtrees (`--filter __x64_sys_*`), or
  sample. Start with filter; add sampling if needed.
- **Q2: non-determinism handling.** Two runs of the same
  kernel produce slightly different traces (task scheduling,
  hash-randomization). uml-diff should run N=3 per side and
  intersect the per-side common subset, then diff. Avoids
  false-positive diffs from noise.
- **Q3: render a call-graph visual diff.** Ambitious UX.
  Would pair well with Perfetto (12); perhaps uml-diff
  outputs two Perfetto traces and the user's browser does
  the visual diff with the existing Perfetto UI. Defer.

## Effort estimate

1 week for the `boot` mode (~shell script). 2 weeks for
`workload` mode including the trace normalizer.

## Dependencies

- `scripts/uml-cross-backend.sh` (landed) as the
  normalizer.
- C-04 + C-05 tracing surface (landed).
- 07-uml-bisect.md uses this as a criterion but doesn't
  block this from landing first.

## Cross-references

- `Documentation/virt/uml/redesign/scripts/uml-cross-backend.sh`
  — the existing normalizer this generalizes.
- `07-uml-bisect.md` — consumer of the `workload` diff as a
  bisection criterion.
- `12-uml-perfetto-trace.md` — visual-diff angle; deferred.
