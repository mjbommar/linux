# Pool-member architecture — comparison and decision (2026-05-21)

**Sprint:** post-2026-05-19
**Author:** 2026-05-21 evening session
**Status:** comparison + recommendation; no code in this commit
**Prereq:** read `09-fork-server-EXTERNAL-RESEARCH.md` (2026-05-20)
first — that memo enumerated CRIU / gVisor / AFL++ / Firecracker /
QEMU / LKML; this memo extends it with OSDI '25 findings and
synthesises a decision against UML's specific state.

---

## 0. The problem this memo is for

The current Phase 2a fork-on-resume implementation in
`arch/um/kernel/template_pause.c` produces **fork-and-exit** pool
members: M-fork child runs briefly on a private MAP_PRIVATE stack
and immediately calls `__NR_exit_group(0)`.  Three observable
consequences (the user's hospital-scenario audit, 2026-05-21):

1. **Pool members can't reach the network.**  `Phase 2`
   `um_template_identity_apply` runs in the **master**, not the
   child.  Each take rebinds the master's TAP / IPv4 / mconsole;
   children inherit via CoW and immediately die.  There is no
   long-lived "pool member" with its own host TAP — just a series
   of forks from a master whose identity changes per take.
2. **The syzkaller shim's `umlctl exec` returns ok=false.**  No
   per-member mconsole socket exists because there's no per-
   member kernel to host it.
3. **The 24h soak hasn't completed naturally** on the post-2026-
   05-21 kernel; that's a wall-clock follow-up.

The original Memo 09 design
(`09-fork-server-snapshot-restore.md` §2) **wanted** long-lived
pool members:

> "Boots ONE master UML with `um_template_pause=1`.  Master runs
> all the Umlfile's `[init.phases]` to the `READY` marker ...
> Supervisor sees the SIGSTOP via `waitpid(WUNTRACED)`, then
> `fork(2)`s the master N times.  Each child is a CoW duplicate
> at the SIGSTOP point.  Children inherit the SIGSTOPped state
> from their parent.  They sit ready until a `pool take` arrives."

Phase 2a deviated from that vision because of the **v1 ceiling**
documented in `arch/um/kernel/snapshot.c` lines 366-396 and
`09-fork-server-EXTERNAL-RESEARCH.md` §1.7: returning up the
syscall stack from `um_template_pause_enter` in the forked child
trips a longjmp-into-stale-jmp_buf hazard.  Phase 2a worked
around it by NOT returning — the child exits on its private
stack.  That's a correct fix for *fork stress*; it just doesn't
implement the *pool* design.

This memo compares four architectural approaches that could
implement long-lived members and recommends one.

---

## 1. Approaches surveyed

### 1.1  In-tree existing path: Phase 1a/1b single-shot (cold boot per member)

**What it is.**  Each `umlctl pool spawn` boots a FRESH master.
Master IS the pool member.  Single-shot.  Verified working:
`pool-spawn-smoke` passes spawn → list → destroy lifecycle today.

**Cost.**  Each member pays full cold-boot latency.  STATUS.md
quotes ~207 ms cold-boot.  Per the Memo 09 §1 reference:
syzkaller's `Pool.Create` target is 5 ms median / 50 ms p99 to
beat QEMU's 5 s.  Phase 1a/1b at 207 ms misses both.

**Concurrency.**  Linear in member count.  100 members = 20 s
total cold-boot wall-time on the published baseline.  Hospital
deployment with hot-failover requirements can't tolerate that.

**What it's good for today.**  Operator-issued single-member
spawns (`umlctl pool spawn --instance X`).  Stay.

### 1.2  Phase 2a (fork→exit, current implementation)

**What it is.**  Master at `template_pause`, fork on each take,
child does work + exits.  Matches AFL++ forkserver pattern.

**What it doesn't deliver.**  Long-lived members.  See §0.

**Why it's in tree.**  Solved the v1 ceiling for fork-stress
specifically; closes the fork→panic class of bugs.  Useful for
AFL-style fuzzing where the worker exits per case.

**Not a pool implementation.**

### 1.3  Pre-fork tree at SIGSTOP point (original Memo 09 §2 design)

**What it is.**  Boot one master, run init to `READY`, master
calls `um_template_pause_enter` which SIGSTOPs the host process.
Supervisor `fork(2)`s the master N times — each child is a CoW
duplicate at the SIGSTOP point, still SIGSTOPped.  On `pool
take`: SIGCONT one child; child's `um_template_pause_enter`
returns; child applies identity; child runs guest indefinitely.

**Why it didn't work.**  The v1 ceiling: when the child returns
from `um_template_pause_enter` up the syscall stack, the kernel
stack's saved-RIP slots have been corrupted by host signal-handler
frames pushed/popped during the fork-iteration window.  See the
EXTERNAL-RESEARCH memo §2.3-2.5 for the full root-cause walk and
§4 for the rt_sigreturn-based fix sketch.

**Path to making it work.**  EXTERNAL-RESEARCH §4 recommends:
  * **Control A**: raw `__NR_rt_sigprocmask(SIG_SETMASK, &fillset)`
    around the fork syscall, unmask only at next stable point.
    ~30 LoC.
  * **Control B**: rt_sigreturn-based re-entry in the child — never
    return from `um_template_pause_enter` "the C way," build a
    minimal rt_sigframe pointing at a known-good entry, syscall
    `__NR_rt_sigreturn`.  ~150 LoC (copy-paste from CRIU's
    `restorer.c` x86 path).

**Risk.**  rt_sigreturn-based re-entry is the canonical CRIU
mechanism — battle-tested for a decade — but porting it to UML's
specific kernel-stack model needs care.  Memo 09 §3 originally
budgeted this work as Phase 2.

### 1.4  Snapshot + exec into fresh process (Firecracker / QEMU / SEUSS)

**What it is.**  Master serializes state.  Per take, a fresh
process is exec'd that reads the snapshot from a memfd / file and
reconstructs registers + memory.

**State today.**  #181 ELF64-core export landed (commit
`ee244842a5da`).  We have an ELF writer.  We don't yet have an
ELF *reader* on the kernel-side that reconstructs from a core
file at boot.

**Comparison to Firecracker.**  Firecracker uses `MAP_PRIVATE`
snapshot files for on-demand page loading.  AWS Lambda SnapStart
reports p50 3.2 ms / p99 8.7 ms restore (vs 110 ms / 340 ms cold
boot) — meets the Memo 09 §1 target.

**Comparison to SEUSS** (BU + Red Hat Research, EuroSys '20).
Unikernel snapshot stacks, tree-structured deltas: each child
snapshot stores only dirty pages relative to parent.  Aggressive
page-level dedup.  Three orders of magnitude improvement in
function start times.

**Cost for UML.**  Need (a) kernel-side ELF64 core reader, (b)
exec-of-self path that early-exits the normal boot before
populating physmem and instead loads from the core file, (c) a
`umlctl pool serve` daemon flow that creates the core file once
and reuses it across takes.  ~400-700 LoC + careful boot-ordering
work.

**Pro.**  No fork-from-mid-syscall hazards at all.  Sidesteps the
v1 ceiling entirely.

**Con.**  Each take pays kernel-cold-init cost (PCI scan, memory
zone init, etc.) unless we explicitly skip those by jumping to a
post-init RIP.  This is the same trade Firecracker made when they
chose serialize+exec.

### 1.5  Tree-structured forks from common seed (OSDI '25 AFaaS / SEUSS)

**What it is.**  Multi-level fork tree.  Seeds are checkpoints at
various stages of initialization (e.g., "language runtime
loaded", "function code imported", "first request served").  A
new instance forks from the closest matching seed, applies the
delta, runs.

**Performance.**  AFaaS reports 1.8-8.14× faster cold start than
prior work, achieving < 15 ms at 24× concurrency.  Production-
deployed at Ant Group for 18+ months.

**Cost for UML.**  Requires (a) snapshot-with-fork semantics so a
seed can produce child seeds, (b) generation-tracking so the
daemon knows which seed to fork from, (c) divergence handling so
a child's identity changes don't bleed into the seed.

**Pro.**  Fastest demonstrated approach.

**Con.**  Big engineering surface.  Multi-week.  Premature for
where UML is today — we should establish single-level fork
correctness before multi-level optimization.

### 1.6  Persistent-mode adaptive forkserver (AFL++ AugPersist, 2024)

**What it is.**  AFL++ persistent-mode variant where the
forkserver position is adjusted to *after* expensive
initialization completes — not at program entry.  Self-adaptive:
the forkserver dynamically picks a persistent-basic-block point
where forking is safe and post-init.

**Relevance to UML.**  AFL's own ready-point invariant
(`um_snapshot_assert_ready` at `arch/um/kernel/snapshot.c:142-198`)
is essentially this idea: assert preconditions before forking.
Phase 2a's `assert_fork_safety` skips four of those preconditions
(see EXTERNAL-RESEARCH §1.1 + §2.1).

**Path to use.**  Adopt the four AFL preconditions verbatim into
`assert_fork_safety` — see EXTERNAL-RESEARCH §5 recommendation.
This complements (1.3), it doesn't replace it.

### 1.7  rt_sigreturn-based clean re-entry (CRIU pattern)

**What it is.**  The fundamental mechanism that underlies (1.3)
working correctly.  In the forked child, instead of returning up
the C call chain (where saved RIPs may be corrupted), build a
fake signal frame pointing at a known-good entry function, then
`syscall(__NR_rt_sigreturn)`.  The host kernel restores ALL
registers atomically from the frame.  No `ret`, no `call` chain
unwind.

**Source of truth.**  CRIU's `criu/pie/restorer.c::rst_sigreturn`
at line 1079:
<https://github.com/checkpoint-restore/criu/blob/criu-dev/criu/pie/restorer.c>.

**Why CRIU works.**  Pointed out in EXTERNAL-RESEARCH §1.1:
> "The restorer builds a struct rt_sigframe containing the saved
> CPU state, then calls rt_sigreturn with rsp pointed at that
> frame.  The host kernel restores ALL registers (including rip,
> rflags, rsp) in one atomic transition."

**Why it solves the v1 ceiling.**  The corruption hazard is
"some C frame's saved-RIP slot got overwritten by a host signal
handler's frame push."  If the child NEVER returns from C through
that slot — it instead atomically transitions to a clean entry
via the host kernel — the corruption is irrelevant.

---

## 2. Decision matrix

| Approach | Latency | Hospital-ready | LoC | Risk | Replaces fork-and-exit? |
|----------|---------|----------------|-----|------|--------------------------|
| 1.1 Cold boot per spawn (today) | 207 ms cold | weak — slow | 0 (exists) | low | no |
| 1.2 Phase 2a fork-and-exit (today) | <50 ms | no — not a pool | 0 (exists) | low | doesn't apply |
| 1.3 Pre-fork tree at SIGSTOP + (1.6) + (1.7) | ~5-50 ms est | YES | ~300 LoC | medium — rt_sigreturn port | yes |
| 1.4 Snapshot+exec (#181 reader) | 3-30 ms est | YES | ~500-700 LoC | medium-high — boot-ordering | yes |
| 1.5 Tree-structured (AFaaS) | <15 ms est | YES at scale | ~1500-2500 LoC | high — multi-week | yes |
| 1.6 AugPersist invariants | n/a — complementary | needed for any other | ~50 LoC | low | n/a |
| 1.7 rt_sigreturn re-entry | n/a — mechanism | enables (1.3) | ~150 LoC | low — proven | n/a |

---

## 3. Recommendation

**Land 1.6 + 1.7 + 1.3 in that order.  Defer 1.4 as the
fallback if 1.3 trips a hazard we don't anticipate.  Defer 1.5
entirely — premature.**

### 3.1  Why this order

The user's hospital question maps to "would I trust this code
with sick children's monitoring data."  The closest existing
production analog for the latency target is Firecracker's
SnapStart (1.4 / 3.2 ms p50).  None of the surveyed approaches
have been deployed at hospital scale specifically; AFaaS is the
closest to deployed-at-real-scale (18 months production at Ant
Group).

For where UML is **today** — Phase 2a's fork-stress is solid;
template_pause works at single-take granularity; the remaining
gap is "make the child stay alive and addressable" — the smallest
change is 1.3 (the original Memo 09 §2 design) with the v1
ceiling closed via 1.7.

  * **1.6 first**: porting the four AFL preconditions into
    `assert_fork_safety` is ~50 LoC + verification.  Reduces the
    blast radius of the v1 ceiling by guaranteeing the
    pre-conditions the AFL path relies on.  Without these the
    next steps inherit the same corruption hazard.

  * **1.7 second**: implement `criu_rt_sigreturn_to(entry_fn,
    stack)` as a host-side primitive in
    `arch/um/os-Linux/template_pause.c`.  ~150 LoC.  Test by
    having a small test program call it from inside an arbitrary
    syscall context.

  * **1.3 third**: rewrite `fork_on_resume_loop` to pre-fork N
    children at the SIGSTOP point (matching Memo 09 §2's design).
    Each child's post-fork path uses the 1.7 primitive to atomic-
    transition into a `pool_member_main()` function, bypassing
    the C return-from-`um_template_pause_enter`.
    `pool_member_main` applies identity (the existing
    `um_template_identity_apply` works as-is) and enters the
    guest's main loop.  Daemon-side: `pool take` SIGCONTs the
    selected child rather than fork-on-each-take.

### 3.2  What this gives us

Once 1.3 lands:

  * **Per-member network**: each child applies its own TAP via
    the existing 374ab47804ed Phase 2.2 code, which already
    issues TUNSETIFF in the child path.
  * **Per-member mconsole**: each child applies its own socket
    via the existing b568f87a2b5b code, which is currently
    misdirected to the master.  Once the child runs identity
    apply, the rebind lands on the child's kernel state.
  * **Single host process per pool**: master + N pre-forked
    children, no per-take exec.
  * **Take latency = SIGCONT + identity-apply RTT** ≈ p50 5 ms,
    p99 50 ms (matches Memo 09 §1.4 target).
  * **The double-counting / dev_id / TAP fixes already in tree
    become useful**: today they apply to the master; under (1.3)
    they apply to the child where they were designed to.

### 3.3  What this doesn't fix

  * **Replenish under crash storm**: if all N children die in 1
    s, the supervisor needs to re-fork from the master.  Means
    master must NEVER return from its `um_template_pause_enter`
    — it must sit in a blocking host primitive forever (as
    EXTERNAL-RESEARCH §1.7 quotes from `snapshot.c`).  Adds a
    small dispatch loop in the master.

  * **Hospital deployment with > 100 simultaneous members per
    host**: 1.5 (tree-structured) is the proven path; defer
    until operator pressure.

  * **Snapshot-to-disk for migration / restart**: that's #181 (we
    have the ELF writer) + a future ELF reader.  Tracked
    separately.

### 3.4  Risk: what if 1.7 trips a new hazard?

`rt_sigreturn` from inside a UML kernel-half code path has not
been done before (CRIU does it from a userspace restorer blob,
not from inside what UML treats as kernel code).  The host kernel
treats `rt_sigreturn` the same regardless — but the UML kernel's
own bookkeeping (preempt_count, IRQ tracking, signals_enabled
flag) may not be coherent after the atomic transition.

Mitigation: write a small test program that exercises the
`rt_sigreturn` primitive against an arbitrary call stack and
verifies the post-jump invariants (preempt_count == expected,
signals_enabled == 1).  This is ~50 LoC of selftest.  Run before
attempting (1.3).

Fallback if 1.7 doesn't work: pivot to (1.4) snapshot+exec.  We
already have the ELF writer; adding the reader is bounded work.

---

## 4. Concrete next actions

1. Write the rt_sigreturn primitive selftest (~50 LoC).  Verify
   it round-trips a clean call/return on a healthy stack.
2. Port the four AFL preconditions into `assert_fork_safety`
   (~50 LoC + tests against `template-pause-fork-stress`).
3. Implement `criu_rt_sigreturn_to(entry_fn, stack)` in
   os-Linux/template_pause.c (~150 LoC).  Smoke against (1).
4. Rewrite `fork_on_resume_loop` for pre-fork+SIGSTOP-pool model
   (~150 LoC).
5. Wire daemon to `pool take = SIGCONT child` model in
   pool_serve.rs (~100 LoC).
6. Update `pool-bench` to measure per-member RSS against the new
   model (the gate-2 fix from `493e53bceab2` should now show
   real numbers, not "0 live children").
7. Run pool-exec-smoke; verify Case A (ok=true with real output)
   instead of Case B.
8. Run pool-bench end-to-end ping from host to pool member IPv4.

**Total budget:** 4-7 days of focused work, depending on how
much the rt_sigreturn pivot takes.

---

## 5. Things I learned researching this that should land in
   memos

  * **No major project forks its running self.**  Firecracker,
    QEMU, gVisor, Wasmtime, Cloud Hypervisor — all use serialize+
    exec.  AFL++ is the closest to fork-of-running-process but
    forks at constructor time (pre-init), not mid-execution.  We
    chose harder ground than the industry consensus.
  * **OSDI '25 "Fork in the Road" / AFaaS** is the most recent
    production data point — tree-structured fork seeds, < 15 ms
    cold start at 24× concurrency, 18 months production.  Worth
    a deep read before any 1.5 work.
  * **SEUSS (EuroSys '20)** validated the unikernel-snapshot-
    tree concept with three-orders-of-magnitude improvements.
    Architecturally close enough to UML that the design ideas
    transfer cleanly.
  * **CRIU's rt_sigreturn pattern is 10+ years old and rock
    solid.**  Reusing it as the v1-ceiling escape hatch is not
    novel research, it's just engineering work nobody's done
    for UML.
  * **AFL++ AugPersist (2024)** confirms the "self-adaptive
    forkserver position" idea is mainstream — supports the (1.6)
    direction.

---

## 6. Sources (sourced via web search 2026-05-21)

  * CRIU restorer: <https://github.com/checkpoint-restore/criu/blob/criu-dev/criu/pie/restorer.c>
  * OSDI '25 AFaaS: <https://www.usenix.org/conference/osdi25/presentation/chai-xiaohu>
  * SEUSS (EuroSys '20): <https://wangziqi2013.github.io/paper/2021/09/08/seuss.html>
  * Nyx libnyx: <https://github.com/nyx-fuzz/libnyx>
  * AFL++ AugPersist (ScienceDirect 2024): "AugPersist: Automatically augmenting the persistence of coverage-based greybox fuzzing"
  * Firecracker snapshot: <https://github.com/firecracker-microvm/firecracker/blob/main/docs/snapshotting/snapshot-support.md>
  * gVisor ptrace platform: <https://github.com/google/gvisor/blob/master/pkg/sentry/platform/ptrace/subprocess.go>
  * syzkaller vmimpl: <https://pkg.go.dev/github.com/google/syzkaller/vm/vmimpl>

Internal cross-references:
  * `09-fork-server-EXTERNAL-RESEARCH.md` — 746-line prior survey
  * `09-fork-server-snapshot-restore.md` — original Memo 09 design
  * `09-fork-server-STATUS.md` — Phase 2a v1-ceiling failure
    analysis
  * `arch/um/kernel/snapshot.c` lines 366-396 — documented
    in-tree v1 ceiling
