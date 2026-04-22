# uml-bisect — unattended bisection with a UML boot + test as the oracle

**Status:** PROPOSED — future phase. Not in the A/B/C/D plan.

The shortest-path argument: a UML build + boot + test takes
minutes. `git bisect` on bare metal takes hours per iteration
(flash/reboot/test). A UML-driven bisection runs end-to-end
unattended in the time a human operator takes to notice the
first iteration completed.

## Ideal end-user flow

```text
$ uml-bisect \
    --good v6.15 \
    --bad HEAD \
    --profile research \
    --check ./check-dmesg-has-event.sh

Bisecting: 312 revisions left to test after this (roughly 8 steps)
... building abcd1234 ... booting ... check ... good
... building ef567890 ... booting ... check ... bad
...
Found: 3f2a9b7c "net: sched: refactor cls_flower skip_sw fallback"
       (2026-03-18, Jamal Hadi Salim)
       boot log diff saved to /tmp/uml-bisect-3f2a9b7c/
```

The `--check` script runs inside the booted UML (via the
C-10 v2 console backend or hostfs-exec); exit 0 means "good",
nonzero means "bad". uml-bisect handles the git-bisect
bookkeeping + the build/boot/test loop + artifact capture.

## What this builds on

Entirely landed infrastructure:

- `scripts/uml-boot-matrix.sh` — the build+boot primitive.
  uml-bisect reuses it for the per-revision build.
- `scripts/uml-quality-q1.sh` — Q1 gate per revision if the
  user asks for it (`--require-q1-clean`).
- `scripts/uml-cross-backend.sh` — the "normalize dmesg"
  logic that lets uml-bisect compare per-revision boot
  output semantically.
- `scripts/uml-perf-compare.sh` — per-host perf baselines.
  A perf-regression bisection (`--criterion perf`) uses it
  as the check.
- C-09 forkserver — `--fast` mode: rather than rebuilding
  the kernel per revision, bisect the *user-visible symptom*
  inside a single snapshot by restoring and running a
  different workload. Different semantic, enables a much
  larger revision-space search.

## Build shape

- `scripts/uml-bisect.sh` (bash) or `tools/uml/uml-bisect/`
  (Rust binary). Start with shell, promote to Rust when the
  bookkeeping grows beyond comfort.
- Wraps `git bisect run` with a check script that:
  1. Reuses the worktree + build-dir pattern from
     `uml-boot-matrix.sh` (O= build dir keyed to the bisect
     step's sha).
  2. Boots under the selected profile with `timeout`.
  3. Routes the user's `--check` into the guest's init
     (hostfs mount, or via the C-10 v2 console backend once
     that lands).
  4. Captures dmesg + trace into
     `/tmp/uml-bisect-<sha>/`.
  5. Exits with the guest check's exit code.
- The bash wrapper maps to `git bisect run` cleanly; no
  reinvention of the bisection loop.

## Check-script contracts

Three canonical check-script modes:

1. **Symptom reproduces** — the simplest. Check exits 0 if
   the symptom is absent, 1 if present. "Git bisect bad"
   means "symptom present on this revision".
2. **Output-diff criterion** — check dumps dmesg/trace +
   `diff` against a reference. "Bad" = diff non-empty.
   Composes with 09-uml-diff.md's normalizer.
3. **Perf regression** — check runs a microbench, compares
   p50 against a baseline via
   `scripts/uml-perf-compare.sh`, exits "bad" on
   >5% regression. Per-host baselines (commit 468000bb8b0e
   landed) make this safe.

## Prior art

- **git bisect run** — the primitive. Nothing novel.
- **syzkaller bisect** — bisects crash-reproducers in QEMU.
  Same shape, slower iterations.
- **cbisect / b4 ty** — kernel-community bisection helpers.
  All of them are patterns for "automate `git bisect run`
  with a kernel-specific test". uml-bisect is one more, with
  UML's faster boot as the wedge.

## Non-goals

- **Distributed / parallel bisection.** Single host, single
  bisect bookkeeping file. Multi-host is 11's territory (UML
  instances orchestrated via the REST API could in principle
  share a bisection).
- **Non-bisect search strategies** (binary-search on
  `Fixes:` metadata, blame-driven culprit picking). Just
  bisection.
- **Bisecting across config changes.** `--config-fragment`
  is held constant across the bisect; changing config is a
  separate search problem.

## Open questions

- **Q1: build cache reuse across bisect steps.** Naive
  per-sha `O=` dir means rebuilding the world each step. A
  ccache or sccache layer in front of gcc/clang cuts that
  substantially; worth the extra setup for any bisection
  spanning more than a handful of steps.
- **Q2: how to capture `good`/`bad` artifacts for
  postmortem.** Save full dmesg + trace per step? That's
  GB-scale on a long bisect. Keep the last-good + first-bad +
  culprit; delete the rest. Configurable with
  `--keep-all-logs`.
- **Q3: boot-timeout policy.** A revision that hangs on
  boot has to fail-open to "skip" (git bisect skip), not
  "bad", or bisection will converge on a non-culprit. Default
  timeout = 120s, configurable.
- **Q4: check-script sandbox.** The script runs inside the
  booted UML. Guest has hostfs access; the script can
  clobber host files outside `~/.local/state/uml/<bisect-
  instance>/` if careless. Document the sharp edge; don't
  try to solve it with extra seccomp in this tool.

## Effort estimate

2-3 days for a working `scripts/uml-bisect.sh`. Additional
week to promote to Rust with richer artifact handling (tar
logs per step, structured bisect state in a JSON file, web
UI over it).

## Dependencies

- Boot-matrix + Q1 + perf-compare scripts (all landed).
- C-10 v2 console backend (optional — lets the check script
  exchange bytes without hostfs, which is cleaner for
  profiles that disable hostfs).

## Cross-references

- `Documentation/virt/uml/redesign/scripts/uml-boot-matrix.sh`
  — the build+boot primitive.
- `Documentation/virt/uml/redesign/scripts/uml-quality-q1.sh`
  — optional per-step Q1 gate.
- `09-uml-diff.md` — normalized-dmesg diff used by the
  output-diff check mode.
- `06-uml-mcp.md` — MCP server exposes this tool to LLM
  agents as the `bisect` verb.
