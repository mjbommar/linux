# Memo 30 — Gate discipline

Status: active 2026-04-30
Audience: anyone iterating on the UML backend stack (seccomp, kvm-v2)

## Why this memo exists

We have a recurring failure mode: when a hard bug bites, the path of
least resistance is to *change the gate* — annotate a test
`EXPECTED_FAIL`, relax a threshold, narrow a regex, or skip a class of
reproducers — instead of fixing the underlying issue. That makes the
gate green and CI happy, but the regression is still there, hidden
behind a quieter signal.

This memo writes down the rules we agreed on and gives them a tool —
`umlctl gate` plus a versioned `tools/testing/selftests/um/gates/`
directory of Gatefile descriptors — so the discipline survives
context switches.

## What "good gate" means here

Five properties, in priority order:

1. **Sealed.** The gate runner cannot redefine what passing means.
   It executes a pre-existing harness, parses the harness's stdout,
   and reports the count. Tightening thresholds is progress. Loosening
   thresholds is a TOML diff in this directory — visible in `git
   blame`, requires a memo entry citing the reason.
2. **Specific.** A gate names the behavior under test, not a vibe.
   Not "it boots" — "fork_exec_wait completes 100 iterations under
   class-a-env with backend=force=kvm-v2".
3. **Cheap.** Sub-2 minutes so a developer runs it on every commit,
   not weekly. Anything slower belongs in L7 (soak), not the inner
   loop.
4. **Bisectable.** Same input → same output, no flakes. If a gate
   goes red between commits A and B, the regression is at one of the
   commits between A and B — period.
5. **Monotone.** PASS counts on a substrate gate must never decrease
   silently. A drop is an investigation, not "oh well, retry."

## The L0–L7 ladder

| L | Gate                       | Budget   | What it catches                   |
|---|----------------------------|----------|-----------------------------------|
| 0 | launcher-cargo             | ~5 s     | Host-side tooling regressions     |
| 1 | kernel-build               | 5–60 s   | Compile / link breaks             |
| 2 | kunit-marshal              | ~30 s    | Backend internals (regs, ABI)     |
| 3 | regrtest-substrate       | ~90 s    | Substrate correctness (env)       |
| 4 | regrtest-class-b/c         | ~3 min   | Process / syscall integration     |
| 5 | perf-getpid (ratio gate)   | ~30 s    | Perf regression                   |
| 6 | umlctl up fastapi.toml     | ~30 s    | Real workload end-to-end          |
| 7 | 24h soak                   | overnight| Drift, leaks, intermittent issues |

L0–L3 every commit. L4–L6 every push to a branch. L7 only for phase
gates and milestone candidates (Phase J, memo 26 §J).

## How the runner works

`umlctl gate run -f <gatefile.toml> --backend <name> --kernel <path>`

1. Parses the Gatefile (TOML, schema_version=1, `deny_unknown_fields`).
2. Substitutes `{{backend}}` and `{{kernel}}` into the env section.
3. Wraps the cmd in `timeout --kill-after=5 <budget_sec>` and runs it.
4. Counts PASS/FAIL/EXPECTED_FAIL by grepping stdout for the
   gatefile's regexes. The runner is forbidden from inventing its
   own counts.
5. Compares against `[thresholds]` (`min_pass`, `max_fail`,
   `require_clean_exit`). Any unmet threshold lands in `failures[]`.
6. Appends one row to `tools/testing/selftests/um/scoreboard.jsonl`
   with `{ts, gate, backend, commit, branch, kernel, host, pass,
   fail, expected_fail, exit_code, duration_sec, thresholds_met,
   metrics, failures}`.
7. Exits 0 on PASS, 1 on FAIL.

`umlctl gate diff [--gate NAME] [-n N]` prints the last N rows as a
table, marking PASS-count deltas (`+N` / `-N`) so a silent
regression jumps out.

`umlctl gate list` enumerates gatefiles under
`tools/testing/selftests/um/gates/`.

## The carve-out rule (the thing this memo enforces)

**Whenever you are about to:**

- Add `EXPECTED_FAIL` / `#[ignore]` / `--skip` to a previously-passing
  test
- Edit an assertion or the harness's pass criteria
- Lower `min_pass`, raise `max_fail`, set `require_clean_exit = false`
- Reduce iteration counts or relax timeouts to make a flake go green

**…stop.** Three options:

1. **Fix the bug.** This is the default. The gate exists to flag
   regressions; the regression is the thing.
2. **Document the carve-out.** If the failure is genuinely out of
   scope for this phase (e.g. a kernel feature we haven't built yet),
   add a memo entry explaining: what fails, why deferring is OK,
   what condition resolves the carve-out, and a link to the issue or
   memo phase that owns it. Then loosen the threshold.
3. **Mark the test EXPECTED_FAIL.** Same audit trail as (2) — the
   `EXPECTED_FAIL` line in the harness must point at a memo or task
   ID. EXPECTED_FAIL counts are tallied separately from FAIL so a
   carve-out doesn't silently mask a real regression on the same
   reproducer set.

What you may not do: silently change a test, narrow a regex,
suppress an exit code, or wrap a flake in a retry loop without
documenting why. The git diff for any of those is the audit signal —
keep it visible.

## Concrete current line (2026-04-30)

| Gate                       | seccomp                   | kvm-v2 (substrate)           |
|----------------------------|---------------------------|------------------------------|
| launcher-cargo             | PASS                      | PASS                         |
| kernel-build               | PASS                      | PASS                         |
| regrtest-substrate       | PASS=25 FAIL=3 XFAIL=3    | PASS=6 FAIL=3 XFAIL=0        |
| perf-getpid                | (baseline)                | (v1; see §H.1 for v2)        |

Open carve-outs:

- **#95 fork_exec_wait** SIGILLs on direct init under v2. Carved out
  of class-a-env: counts as one of the FAIL=3 above, not silenced.
  Owner: memo 26 §E.4 follow-up.
- **#96 multi-import Python** crashes UML fatally. Same posture.

Anything that drops `regrtest-substrate` PASS below 6 on v2 or
below 25 on seccomp is a regression, full stop.

## Workflow recipe

```
# Inner loop, every commit:
umlctl gate run -f tools/testing/selftests/um/gates/launcher-cargo.toml
umlctl gate run -f tools/testing/selftests/um/gates/kernel-build.toml
umlctl gate run -f tools/testing/selftests/um/gates/regrtest-substrate.toml \
    --backend seccomp --kernel ~/src/uml-builds/uml-clean/linux
umlctl gate run -f tools/testing/selftests/um/gates/regrtest-substrate.toml \
    --backend kvm-v2 --kernel ~/src/uml-builds/uml-clean/linux
umlctl gate diff -n 8

# Before pushing:
# (add L4–L6 above)

# Phase milestone:
# Run L7 (24h soak) on a stable branch.
```

The scoreboard at `tools/testing/selftests/um/scoreboard.jsonl` is
git-ignored — every developer accumulates their own measurement
history. The gate descriptors and this memo are the version-controlled
spec.
