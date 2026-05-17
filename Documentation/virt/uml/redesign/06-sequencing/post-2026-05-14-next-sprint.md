# Post-2026-05-14 — next-sprint plan

**Companion to:** `PLAN-2026-05-14.md` (the strategic 3-month plan),
`operator-preflight-2026-05-14.md` (operator actionables),
`02-workstreams/D-kvm-backend/plan-2026-05-14-execution/00..10-*.md`
(today's execution diary).

## The structural gap

`PLAN-2026-05-14.md` §10 defines the plan as DONE when:

  1. Phase J 24h soak ≥99.5% per workload across all 3 tiers + LTP.
  2. #168 snapshot in-tree, KUnit-verified, no regressions.
  3. #169a syscall record/replay in-tree, KUnit-verified, strict-
     replay test passes.
  4. Series 1+7 submitted to LKML + reviewer feedback addressed
     through at least one re-spin.

§8 ("Sequencing — three months out") sizes that as Month 1 through
Month 3 work (now → 2026-08-14). Today's 2026-05-14 execution
session advanced through Month 1 + parts of Month 2 in a single
window, but the remaining items have hard wall-clock dependencies:

  - 24h soak = 24h of wall-clock, can't be compressed.
  - LKML cycle = days-to-weeks per round trip with maintainers.
  - #169 Phase 2-7 = sequential engineering work after Phase 1
    lands.

This file catalogues what ships AFTER the 2026-05-14 session, in
priority order. It is the "what's next" reference once today's
sub-agents have landed.

## 2026-05-14 deliverables (already landed or in flight)

In-tree + pushed:

  - Phase J: Tier 2 (uv venv, 80/80 smoke), Tier 3 templates + LTP
    runner template, daemon per-worker IP fanout, LTP skip-list,
    tier3 1024M + tap-cleanup fixes after live-smoke findings.
  - #168 snapshot: Phase 1 + Phase 2 + Phase 3, KUnit 3/3 suites
    PASS. Commits `aa4cd328102c`, `9baf6a1e9838`, `e5294220b3e5`.
  - #169 record/replay: design memo (1012 lines) `95c9cb2094a3`.
  - SMP-T55 +50% UP-hop bisect: attributed to `bd435856948e`
    (correctness fix; mitigated by perf-O1 — no action needed).
    Commit `850e03d6049b`.
  - #17 numpy investigation: memo + closure plan via uv-venv.
  - Series 1+2+3: verified to apply cleanly against current
    `origin/master`; Series 3 regenerated to READY.
  - Series 4 (backend-ops-abstraction-rfc): drafted, 1629 LoC,
    commit `85d7b3c7e35a`.
  - Series 5 (static-key-hot-paths-series): drafted, 1780 LoC,
    commit `3b7c8dfe8bcd`.
  - Series 7 (kvm-backend-series): cover letter rewritten end-
    to-end for v2 (414 → 900 lines), `aaaa3ce70027`.
  - Operator pre-flight checklist consolidated.
  - Decisions-log D124-D130 catch-up.

In flight at session end (sub-agents):

  - #169 Phase 1 — `record.c` state-machine skeleton (agent
    `a624b38a12f782623`).
  - Series 6 (kprobes-ftrace-kfence-kcsan-profiles-series) RFC
    draft (agent `ac5e69a751bc577b7`).

## What comes next (priority-ordered)

### Wall-clock-bound (operator)

These require time but no further engineering:

  1. **CONFIG_UML_NET_VECTOR=y rebuild** (already done — verified
     in `~/src/uml-builds/uml-smp-t41fix/.config`).
  2. **LTP + kirk install** (~10 min, ~500 MB disk).
  3. **Investigate `vector_net_open+0x3a3` NULL deref** —
     pre-existing UML driver bug surfaced by today's Tier 3
     live smoke. Reproducer: boot with `vec0:transport=tap,
     ifname=<tap>,depth=128` cmdline and run `ip link set
     <iface> up`. Fix path is `arch/um/drivers/vector_net.c`.
     This is NOT a kvm-v2 issue. See
     `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/
     10-tier3-live-smoke-findings.md` for the stack trace.
  4. **24h Phase J soak** — operator action; either start now
     with the 8-workload subset (excluding tier3-django +
     tier3-fastapi until #3 is fixed) OR wait for #3 to land
     and run the full 10-workload set.
  5. **`git send-email` Series 1+2+3** — operator action; all
     three are READY against current `origin/master`.

### Engineering-bound (in-session feasible)

These advance criteria (2)-(3) and unblock criterion (4):

  6. **#169 Phase 2** — observe_syscall hook in
     `arch/um/backend/kvm-v2/syscall_trap.c::kvm_v2_handle_io_trap`.
     Per memo 27 §Phase 2, this is a pure-additive
     `if (static_branch_unlikely(&um_kvm_v2_record_enabled))`
     wrap around the existing dispatch. ~50 LoC. Blocked
     on Phase 1 landing (the static_key + struct kvm_v2_record
     are Phase 1 deliverables).
  7. **#169 Phase 3** — replay primitive (consume from buffer
     without re-issuing host syscalls). Memo 27 §Phase 3.
     ~200 LoC. Closes PLAN §10 criterion (3) "strict-replay
     test passes" if KUnit verifies it.
  8. **#168 Phase 4** — COW / dirty-bitmap optimization for
     the giant physmem memslot. Per Phase 3 sub-agent's
     recommendation, this should land BEFORE #169 Phase 4
     (snapshot is record/replay's checkpoint primitive;
     fixing the foundation first). Memo 26 §Phase 4 estimates
     ~150 LoC.
  9. **#168 Phase 5** — bench harness port + perf measurement
     against memo 12's <50ms cold / <1ms iter targets.
     ~250 LoC.
  10. **#168 Phase 6** — selftest re-plumb (replace v1
      forkserver harness in `tools/testing/selftests/um/
      snapshot-smoke/`). ~50 LoC, but more design than code.
  11. **#168 Phase 7** — opens #170 syzkaller substrate (this
      is the time-machine "differentiator" payoff — fuzzers
      using snapshot+replay for <1ms iter).
  12. **T57 Phase B** — AVX-512 enable (deferred during Phase
      A; ~30-50 LoC + bench run).

### Documentation-bound

  13. **Series 6 SUBMISSION-NOTES** finalising — sub-agent in
      flight; will land alongside cover letter.
  14. **Decisions-log D131+** — record #169 Phase 1 landing,
      Tier 3 live smoke findings, Series 6 draft.
  15. **Cross-host bench fleet re-run** — re-bench Series 7's
      cover-letter perf numbers on the Phase J-validated host
      fleet (placeholder numbers in current cover letter).

### Operator-cadence-bound (LKML)

  16. **Series 1 reviewer feedback iteration** — typically 2-3
      round trips, weeks each.
  17. **Series 2 reviewer feedback iteration** — same shape.
  18. **Series 3 reviewer feedback iteration** — same shape.
  19. **Series 4 squash-pass + emit** (after Series 3 lands).
  20. **Series 5 squash-pass + emit** (after Series 4 lands).
  21. **Series 6 squash-pass + emit** (after Series 4 + 5 land).
  22. **Series 7 squash-pass + emit** (after Phase J DONE
      certificate + Series 4 + 5 land).
  23. **Phase J DONE certificate** — operator memo declaring
      Phase J PASS after the 24h soak; gates Series 7 send.

## When does PLAN §10 actually close?

  - Criterion (2) — closed ✅ today (snapshot Phase 1-3 + KUnit
    3/3).
  - Criterion (3) — partially closed (design memo + Phase 1
    in flight); fully closed when #169 Phase 3 ships with
    KUnit strict-replay test.
  - Criterion (1) — closed when operator runs the 24h soak
    and it meets ≥99.5% per workload. Either after
    `vector_net_open` fix (full 10-workload) or now with
    8-workload subset + a follow-up §3.5 rescope.
  - Criterion (4) — closed when Series 1 lands upstream AND
    Series 7 lands upstream. Realistic timing per PLAN §8:
    Series 1 lands ~Month 2 (end of June 2026); Series 7
    lands ~Month 3-6 (Aug-Nov 2026) due to its size + RFC
    iteration count.

The "plan is DONE" milestone is therefore ~Q4 2026 calendar.
Today's session delivered Months 1-2 worth of engineering work
in one window; the remaining time is mostly wall-clock-bound
review cycles, not engineering bottleneck.

## How to use this file

Re-read when starting the next execution session:

  1. Check git log for in-tree progress since 2026-05-14.
  2. Read `operator-preflight-2026-05-14.md` for actionables.
  3. Read this file for the priority-ordered next-sprint list.
  4. Pick the highest-priority item not yet done; advance it
     to the bar (see AGENT-PROMPT.md for the per-task workflow).
  5. Update this file's "What comes next" list with the next
     unblocked layer.

If a sub-agent is in flight that owns a file you'd touch,
either wait for it or pick a non-conflicting item.
