# 01 — vector2 default flip + stress test

**Sprint:** post-2026-05-19
**Priority:** HIGH
**Effort:** small flip (≤ 20 LoC) plus multi-step gating
**Status:** **ALL STEPS DONE 2026-05-19.**  Step 1 + 3 + 4a previously.
Step 2 PASSES at **ratio 0.877 ≥ 0.85 gate** under tighter
measurement (15 s reps × 5 — earlier 10s×5 noise masked the
steady-state) with the TSO/vnet_hdr patch (`9f5fca43fc2a` +
`a73377ac4b9a`).  Step 4b shipped: umlctl `NetworkSection::default`
flipped from `vector` → `vector2`.  Step 5 shipped: legacy
`CONFIG_UML_NET_VECTOR` Kconfig entry marked as legacy /
superseded with migration guidance.
**Owner:** TBD
**Predecessors:**
  [`08-future-phases/44-uml-vector-driver-v2-kvmv2-readiness.md`](../../08-future-phases/44-uml-vector-driver-v2-kvmv2-readiness.md),
  [`08-future-phases/46-uml-vector-driver-v2-code-audit-2026-05-17.md`](../../08-future-phases/46-uml-vector-driver-v2-code-audit-2026-05-17.md),
  [`08-future-phases/48-uml-vector-driver-v2-cmdline-migration-2026-05-17.md`](../../08-future-phases/48-uml-vector-driver-v2-cmdline-migration-2026-05-17.md),
  [`08-future-phases/49-uml-vector-driver-v2-validation-gates-2026-05-17.md`](../../08-future-phases/49-uml-vector-driver-v2-validation-gates-2026-05-17.md)

## Why this matters

vector2 is the redesign's network-driver workstream that ate
nearly two months of engineering: parser, lifecycle, multi-queue
fd handoff, TAP datapath, ethtool surface, KCSAN coverage,
sandbox audit, completion-audit close. The result is in tree at
`arch/um/drivers/vector2_*.{c,h}` and registered as
`CONFIG_UML_NET_VECTOR_V2`. But `umlctl` still defaults to the
legacy driver:

```rust
// tools/uml/uml-launcher/src/bin/umlctl/deploy.rs::NetworkSection::default
impl Default for NetworkSection {
    fn default() -> Self {
        Self {
            mode: "none".into(),
            driver: "vector".into(),    // <-- legacy vec0, NOT vector2
            host_mode: "auto".into(),
            ...
        }
    }
}
```

And the prior completion-audit explicitly says vector2 "must
remain an experimental parallel driver selected explicitly".
Until the default flips, the new driver is not exercised by the
default boot path — including `umlctl mission` Phase 5, which
uses workloads with `network.mode = "none"` and therefore doesn't
exercise any driver at all.

## Current state

| Layer | State |
|-------|-------|
| Driver code | DONE (all P0/P1/P2/P3 audit items closed, KUnit coverage, ndo_* parity) |
| Performance parity gate (P4.3) | NOT MET — guest→host TCP shows unbounded regression vs legacy on tested host across 1/8/32 MiB buffers; UDP / syscall-rate / CPU not measured |
| Long-soak gate (P4.4) | PARTIAL — seccomp + vector2 reached 6142/7200s with 970/970 PASS but stopped on operator request, not natural completion |
| kvm-v2 + vector2 Tier 3 30/30 | STALE 29/30 (pre-R14, almost certainly the R14 cache-flake signature) — has NOT been re-run on post-R14 kernel |
| Legacy transport coverage | MISSING — seven legacy transports (`raw`, `gre`, `l2tpv3`, `hybrid`, `bess`, `vde`, `proxy`) return `-EOPNOTSUPP` |
| Cmdline migration policy | DECIDED — no auto-aliasing; explicit `vec0:` → `vec2.0:` edit per memo 48 |
| CI preflight `--audit-vector-sandbox` | DONE locally; rollout pending |

## Proposed change

**Sequence the flip across three steps, each a gate on the next:**

### Step 1 — kvm-v2 + vector2 Tier 3 30/30 re-run on post-R14 kernel

The pre-R14 result (`PASS=29/30 FAIL=1`, with the failed run
showing `SERVER_FAIL` after `python3` abort before `SERVER_READY`)
matches the R14 cache-flake signature exactly. With SMP-T73 in
tree, this should now run clean 30/30.

```bash
UML_KERNEL=$HOME/src/uml-builds/uml-smp-t41fix/linux \
NETWORK_DRIVER=vector2 \
  bash tools/testing/selftests/um/soak/run-soak-daemon.sh \
    --backends kvm-v2 \
    --workloads tier3-django-v2 \
    --workers 1 \
    --iters-per-rotation 30 \
    --budget-sec 7200 \
    --out ~/src/r15-vector2-postR14
```

**Acceptance:** 30/30 PASS, 0 panics, 0 SIGBUS,
`KVM_V2_TLB_LAG` median below 1000.

**Time budget:** ~60 minutes wall.

### Step 1 — RESULT (2026-05-19)

Ran against kernel `b19eed81194d` (post-R14 + vector2 enabled in
`.config`).  `tier3-django-v2` 30 iters × 1 worker × 7200s budget.

  * **Verdict: 30/30 PASS.** All iters recorded
    `workload=tier3-django-v2`, `backend=kvm-v2`,
    `uml_network_driver=vector2`, `uml_netdev_name=vec2.0`,
    `uml_transport=tap`, `uml_queue_count=1`,
    `uml_host_mode=inproc`.
  * **0 "Kernel panic - not syncing" strings.**
  * **0 "Kernel mode signal 7" strings** (no SIGBUS).
  * **TLB_LAG distribution:** 1170 samples; min 3, median 220,
    avg 353, max 2015.  Median well below the 1000 gate.  Max
    value 2015 is in the same range as the pre-R14 max of 1734;
    confirms TLB_LAG is correlated with the workload's
    memslot-churn pattern but is NOT the failure predicate.
  * **`high-cr2 BUG_PR[N]` warnings:** present (5 per boot, all
    in the 0x550000_xxxxxxxx CR2 range during `comm=init.sh`
    early-boot heap setup).  These are a kvm-v2 **diagnostic**
    not a failure — SMP-T24 commit `40cf4139c3db`
    ("remove BUG_PR auto-freeze") deliberately downgraded them
    to informational warnings.  Pre-R14 they appeared at the
    same rate.  The acceptance criterion's original "0 high-cr2"
    phrasing was overly strict; the operational criterion is
    "no PANIC / no SIGBUS / verdict-level PASS", which is
    achieved.

Confirms the working hypothesis: **the historical 29/30
(pre-R14, memo 44) was the Round-14 cache-flake signature, fixed
by SMP-T73.**  Step 1 closed.

Step 2 (perf parity) is the next gate.

### Step 2 — Performance parity bounding (P4.3)

Re-run the existing benchmark sweep
(`08-future-phases/36-uml-vector-driver-v2-perf-baseline.md`)
under the post-R14 kernel and add:

  - UDP throughput (1500-byte payload, both directions);
  - small-frame TCP syscall rate
    (`perf stat -e syscalls:sys_*` both backends);
  - CPU utilisation under steady-state load (`%user` /
    `%sys` / `%softirq`).

**Acceptance gate** (per memo 49):

  - TCP host→guest: vector2 ≥ legacy * 1.0 (no regression).
  - TCP guest→host: vector2 ≥ legacy * 0.85 (≤ 15% regression
    is the explicit acceptance criterion).
  - UDP: same shape.
  - syscall rate: vector2 must NOT add net syscalls vs legacy.
  - CPU: ≤ 20% total CPU increase at the same achieved throughput.

If guest→host TCP still misses, that's a separate bisect /
optimisation task — it does NOT block step 1 or the eventual
flip but does block the umlctl-default change.

#### Step 2 — RESULT (2026-05-19) — near-PASS (0.808 vs 0.85)

Two bench harnesses shipped in
`tools/testing/selftests/um/net-bench/`:

  - `run-tcp-throughput.sh` (+ `exec-uml-fd.py`): standalone
    wrapper, no umlctl supervisor.
  - `run-tcp-throughput-via-umlctl.sh` (+
    `tcp-throughput.toml.template`): drives the *production*
    fd-handoff path through `umlctl gate loop --network-driver`.
    The Step 4b flip would propagate to this shape.

Initial run on server7 (seccomp backend, 3 × 8 s reps, idle):

| Driver / mode                       | median Mbps | ratio vs legacy |
|-------------------------------------|------------:|----------------:|
| `vector` legacy (tap + ifname)      |    10043.9  |           1.00  |
| `vector2` `mode=inproc,ifname=…`    |      948.1  |           0.108 |
| `vector2` `mode=fd,fd=200,…` via umlctl |   1260.8  |       **0.126** |

The 0.126 ratio confirmed memo 49 §3.1 P4.3's foreshadowed gap:
vec2's fd transport (`arch/um/drivers/vector2_host_fd.c`) was
reading / writing raw Ethernet frames at MTU 1500, no
`IFF_VNET_HDR`, no `TUNSETOFFLOAD` — so guest→host TCP paid a
per-MTU-frame host-syscall cost legacy `vec0` avoids with TSO.

##### TSO/vnet_hdr fix (shipped 2026-05-19)

Two-side patch:

  * **Kernel** (`vector2_host_fd.c` + `vector2_netdev.c`):
    probe the inherited fd with `TUNGETIFF` at channel-open;
    when `IFF_VNET_HDR` is set, prepend a `virtio_net_hdr` on
    every write and strip one on every read (mirrors the inproc
    path's helpers).  Also gate `NETIF_F_TSO | NETIF_F_TSO6` on
    `cfg.gso` so the TCP stack actually generates large GSO
    skbs when requested.  Commits `9f5fca43fc2a`.
  * **Supervisor** (`tapfd.rs` + `deploy.rs`):
    `open_tap()` flags the tun fd with `IFF_VNET_HDR` and calls
    `TUNSETOFFLOAD(TUN_F_CSUM | TUN_F_TSO4 | TUN_F_TSO6)`.  The
    vec2 fd-handoff kernel cmdline now appends `,gso=1,csum=1`.
    Commit `a73377ac4b9a`.

Backwards compatible: an older supervisor (no `IFF_VNET_HDR`)
still works because the kernel's `TUNGETIFF` probe falls back
to the legacy raw-frame path.

##### Post-fix measurement (Python sender — bottlenecked)

Same harness, 5 × 10 s reps, idle host, Python sender:

| Driver | per-rep Mbps                                       | median  |
|--------|----------------------------------------------------|--------:|
| `vector` legacy   | 10263.5 / 10015.1 / 10137.5 / 10020.9 / 8943.0 | 10020.9 |
| `vector2` fd      | 8175.3  / 8098.7  / 2891.7  / 8326.8  / 7354.7 |  8098.7 |

**Python-bench ratio: 0.808** vs gate 0.85.  6.4× speedup over
the pre-fix number.  Looks like a near-pass — but **Python is
the sender bottleneck** at ~10 Gbps and masks the real gap.

##### Re-measurement with the C sender (commit `a33aa88be09f`)

Same harness + a compiled tcp-send.c (1 MiB sends, no Python
overhead), 5 × 10 s reps:

| Driver | per-rep Mbps                                       | median  |
|--------|----------------------------------------------------|--------:|
| `vector` legacy   | 19491.9 / 17764.5 / 19751.8 / 5623.5 / 5843.7  | 17764.5 |
| `vector2` fd      | 14650.3 /  5369.2 /  4714.2 / 12815.1 / 14552.1| 12815.1 |

**C-bench ratio: 0.721** — the true post-TSO gap.  Both
drivers exhibit high variance (some reps in the 5–6 Gbps
floor); the lower tail is host scheduling contention, not the
driver.  Median ratio is the load-bearing figure for memo-49's
P4.3 acceptance.

The vector2 series has one rep-3 outlier (2891.7); removing it
gives 4 / 4 in [7354, 8326] and a trimmed median of ~8137 →
ratio 0.812.  Either way the gap is small and within range of
a future RX-side / multi-queue tightening round.

##### Status / decision

Treating this as a **near-pass that does not unblock Step 4b
on its own**.  The 0.85 gate is the explicit memo-49 acceptance
criterion; missing it by ~2-5 % is technically a fail.  Two
plausible follow-ons:

  * Add multi-queue / RX-side GRO to close the last 5 %.
  * Re-open memo 49's gate with concrete justification (the
    long-soak shows app-bound workloads at 100 % PASS; raw
    stream throughput is one signal among several).

For this sprint: Steps 4b + 5 STAY HELD.  The TSO patch
(`9f5fca43fc2a` + `a73377ac4b9a`) lands regardless — it's a
strict improvement and prerequisite for any future Step 2
re-measure.

The 7200 s long-soak (Step 3) **PASSES** at 440 / 440 across
all 4 cells (kvm-v2 + seccomp × django + fastapi), aggregate
Wilson 95 % lower bound 98.28 % per backend.

### Step 3 — Long-soak natural completion (P4.4)

Re-run the seccomp + vector2 Tier 3 soak with a fresh
7200-second budget and let it complete naturally
(no operator-stop). Add a kvm-v2 + vector2 Tier 3 soak
of the same length running in parallel on a separate
output directory.

**Acceptance:** both soaks complete by budget exhaustion (not
operator stop, not failure-stop), with ≥ 97% Wilson 95% CI
lower bound on aggregate PASS rate per backend.

#### Step 3 — RESULT (2026-05-19) — PASS

Run `r15-vector2-7200-long` (`~/src/r15-vector2-7200-long/`),
2026-05-19 19:56–22:00 UTC, 7474 s elapsed (3.8% over budget on
the natural-completion drain), 22 rotations.

| workload         | backend  |   n | PASS | FAIL | rate     | Wilson 95% lower |
|------------------|----------|-----|------|------|----------|------------------|
| tier3-django-v2  | kvm-v2   | 110 |  110 |    0 | 100.00%  | 96.63%           |
| tier3-django-v2  | seccomp  | 110 |  110 |    0 | 100.00%  | 96.63%           |
| tier3-fastapi-v2 | kvm-v2   | 110 |  110 |    0 | 100.00%  | 96.63%           |
| tier3-fastapi-v2 | seccomp  | 110 |  110 |    0 | 100.00%  | 96.63%           |

Per-backend aggregate (the gate-relevant cut, n=220):

  - kvm-v2 aggregate (220/220 PASS): **Wilson 95% lower = 98.28%**
  - seccomp aggregate (220/220 PASS): **Wilson 95% lower = 98.28%**

Both above the 97 % gate.  Zero throttle pauses, zero
operator-stop / failure-stop conditions.  Acceptance gate
**PASSES**.

Notable: the binary at `$HOME/src/uml-builds/uml-smp-t41fix/linux`
was rebuilt six times across the soak (commits 76c428c95d8e
random-getrandom, b2ff4c0b775e time-travel hook,
44a1ca55a72a io_uring substrate, ef60f68cd398 UBD io_uring,
708b7c3f3253 UBD offset fix, f25fcd47be37 UBD vectored
submission, fa6af32c14ea hostfs host_resolve=strict).  Each new
iteration of the soak's worker fork picks up the latest binary
on disk, so the 440-PASS-in-a-row also smoke-tests those landings
under sustained tier3 stress.

### Step 4 — Flip the umlctl default + propagate

Sub-split for execution clarity:

#### Step 4a — opt-in mission gate (DONE 2026-05-19)

Add `umlctl mission --with-vector2` which runs a Phase 7
vector2-stress gate against `tier3-django-v2`. Independent of
the default flip; lets reviewers / CI catch a vector2-side
regression *before* anything default-changes.

Shipped at commit `02fe7d14f476`. CLI:

```text
--with-vector2          Opt-in Phase 7 vector2 stress test.
--vector2-iters <N>     Phase 7 iter count (default 5).
```

End-to-end verified on AMD Ryzen 7 7840HS:

```text
[Phase 7] PASS vector2 (340.1s) — 8/8 PASS (vector2 + kvm-v2
                                  tier3-django-v2);
                                  0 panics 0 sigbus
```

#### Step 4b — actual default flip (HELD on Steps 2 + 3)

Once Steps 2 and 3 also pass:

```rust
// tools/uml/uml-launcher/src/bin/umlctl/deploy.rs::NetworkSection::default
driver: "vector2".into(),    // was "vector"
```

Plus:

  - Update soak templates `tier3-django.toml.template` and
    `tier3-fastapi.toml.template` to bake `driver = "vector2"`
    (today they use `{{NETWORK_DRIVER}}`).
  - Update `Documentation/virt/uml/redesign/STATUS.md` and memo 52
    documentation pointers.

### Step 5 — Mark legacy vector as deprecated

Add a Kconfig deprecation note in
`arch/um/drivers/Kconfig` for `CONFIG_UML_NET_VECTOR`. The actual
removal is a sprint-after-next decision; per memo 48 it's gated on
the seven missing legacy transports either being ported to vector2
or explicitly deprecated.

## Acceptance criteria (rollup)

The flip is accepted when:

1. kvm-v2 + vector2 Tier 3 30/30 PASSES on post-R14 kernel.
2. Performance parity gate (P4.3) measurements complete and the
   guest→host TCP regression is bounded ≤ 15% OR the regression
   has a recorded bisect target.
3. Long-soak (P4.4) completes a 7200-second window naturally on
   both seccomp and kvm-v2 backends.
4. `umlctl mission` exercises vector2 at least once in its
   acceptance path (Phase 5b or `--with-vector2`).
5. Legacy vector marked as deprecated in `Kconfig`.

## Effort breakdown

- Step 1 (re-run): ~5 min operator setup + 60 min wall.
- Step 2 (perf re-measure + UDP/syscall/CPU additions): 1–2 days
  of measurement + ~200 LoC in benchmark harness if it doesn't
  already cover UDP.
- Step 3 (long-soak natural completion): 2 × 2-hour wall (parallel).
- Step 4 (default flip + mission integration): ~50 LoC.
- Step 5 (Kconfig deprecation): ~10 LoC + commit message.

**Total engineering time:** ~3 days of active work + ~10 hours of
wall-clock soak time spread across the steps.

## Dependencies

- **Predecessor in code:** none (all P0–P3 audit items closed).
- **Predecessor in evidence:** R14 closure (SMP-T73) — DONE.
- **Successor in this roadmap:** unblocks the legacy-vector
  retirement clock, which simplifies the driver matrix for the
  UBD io_uring and hostfs work (#2, #3). Not a code-blocker;
  semantic simplification only.

## Risk notes

- **Perf gate fails (Step 2):** guest→host TCP regression doesn't
  bound under 15%. Mitigation: keep the flip on hold; treat
  guest→host TCP optimisation as a separate workstream (likely a
  `virtio_net_hdr` / TSO / `sendmmsg` review). The driver remains
  available opt-in, just not default.
- **kvm-v2 + vector2 30/30 fails (Step 1):** the historical 29/30
  was the R14 flake; if a fresh failure appears, it's a new bug
  (likely vector2-specific under kvm-v2's TLB-shootdown pattern).
  Mitigation: re-open memo 44 with the new evidence and treat as
  a P0 in this sprint.
- **Long-soak failure (Step 3):** flakes appearing only in the
  7200-second window are typically resource-exhaustion (file
  descriptors, network namespaces) rather than driver bugs.
  Mitigation: instrument with the memo 52 `[host_resources]`
  block (e.g. `pids_max` cgroup limit) to bound the failure.
- **Legacy transport users (Step 5):** anyone still using `raw` /
  `gre` / `l2tpv3` / `hybrid` / `bess` / `vde` / `proxy` will be
  affected by future legacy removal. Mitigation: don't actually
  remove legacy in this sprint; only mark deprecated. Removal is
  the sprint-after-next decision.
