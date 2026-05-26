# Diary: 08 — SMP-T55 UP-hop bisect (Track C polish item)

**Date:** 2026-05-14 (work performed 2026-05-16).
**Plan task:** `Documentation/virt/uml/redesign/06-sequencing/PLAN-2026-05-14.md`
task #14 (Track C polish).
**Output memo:** `state-audit/T55-up-hop-bisect-2026-05-14.md`.

## What I did

Bisected the +50% UP-hop perf regression on `perf-py-startup`
between `f1e3130a69af` (2026-04-30 baseline) and `ad18db7c3768`
(2026-05-02 peak), 98 commits in the window.

Methodology:
  - `git worktree add` at `$HOME/src/uml-worktrees/t55-bisect/`,
    detached HEAD per bisect step.
  - Per-step build dir at
    `$HOME/src/uml-builds/t55-bisect/step-<short-sha>/` —
    isolates each bisect step's `O=` build per
    `feedback_o_dir_discipline.md`.
  - Build: `make ARCH=um O=<builddir> olddefconfig && make
    ARCH=um O=<builddir> -j16 linux`. ~42s per build on
    server7 (Ryzen 7 7840HS, 16 cores).
  - Config: copied from `$HOME/src/uml-builds/uml-smp-t41fix/.config`
    (CONFIG_SMP=y, NR_CPUS=4, PREEMPT_VOLUNTARY).
  - Measurement: `tools/testing/selftests/um/perf-py-startup/run-perf-py-startup.sh`
    with `SAMPLES=5 MAX_V2_RATIO=0` to read raw numbers.
  - Verdict threshold: ratio ≤ 0.50 → good, > 0.50 → bad.
    Midpoint between baseline 0.333 and peak 0.667.

Six bisect steps (matched log₂(98) ≈ 6.6 prediction).

## Result

**First bad commit: `bd435856948e`** — "um: kvm-v2: re-add
interrupt_end() to syscall arm (substrate parity)", authored
2026-04-30 11:55 EDT.

Per-step kvm-v2 wall-clock medians:

  - parent (`9c61f164adf0`): 0.040 s
  - **`bd435856948e`: 0.120 s** ← +200% on a single commit
  - bad endpoint (`ad18db7c3768`): 0.080 s (later commits in
    the window — most notably `ba9c83331f30` Phase H.2 lazy-FPU —
    partially offset the +200% to a net +100% by end of window).

## Mechanism

The commit adds one line: `interrupt_end();` after
`handle_syscall(regs)` in `kvm_v2_handle_io_trap`. Each
post-syscall path now drains pending TIF_WORK + signal +
`-ERESTART*` translation. Per syscall cost: function-call
overhead + `_TIF_WORK_MASK` bit-test + (if set) `do_signal`
chain. At ~30-50k syscalls during Python startup, the
cumulative cost is ~80ms on this host.

The commit is a **correctness fix** — without it,
`-ERESTARTSYS` (errno 512) leaks to userspace as if it were a
normal errno, and dash's builtin write output is lost after
fork+wait. Substrate gate pre-fix: PASS=6/FAIL=3/EXP_FAIL=3;
post-fix: PASS=25/FAIL=3/EXP_FAIL=3 — full parity with seccomp.
Cannot be reverted.

## Mitigation (already in tree)

Commit `24557e95c4b1` ("um: kvm-v2: perf-O1 — gate post-syscall
interrupt_end() on -ERESTART/_TIF_WORK"), 2026-05-03 22:44 EDT,
inlines the `_TIF_WORK_MASK` gate + `-ERESTART*` range check,
saving ~120 cyc/syscall on the fast path. Validated by its own
bench-micro at -0.32% wall-clock, substrate gate bit-identical.

Today at HEAD (`9baf6a1e9838`), `perf-py-startup` reads ratio
≈ 1.0 (kvm-v2 0.10s, seccomp 0.09-0.10s) — well under the 1.20
gate ceiling. Nothing to do.

## Hypothesis falsified

The task brief's framing — "T55's per-vCPU dirty epoch may be
racing with another change … possibly the gadget LSTAR work or
a TDP-related TDP-flush change" — is wrong on dates:

  - SMP-T55 (`fd2f9639b0ce`) landed 2026-05-07, **six days after
    the bisect window closed**. T55 is the *fix* memo 23 proposed,
    not a cause.
  - The gadget revival (`7ebcd8aac347`) landed 2026-05-04, also
    outside the window.
  - The actual offender is a plain syscall-arm correctness fix,
    no TDP/LSTAR/T55 interaction at all.

Future task briefs should `git log --format='%ci'` cited
SMP-T## commits before framing hypotheses, especially when
memo 22's text already names four candidate commits (none of
which were the actual offender — they fall later in the window).

## Constraints respected

  - No touches to `arch/um/backend/kvm-v2/` source files
    (read-only investigation).
  - No touches to `Documentation/.../upstream-patches/`.
  - All builds used `O=` to per-step dirs; source tree stays
    pristine.
  - Worktree-based bisect — main checkout's untracked
    `tools/testing/selftests/um/soak/t57-*` files preserved.

## Wall-clock + budget

  - 6 bisect steps × ~7 min each (build 42s + measure ~5min) =
    ~45 min total bisect.
  - Initial setup + memo writing: ~30 min.
  - Total: ~75 min, well under the 2-hour budget.

## Followups (optional, not blockers)

  1. Update `state-audit/23-smp-t55-perf-regression-plan.md`
     §3.1 "Confidence: medium. I haven't bisected." to cite
     the new memo's bisect result.
  2. Update `state-audit/22-smp-t41-stress-and-perf.md`
     §"Known perf regression" similarly.
  3. None of memo 22's four named candidate commits
     (`7e1c255a09ad`, `9f0ff6257e8b`, `95b3a85bd309`,
     `e5977806fd14`) were the bisect winner; all are
     innocent. The memo's hypothesis ("cumulative effect across
     ~99 commits") was approximately right but the dominant
     single offender was missed because the framing focused on
     post-2026-05-01 commits.

## Cleanup

Worktree at `$HOME/src/uml-worktrees/t55-bisect/` and the seven
build dirs at `$HOME/src/uml-builds/t55-bisect/step-*/` retained
for one week to allow re-running the bisect harness if any
reviewer wants to verify. Total disk: ~6 GiB. Will tear down at
the end of Track C polish.
