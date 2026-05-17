# UML Redesign — Status Tracker

Last updated: 2026-05-07 (**STATUS drift cleanup + experimental verification pass.** Verified on AMD Ryzen 7 7840HS (Zen 4) against post-T54/T56/gadget kernel rebuilt to current HEAD: substrate gate kvm-v2 **PASS=25/FAIL=3/XFAIL=3** (seccomp identical), cpython-parity **21/21 PARITY**, mt-mini SMP T=8 ncpus=4 N=30 **30/30 = 100%**, bench-py **kvm-v2 4.00× faster than seccomp** (matches 2026-05-04 cross-host baseline), bench-micro getpid **kvm-v2 ~1050× faster** (89-105 cyc / 109k cyc seccomp), T57 stress-ng `--vm --verify` reproduces SIGILL on kvm-v2 only. **One unmasked finding: `perf-py-startup` gate FAILS** (ratio_v2_over_seccomp = 1.250 vs max=1.2 on Python startup workload) — surfaced as **SMP-T55** in this update; root cause is T26/T27 always-`KVM_GET_FPU` reverting H.2 lazy-FPU. Hot-path workloads unaffected. Drift fixes: T33-as-mt-mini-closer marked HISTORICAL (T41 was true closure); migrate_disable site count corrected (3 not 5); phase ledger extended with T33/T36/T37/T41/T47/T54/T56 + gadget revival + perf-O1 rows. Prior `2026-05-05` Phase J pilot context preserved at `02-workstreams/D-kvm-backend/phase-J-pilot-2026-05-05.md`; SMP-T55 fix plan at memo state-audit/23.)

This document is the single source of truth for "where are we, what's
broken, what's next." Updated whenever priorities or blockers change.

If something contradicts a memo in `02-workstreams/` or
`04-risks/decisions-log.md`, this file wins until the underlying memo
catches up.

**Tip:** `umlctl-deploy` HEAD (Phase J pilot soak rig + perf-getpid
parser fix). Functional gates clean post-gadget + post-T54/T56/T57-
disabled; one perf gate now failing (SMP-T55, see below):

  - **mt-mini SMP T=8 ncpus=4 N=400: 400/400 = 100.0% PASS** (post-T54
    SOCK_CLOEXEC fix + post-T56 EINTR-mid-gadget fix; Wilson 95%
    [99.0%, 100.0%]). The init.sh-hang class (SMP-T54) is closed.
    Re-verified post-J-pilot at N=30 → 30/30.
  - **Phase J pilot soak (3 short workloads × 2 backends × N=40 each):
    240/240 = 100.0%** (memcheck/iocheck/stress-ng — see Last updated
    line above for breakdown).
  - substrate gate kvm-v2: PASS=25/FAIL=3/XFAIL=3 (matches seccomp)
  - cpython-tier0 kvm-v2: PASS
  - cpython-parity 21/21 PARITY (UP and SMP)
  - **bench-py kvm-v2 (cross-host, post-gadget, 8 hosts): 2.99–4.06× faster than seccomp** across Skylake/Kaby/Alder Lake/Zen 4. Verified on Zen 4 7840HS today: 4.00× (kvm-v2 125.13 ms / seccomp 501.20 ms), absolute ms unchanged from 2026-05-04 baseline.
  - bench-micro getpid (post-gadget, 8 hosts): **193–908×** speedup (gadget collapses ~36 800 cyc roundtrip to ~90 cyc in-guest). Verified today: kvm-v2 89–105 cyc / seccomp 109 532 cyc → ~1050× (Zen 4 7840HS).
  - threaded-fork-malloc 0/116000 forks failed (T29)
  - threaded-subprocess-wait 10/10 (T29)
  - userspace ABI test (RDX/R8/R10 across 11 gadget syscalls): 11/11
    preserved (post-gadget item #1+#4 fix, commit `7ebcd8aac347`)
  - **`perf-py-startup` gate: PASS** (ratio_v2_over_seccomp = 1.10 vs max=1.2 on Zen 4 7840HS post-`fd2f9639b0ce`). Was 1.250 FAIL pre-fix. SMP-T55 closed at gate-ceiling level via per-vCPU FPU-dirty epoch flag (memo state-audit/23 option a) — restores Phase H.2 lazy-FPU win while preserving T26/T27 cross-task XMM guarantee (24 000 forks across 6 boots, 0 CHILD_FAIL). Stretch target (ratio ≤ 0.55) deferred to a follow-up bisect of the +50% UP-hop in 04-30→05-02 SMP-correctness commits.

**Prior closes superseded** (T33's prev_roots-cache fix was a partial
fix — it changed dispatch timing and incidentally reduced the
EINTR-mid-stub frequency by ~+15pp, but the actual mechanism was
not what T33 thought; T41's recovery of user RAX is the true root
cause).

---

## TL;DR — what works

Headline: KVM v2 backend has cleared **Phases A–H + G.1 + G.2 (now
fully active) + I (lift)**. SMP works. Substrate parity works. The
old umbrella P0 (#274 — Python is table stakes) is **functionally
closed**: cpython-parity reads 21/21 under both UP and SMP. All
remaining work is Phase J validation, post-Phase-G polish, paused-
functionality resumption (snapshot/record-replay/syzkaller), and the
LKML upstream queue.

| Workload                                          | Backend | UP        | SMP (--with-cpus=4) |
|---------------------------------------------------|---------|-----------|---------------------|
| `init=/bin/echo`                                  | kvm-v2  | yes       | yes                 |
| `python3 -V` / hashlib / ssl / single-import      | kvm-v2  | yes       | yes                 |
| `python3 -c "import unittest"`                    | kvm-v2  | yes       | yes                 |
| Substrate gate (regrtest substrate, 25+3+3)       | kvm-v2  | PASS=25   | PASS=25             |
| cpython-parity gate (21 stdlib modules)           | kvm-v2  | 21/21     | 21/21               |
| mt-mini × 30 (T=8, ncpus=4)                       | kvm-v2  | n/a       | 100%                |
| mt-mini × 30 (T=4, ncpus=4)                       | kvm-v2  | n/a       | 30/30 (post T22)    |
| mt-yieldonly × 15 (T=8, ncpus=4)                  | kvm-v2  | n/a       | 100%                |
| mt-rawmmap × 20 (T=8, ncpus=4)                    | kvm-v2  | n/a       | 100%                |
| mt-mmap-stress (T=8, ncpus=4)                     | kvm-v2  | n/a       | PASS (post T22)     |
| threaded-subprocess-wait × 20 (T22)               | kvm-v2  | n/a       | 17-18/20 (NM_stub+2=0) |
| **threaded-subprocess-wait × 10 (post T29)**      | kvm-v2  | n/a       | **10/10, 0/4000 fails (100%)** |
| **threaded-fork-malloc × 6 (post T26/T27)**       | kvm-v2  | n/a       | **0/24000 fails (100%)** |
| **threaded-fork-malloc × 30 LONG SOAK (post T29)**| kvm-v2  | n/a       | **29/30 boots, 0/116000 forks (TRUE 0%)** (1 RCU-stall outlier, no CHILD_FAIL) |
| **substrate gate (post T29)**                     | kvm-v2  | n/a       | **PASS=25/FAIL=3/XFAIL=3 (matches seccomp)** |
| **cpython-parity gate (post T29, 21 modules × 10)**| kvm-v2 | n/a       | **210/210 PARITY across 10 boots** |
| **wide cpython-parity (post T29, 135 curated modules)** | kvm-v2 | n/a   | **134 PARITY / 0 REGRESSION / 1 KVM_BETTER / 5 BOTH_FAIL** ([log](02-workstreams/D-kvm-backend/wide-parity/2026-05-03-wide-parity-135-modules.log)) |
| `make -j4` inside guest (Phase G.3)               | kvm-v2  | n/a       | yes                 |
| All workloads under `backend=force=seccomp`       | seccomp | yes       | yes (deterministic) |

**Two builds tracked:** `~/src/uml-builds/uml-clean` (UP),
`~/src/uml-builds/uml-smp` (SMP, `--with-cpus=4`). Per the
"don't put kernel build dirs in /tmp" discipline rule (~700 MB each
on `/`, never tmpfs).

---

## v2 phase ledger

| Phase | Title                                  | State    | Anchor commits / memos |
|-------|----------------------------------------|----------|------------------------|
| A     | init.c + per-VM context + vcpu0        | DONE     | memo 26 §A             |
| B     | host-VA / kernel-VA Kconfig split      | DONE     | memo 25 R1             |
| C     | per-CPU vCPU pool                      | DONE     | memo 26 §C             |
| D.0–D.5 | trampoline, dispatch, return, MSRs, PML4[448] | DONE | memo 26 §D, tasks #59–#65, #72–#88 |
| E     | exception handling (IDT/TSS/IST)       | DONE     | memo 26 §E, tasks #81–#96 |
| F     | signal/preemption                      | DONE     | memo 26 §F, task #67   |
| G.1   | SMP — per-CPU IDT/IST/TSS install      | DONE     | memo §G.1, task #140   |
| G.2   | cross-vCPU TLB kick (A+B+C)            | DONE     | A+B `7e1c255a09ad`/`9f0ff6257e8b`; C activated `ad18db7c3768` after T13 migrate_disable fix |
| G.3   | SMP validation — `make -j4` in guest   | DONE     | task #142              |
| H.1b  | per-task FPU snapshot (mmu_gather UAF) | DONE     | task #119              |
| H.1b  | #121 cr2+IDT EINTR preservation        | DONE     | `ba7aa0aa0240` + `e5977806fd14` (inline PF handler) |
| H.2   | CR0.TS lazy FPU                        | DONE     | task #120              |
| H.3   | perf target met (v2 2.58× seccomp)     | DONE     | memo §H.3              |
| SMP-T16 | Bug A — cr2-preserve across same-task re-entry | DONE | `2f0c87a47dcb`, memo state-audit/09 |
| SMP-T17 | Bug B EINTR-mid-NM-stub variant       | DONE     | `85244f068ad3`, memo state-audit/10 |
| SMP-T19 | IST sanity guards (snapshot/restore/write) | DONE | `d9ed9e14c7b6`         |
| SMP-T20 | RCU-deferred mmu_gather page free      | DONE     | `d9ed9e14c7b6`         |
| SMP-T22 | Bug B class — vmexit-on-#NM, eliminate iretq surface | DONE | `ddf3cfe5cf31`, memo state-audit/11 |
| SMP-T23 | cross-mm cr2-zero (last_mm gate) | DONE | `a0be14e66013`, memo state-audit/12 |
| SMP-T25 | Bug B residual — LSTAR-EINTR HOST_IP rewind | DONE | `b1421d7583e9`, memo state-audit/13 |
| SMP-T26/T27 | FPU cross-task leak — always KVM_GET_FPU after KVM_RUN | DONE | `76b1d98b2006`, memo state-audit/15 |
| SMP-T29 | fork-snapshot clobber — gate capture_for_switch_out on `vcpu->last_task` | DONE | `44d21b5a14ab`, memo state-audit/16 |
| SMP-T33 | mt-mini residual — KVM TDP `prev_roots` cross-task drop (partial, superseded by T41) | DONE (superseded) | `9ccdc4300713`, memo state-audit/20 |
| SMP-T36 | mm/mmu_gather batch-free skip after partial deferral | DONE | `bedd73af5033` |
| SMP-T37 | per-CPU UML host pthread CPU pinning (defense-in-depth) | DONE | `1b1febc1ffba` |
| SMP-T41 | mt-mini byte[0]=0 — recover user RAX in EINTR-mid-PF-stub (true mt-mini residual closure) | DONE | `af659ad4297d`, memo state-audit/21 + 22 |
| SMP-T47 | WARN_ON_ONCE on cross-task `KVM_SET_SREGS` failure (mainstream-readiness #6) | DONE | `602e9a27625c` |
| SMP-T54 | worker socketpair fd leak — SOCK_CLOEXEC + umlctl sysrq halt | DONE | `467aa7d142c0` |
| SMP-T56 | LSTAR-EINTR carve-out extended to cover gadget body (post-revival) | DONE | `db9170b5a7b3` |
| Gadget revival | Phases 1-7 + mainstream-readiness items #1-5 (`lstar_gadget.S`, RDX/R8/R10 ABI preservation, KUnit byteshape, `CONFIG_UM_BACKEND_KVM_V2_GADGET` Kconfig) | DONE | `7ebcd8aac347` |
| perf-O1 | Gate post-syscall `interrupt_end()` on `-ERESTART` / `_TIF_WORK` (~120 cyc/syscall save) | DONE | `24557e95c4b1` |
| I.2   | KUnit suites (vCPU pool / memslot / IDT) | DONE   | task #113              |
| I.3   | docs — backend README + backends.rst   | DONE     | task #114              |
| I.4   | lift EXPERT gate from CONFIG_UM_BACKEND_KVM_V2 | DONE | task #112        |
| J-pilot | realistic-workload soak rig (memcheck/iocheck/stress-ng + cpython-soak/kbuild-tiny templates) — first pilot 240/240 = 100% on short set | DONE | `95c95267202e`, memo `02-workstreams/D-kvm-backend/phase-J-pilot-2026-05-05.md` |
| J     | validation — 24h continuous + Tier 1/2/3 + LTP | **IN PROGRESS** | task #167. Daemon `4977e0357ce5`; Tier 1 `86f6910dcb3f`; Tier 2 `b78ac72e0b3c` (80/80 smoke); Tier 3 + LTP scaffolding `57b151ef2e99` (templates land but operator pre-flight pending — `CONFIG_UML_NET_VECTOR=y` rebuild + LTP/kirk clone). Tier 3 design memo `phase-J-tier3-design-2026-05-14.md` (per-worker IP option b); LTP curation `phase-J-ltp-curation-2026-05-14.md` (kirk-driven, 2050-test suite, 112-skip). First 2h post-Phase-A soak `485eff5c96be`: kvm-v2 200/200. Remaining: daemon-side per-worker IP carve-out (#25), operator pre-flight, 24h soak run. |
| SMP-T55 | perf-py-startup gate restored — per-vCPU FPU-dirty epoch flag (memo 23 option a). Ratio 1.250 → 1.10 (gate ceiling 1.20). T26/T27 cross-task FPU guarantee preserved (24 000 forks, 0 CHILD_FAIL). +50% UP-hop bisect deferred. | DONE (gate passes; UP-hop bisect deferred) | `fd2f9639b0ce`, memo state-audit/22 + 23 |
| SMP-T57 | stress-ng `--vm --verify` SIGILL on kvm-v2 — **AVX/XSAVE enabled (Phase A landed `ab68bf077de3`)**: CR4.OSXSAVE + KVM_SET_XCRS(0x7) + CPUID un-mask of XSAVE/OSXSAVE/AVX/AVX2/FMA/F16C. Lazy first-dispatch arming (must install CPUID before SET_SREGS adds OSXSAVE; SET_XCRS needs OSXSAVE synchronously live in vcpu->arch.cr4 — two chicken-and-egg validations resolved). vm-method matrix on kvm-v2: 12/14 sampled methods now PASS (was 0/14). Regression battery green (substrate, cpython-parity, mt-mini, perf-py-startup, threaded-fork-malloc 0/24000). Still failing: mscan + prime-incdec (use AVX-512 `vmovdqa64 %zmm` — Phase B territory: un-mask leaf-7 AVX-512 bits + bump XCR0 to include 5/6/7). | DONE (Phase A; Phase B optional follow-up for AVX-512) | task #243; memo state-audit/25 §3.1 (Phase A) + §3.2 (Phase B deferred). |

**Closed umbrella P0:** #274 (KVM SIGSEGVs on Python C-extension
import — "table stakes") closed functionally with cpython-parity
21/21 under both UP and SMP. Phase J formalizes the closure with
24h continuous + tiered third-party tests.

---

## SMP-T13 — the migrate_disable headline fix (2026-05-02)

**Root cause:** `kvm_v2_vcpu_run` used `preempt_disable()` to pin the
running task to its current host CPU so the local `cpu`, `vcpu`, and
`run` ptrs stayed valid across dispatch. UML builds with
`CONFIG_PREEMPT_VOLUNTARY` and **no** `CONFIG_PREEMPT_COUNT`, so
`preempt_disable()` is a NO-OP — it does not actually prevent task
migration. When `handle_syscall` calls `schedule()` (e.g. from
`sched_yield`'s path), the task can migrate to a different host CPU;
on resumption it continues with stale `vcpu`/`run` ptrs. The
subsequent `KVM_RUN` ioctl operates on the wrong vCPU, racing with
the task that legitimately owns that CPU's vCPU.

**Fix:** `migrate_disable()` / `migrate_enable()` at 3 sites in
`arch/um/backend/kvm-v2/vcpu.c`: one in `kvm_v2_vcpu_run` (the
canonical migrate-pin around the dispatch loop), plus
`kvm_v2_fpu_capture_for_fork` and `kvm_v2_fpu_capture_for_switch_out`
for symmetry.

**Layer 8 of the state-audit framework** — see
`02-workstreams/D-kvm-backend/state-audit/08-smp-t13-FIXED.md` for the
full root-cause walk, the `UM_CPU_MIGRATED` empirical confirmation
probe, the test matrix before/after, and the audit timeline (Layers
1–8 over 2 days).

**Secondary fix (kept as defense-in-depth):** conditional CS/SS
reset to USER selectors when entry RIP is user-half (`f780bd5d54fa`).
Plugs a CPL=0 leak when the EINTR-mid-IDT path leaves CS=kernel
with RIP=user. Now strictly secondary to the migrate_disable fix.

---

## ~~mt-mini SMP T=8 residual — CLOSED via SMP-T33 (2026-05-03)~~ (HISTORICAL — superseded by SMP-T41 2026-05-03)

T33's `KVM_SET_SREGS` ioctl on cross-task dispatch (`9ccdc4300713`)
*reduced* the mt-mini residual but was a partial fix on the wrong
mechanism. The actual cause was the EINTR-mid-PF-stub path failing
to recover user RAX (memo state-audit/21, commit `af659ad4297d` —
SMP-T41). T33 changed dispatch timing in a way that incidentally
made the EINTR window narrower, so it appeared closer to a fix
than it was.

The ablation table below is preserved for the historical record;
all "T33 fix" entries now read with the qualifier "partial; T41 is
the true root-cause closure."

After T31/T32 ruled out TDP coherence and page-recycling, an `ncpus` ablation localized the bug definitively:

| Configuration | mt-mini SMP T=8 × 60 | flake |
|---|---|---|
| Bare host | 30/30 | 0% |
| UML+seccomp | 30/30 | 0% |
| **UML+kvm-v2 ncpus=1** | 60/60 | **0%** |
| UML+kvm-v2 ncpus=2 | 58/60 | 3% |
| **UML+kvm-v2 ncpus=4 (was)** | **50/60** | **17%** |
| **UML+kvm-v2 ncpus=4 (T33b fix)** | **118/120** | **1.7%** |

**Root cause** (opus subagent, state-audit memo 20): KVM keeps a per-vCPU LRU of recently-used TDP roots (`vcpu->arch.mmu->prev_roots`). v2 ships new CR3 via `KVM_SYNC_X86_SREGS` dirty-bit; KVM's `kvm_mmu_new_pgd` fast-switches to a cached root keyed by `cr3 | pcid` **without revalidating leaf SPTEs**. Under the per-host-CPU vCPU pool, multiple UML tasks share a vCPU; their TDP roots cycle through `prev_roots[]`; mmu_notifier flushes targeting the *active* root on a different vCPU may not invalidate the entry sitting in this vCPU's `prev_roots[]`. Re-dispatch fast-switches to stale root → mt-mini's `got=0 expect=N`.

**Fix** (commit `9ccdc4300713`): `kvm_v2_load_user_sregs` issues a full `KVM_SET_SREGS` ioctl on cross-task / cross-mm dispatch. Heavy ioctl path takes `__set_sregs2 → kvm_mmu_reset_context` which drops `prev_roots[]`. Same-task re-entries take the existing fast SYNC path (no extra ioctl).

**Cost:** one extra ioctl per cross-task dispatch. Negligible on real workloads (cpython-parity, fork-malloc unchanged in regression checks).

## ~~mt-mini SMP T=8 residual — TDP coherence RULED OUT (SMP-T31)~~ (HISTORICAL)

Three-way comparison establishes the bug as v2-specific:

| Configuration | mt-mini SMP T=8 × 30 | flake |
|---|---|---|
| Bare host | 30 / 30 | 0% |
| UML + seccomp | 30 / 30 | 0% |
| UML + kvm-v2 (production) | 22 / 30 | 27% |
| UML + kvm-v2 + T31a (madvise per-PFN in tlb_sync) | **17 / 30** | **43% (WORSE)** |

T31a (per-PFN `madvise(MADV_DONTNEED)` in `um_tlb_sync` to fire KVM's mmu_notifier) was implemented per `02-workstreams/D-kvm-backend/31-smp-t31-tdp-coherence-fix-plan.md` (Option α) and **regressed mt-mini by 16 pp**. Combined with two prior null results (G.2 IPI-kicker ablation in tlb.c:561-565 and SMP-T26 H_E in syscall_trap.c:1471-1484), three independent experiments targeting "force TDP/EPT coherence" all failed to help — and the most aggressive (T31a) actively amplified the bug.

**Conclusion:** the mt-mini `got=0 expect=N` failure is NOT in KVM TDP/EPT coherence. Full post-mortem at `02-workstreams/D-kvm-backend/state-audit/18-smp-t31-tdp-coherence-RULED-OUT.md`. Code reverted to pre-T31 state.

**SMP-T32 update (overnight session 2026-05-03):** four additional experiments — T32a (drain disabled), T32b (revert RCU defer / immediate-free), T30 H2-narrow (os_map_memory restored), T32c-prep (KVM `KVM_INVALIDATE_GFN_RANGE` ioctl, untestable without host KVM rebuild) — all negative or noise-bound at n=30-60. Total of 7 experiments now triangulating the bug class. Updated baseline at n=60 = 50/60 = 83% pass (variance was bigger than n=30 suggested, +/- 10pp). NONE of (TDP coherence | mmu_gather defer | page-recycling | spawner-mm parallel mapping) is the bug. Comprehensive memo at `02-workstreams/D-kvm-backend/state-audit/19-smp-t32-mt-mini-residual-investigation-COMPREHENSIVE.md` documents what's ruled out and what remains untestable in-session (specifically Option 3 — host KVM patch).

Next direction (when prioritized): set_pte memory-ordering audit, or pte_needsync race analysis, or cross-host (Intel) validation. None require new in-session experiments — all need fresh investigation with different tooling.

## Residual flake state

After T13 + G.2 activation, **all four reproducers PASS at 100%** at
T=8/ncpus=4 in the soak runs that produced the table above.

Two narrow-scope residuals are tracked but deferred:

- **#164 — mt-mini residual ~5% (page-recycling class)** — distinct
  from T13's CPU-migration class; bug surfaces only after the
  migrate_disable fix unblocked it. Suspect: post-munmap PFN reuse
  visible to a sibling thread before TLB drain. Not blocking #274.

- **#165 — mt-yieldonly residual ~5% RIP-corruption** — distinct
  from T13's CPU-migration class; surfaces only at very high yield
  rate. Not blocking #274.

Both are tracked as P2 follow-ups in the task tracker. They do not
gate Phase J; they do not gate snapshot/record-replay resumption.

---

## State-audit framework (the machinery that solved T13)

`02-workstreams/D-kvm-backend/state-audit/` is now the canonical
template for any future "SMP/concurrency/state-ownership"
investigation in v2:

| Layer | File                              | What it gives you |
|-------|-----------------------------------|-------------------|
| 1     | `01-state-inventory.md`           | 149 state items enumerated |
| 2     | `02-operations-inventory.md`      | 50+ operations × state items |
| 3     | `03-state-ownership-matrix.md`    | who-writes-what, who-reads-what |
| 4     | `04-suspect-register-audits.md`   | per-register "could this be the bug?" notes |
| 5     | `05-toolkit.md`                   | cscope / ast-grep / bpftrace / ftrace recipes |
| 6     | `06-findings.md`                  | 11 bug candidates ranked C1–C11 |
| 7     | `07-smp-t12-findings.md`          | 5 single-fix attempts that did NOT resolve |
| 8     | `08-smp-t13-FIXED.md`             | root cause + patch + test matrix |

**Plus** the runtime infrastructure:

- `arch/um/backend/kvm-v2/state_trace.[ch]` — per-CPU 3-layer
  shielded snapshot ring (CONFIG_y + DEFINE_STATIC_KEY_FALSE +
  debugfs `enabled` toggle). Free under CONFIG=n; near-free under
  CONFIG=y but key=off; full visibility when key=on.
- 14 `KVMV2_TRACE()` hookpoints in `vcpu.c` + `syscall_trap.c`.
- 7-line cpu+seq-tagged dump format (KVMV2T-{H,R,S,T,F,M,I}) +
  `tools/testing/selftests/um/state-trace/parse-trace.py`
  (subcommands: summary / pid / invariants / diff / mmap-zero /
  pid-window).
- Anomaly auto-freeze on mmap-returns-zero and other invariants.
- Companion reproducers: `mt-mini`, `mt-rawmmap`, `mt-yieldonly`,
  `mt-stackcheck`, `mt-byteset`, plus the canonical
  `mt-mmap-stress` soak.

---

## The merge gate (cpython-parity)

`tools/testing/selftests/um/cpython-parity/cpython-parity.sh` runs
21 curated stdlib test modules under both backends and reports
module-by-module divergence. Statuses: `PARITY`,
`DIVERGE_KVM_REGRESSION`, `DIVERGE_KVM_BETTER`, `SKIP_NO_RESULT`,
`BOTH_FAIL`.

Today's reading (UP and SMP both):

```
TOTAL: parity=21 diverge=0 skip=0
```

The gate widens for Phase J:

1. Tier 1: pytest-driven third-party libraries (`requests`,
   `cryptography`, `numpy`).
2. Tier 2: `pip install <pkg> && pytest <pkg>` — full network +
   tarball + subprocess.
3. Tier 3: small Django/FastAPI server actually serving requests
   over loopback (needs `CONFIG_UML_NET_VECTOR=y` rebuild + tap
   setup).

Each tier is an explicit kselftest. **Width before depth** —
reaffirmed.

---

## Active workstream summary

### Closed (this session)

- **#274** — KVM SIGSEGVs on Python C-extension import — closed
  functionally; Phase J formalizes.
- **G.2** activation — cross-vCPU TLB kick now ON (`ad18db7c3768`)
  after the migrate_disable fix removed the IPI-storm scenario.
- **SMP-T1 through SMP-T13** — full state-audit investigation arc.

### In progress / next

- **#166** — this STATUS.md refresh (you are reading the result).
- **#167** — Phase J validation: 24h continuous, Tier 1/2/3, soak.
- **#168** — KVM-aware `um_snapshot_ready` (was #250, deferred per
  #274). Unblocked.
- **#169** — first-class time-travel + record/replay extensions
  (was #253). Unblocked.

### Deferred residual flakes (P2)

- **#164** — mt-mini residual ~5% (page-recycling class).
- **#165** — mt-yieldonly residual ~5% RIP-corruption.

### LKML upstream queue (Phase 5, gated on Phase J for Series 7)

- **#174** — Series 1: bpf-hygiene-v1 (READY).
- **#176** — Series 2: kmsan-arch-callback-rfc.
- **#177** — Series 3: ftrace-notrace-generic-v1.
- **#178** — Series 4: backend-ops-abstraction-rfc.
- **#179** — Series 5: static-key-hot-paths.
- **#175** — Series 7: kvm-backend-series (BLOCKED by Phase J).
- **#180** — Series 6: per-profile C-series (BLOCKED by Series 7).

### Portability + ecosystem (mid-term)

- **#170** — vm/uml syzkaller backend (external).
- **#171** — ARM64 KVM v2 backend port (blocked by #178 + #183).
- **#172** — RISC-V KVM v2 backend port (blocked by #178 + #183).
- **#173** — cross-host CI matrix expansion.
- **#181** — snapshot v2 ELF64-core export (blocked by #168).
- **#182** — observability spine extensions O2.2 + O3.2.
- **#183** — C-06 BPF JIT B2 portable-emitter refactor.

---

## Discipline rules in effect

These survived the 2026-04-25 retrospective and continue to apply:

1. **`cpython-parity` is the canonical reliability metric.** Now
   that it reads 21/21, it remains the gate that future work must
   not regress. Phase J adds the Tier 1/2/3 expansion; do not
   substitute deeper metrics for breadth.

2. **No test downgrades.** If a kselftest harness sees a fatal
   signal, segfault, panic, or non-zero exit, it must FAIL.
   Skipping is allowed only for "the prerequisite isn't installed
   on this host." Codified at memo 30 (gate-discipline) and the
   `feedback_gate_discipline` memory entry.

3. **Width before depth.** Adding KUnit cases for the dispatcher
   is depth — measures the same surface more carefully. Adding
   third-party library tests, real package installs is width —
   measures whether the thing is useful at all. Width catches
   "your foundation is broken." Depth doesn't.

4. **Smell test before every "done" claim.** Would I bet $100
   that a user can do <the use case> on this build right now? If
   no, it's not done.

5. **Disagreement with the user is a signal to slow down, not push
   through.** When the user says "are you sure?" — re-test.

6. **Don't put kernel build dirs in /tmp.** ARCH=um builds live in
   `~/src/uml-builds/`. tmpfs fill silently breaks Bash stdout
   capture. Codified at `feedback_build_dirs_not_tmp`.

7. **Systematic investigation, not whack-a-mole.** When debugging
   hard problems: leverage external implementations (gVisor /
   qemu), web search, SDM knowledge, ripgrep, gdb — NOT N=30
   empirical loops with band-aid spins. Codified at
   `feedback_systematic_investigation`. The state-audit framework
   (Layers 1–8) was built specifically to operationalize this.

---

## Historical context (compacted)

The previous era of this document tracked v1's incremental fight to
reach cpython-parity 16-18/21 with deteriorating signal-to-noise. v1
was archived at tag `kvm-v1-archive-20260428` in
`arch/um/backend/kvm-v1-archive/` (gated on
`BROKEN`); see `02-workstreams/D-kvm-backend/24-eli5-and-clean-slate.md`
and `25-v2-restart-guide.md` for the full archive rationale. Stage
A's per-task vCPU foundation informed v2's design but does not
directly survive: v2 uses a per-CPU vCPU pool (memo 26 §C) instead
of v1's per-task model, because the per-CPU model is what KVM
mmu_notifier and TDP expect.

For the v1 fix history (Phases A–K, the `901213a8d2d1` keystone, the
ld-linux NULL deref dissection), see git log for tags
`stage-a-landed-20260427` and `kvm-v1-archive-20260428`. Those
investigations remain valuable as **case studies** in the kinds of
bugs the v2 architecture made structurally impossible — but are no
longer load-bearing for any current work.

---

## How to use this document

When starting a session: read the TL;DR, check the merge gate state,
pick the highest-priority `pending` task in the active workstream
summary above. **Phase J is the current P0** — anything else is
either a P2 residual or a downstream item gated on Phase J.

When ending a session: update the TL;DR, the merge gate state, and
the active workstream summary. Commit this file with the change.

When in doubt about whether to work on something: if it claims to
"close" v2 work, ask whether it's gated on Phase J. If it's a new
feature, ask whether the snapshot or record/replay paused work
(#168 / #169) is more directly aligned with the vision.
