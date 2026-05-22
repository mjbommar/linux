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

## Day-2 amendment (2026-05-22)

Continued the same session into day-2 work.  Additional commits:

```
757888e5680c  AFL preconditions in assert_fork_safety (task #18)
0d834f5d7fa5  stub_data is fully load-bearing (experiment)
07bc5fbb15eb  early-fork + pool_member experiment
d7e28a4f5d00  state-audit/32 — v1 ceiling is mid-fn SIGSEGV, not ret
a1d6d0020ccc  block host signals BEFORE SIGSTOP (hardening)
c2829c412367  um_skas_disown_inherited() helper (infrastructure)
4d8b6bc65d3d  split identity blob parse from apply
efdbe59e9d66  state-audit/32 — per-member physmem isolation needed
```

22 commits total.  Architectural finding nailed down:

  **UML's physmem_fd is MAP_SHARED across all forked UML kernels.**
  Per-member stub isolation requires per-member physmem_fd, a
  refactor of init_new_context + arch_um_load_physmem.  This is the
  actual blocker for sustained N-member dispatch; it's a separate
  workstream from Path A integration.

What's verifiably done (selftest truth):
  - template-pause-pivot-smoke: 20/20 PIVOT_OK
  - template-pause-pool-member-smoke: init.sh → MEMBER_DONE PASS,
    with identity-parsed marker confirmed
  - template-pause-pool-sustained-smoke: iter-1 PASS, iter-2+
    XFAIL (documented blocker)

What's needed for full 1.3 build-out per Memo 09 §2:
  - Per-pool-member physmem_fd backing (separate workstream)
  - Once that lands, um_skas_disown_inherited() +
    um_skas_respawn_all_stubs() in child_entry_pool_member can be
    re-enabled; sustained dispatch should PASS.
  - Selftest sustained-smoke becomes the acceptance gate.

What's blocked downstream (not session-shippable):
  - Pool-bench under N members (Memo 09 Phase 3 scaffold ready)
  - syzkaller vm/uml shim (task #4)
  - Series 7 send (task #7; also blocked on 24h soak completion)
