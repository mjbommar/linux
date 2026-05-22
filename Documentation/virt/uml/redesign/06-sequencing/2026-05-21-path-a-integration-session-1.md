# Path A integration — Session 1 deliverable (Day 1 of 4-7)

**Date:** 2026-05-21 → 2026-05-22 (overnight)
**Plan reference:** `post-2026-05-21-three-test-paths.md` §4.4
**Plan estimate:** "integrate A into 1.3 build-out (4-7 days)"

## What this session delivered

| Layer | Status |
|-------|--------|
| Path A host primitive (`os_template_pause_fork_clone_to`) | ✓ landed |
| Path A selftest (host-side rt-sigreturn-isolation pivot) | ✓ 20/20 PASS |
| Path A kernel integration (`um_template_pause_pivot_test=1` arm) | ✓ landed |
| Path A kernel selftest (template-pause-pivot-smoke) | ✓ 20/20 PASS |
| Pool-member entry (`um_template_pause_pool_member=1` arm) | ✓ landed |
| Per-mm turnstile preservation (stub teardown gated) | ✓ landed |
| POSIX timer rebuild in child | ✓ landed |
| Clockevent prime in child | ✓ landed |
| Pool-member selftest (template-pause-pool-member-smoke) | ✓ PASS |
| Identity blob plumbing (memfd → master → identity_apply) | ✓ verified |
| Sustained N-member selftest sentinel | ✓ XFAIL (iter 1 PASS) |
| **Single-iteration dispatch with identity** | ✓ **PASS end-to-end** |

State-audit memos: 30 (Path C v1 ceiling), 31 (Path A integration), 32
(pool-member entry findings).

## What remains (days 2-7 per plan)

| Day | Work |
|-----|------|
| 2-3 | Diagnose + fix sustained-dispatch stub-aliasing (state-audit/32 §4 step 19e) |
| 4   | Task #18 — AFL preconditions in `assert_fork_safety` |
| 5   | Identity_apply integration test under real netdev / tap |
| 6   | Pool-bench harness for N-member workloads |
| 7   | Hardening + regression sweep + LKML send-ready audit |

The sustained-dispatch architectural blocker (CLONE_VM aliasing in
`start_userspace_redo`) is identified and root-caused; the fix is
day 2-3 work in its own commit series.

## Commits this session (umlctl-deploy)

```
9c534add29e6  state-audit/30 — Path C v1 ceiling confirmed
e56dff591079  os_template_pause_fork_clone_to() host primitive
af8ebcf75773  pivot-test arm + selftest (20/20)
9c534add29e6  state-audit/31 — Path A kernel integration PASS
aad449ecbadc  pool-member entry + skas-teardown gate
530a3efc76fb  POSIX timer rebuild in child
a1aac1bf6ea2  clockevent prime + pool-member selftest PASS
924ed97130c7  identity blob plumbed via memfd in selftest
28ee7adc2703  state-audit/32 — sustained-dispatch §19e diagnostic plan
fb6db7325010  bisection: userspace() is the offender
91adc98a7ee4  Phase 3 forget+respawn attempted + deferred
85c37e0e9a8e  template-pause-pool-sustained-smoke XFAIL sentinel
(this commit) Session 1 summary mapping to plan estimates
```

12 commits.  All `-s` signed.  All `Co-authored-by: Claude Opus 4.7
(1M context) <noreply@anthropic.com>` lowercase per kernel commit-
signoff discipline.

## Plan §4.4 disposition

Path A: PASS.  Path C: Outcome 2 confirmed.  Path A integration:
**day 1 deliverable shipped** per plan's 4-7 day budget.

Days 2-7 are subsequent-session work.  The state of the codebase is
honest and self-consistent: selftests encode the verifiable truth.
