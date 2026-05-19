# 01 — vector2 default flip + stress test

**Sprint:** post-2026-05-19
**Priority:** HIGH
**Effort:** small flip (≤ 20 LoC) plus multi-step gating
**Status:** planned
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

**Acceptance:** 30/30 PASS, 0 panics, 0 SIGBUS, 0 high-cr2,
`KVM_V2_TLB_LAG` median below 1000 (the pre-R14 values 1734
correlated with workload but not with failure — we want to
re-baseline).

**Time budget:** ~60 minutes wall.

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

### Step 3 — Long-soak natural completion (P4.4)

Re-run the seccomp + vector2 Tier 3 soak with a fresh
7200-second budget and let it complete naturally
(no operator-stop). Add a kvm-v2 + vector2 Tier 3 soak
of the same length running in parallel on a separate
output directory.

**Acceptance:** both soaks complete by budget exhaustion (not
operator stop, not failure-stop), with ≥ 97% Wilson 95% CI
lower bound on aggregate PASS rate per backend.

### Step 4 — Flip the umlctl default + propagate

Once Steps 1–3 pass:

```rust
// tools/uml/uml-launcher/src/bin/umlctl/deploy.rs::NetworkSection::default
driver: "vector2".into(),    // was "vector"
```

Plus:

  - Update soak templates `tier3-django.toml.template` and
    `tier3-fastapi.toml.template` to bake `driver = "vector2"`
    (today they use `{{NETWORK_DRIVER}}`).
  - Add `--with-vector2` flag (or Phase 5b) to
    `umlctl mission` that runs at least one tier3 iteration to
    exercise the new default end-to-end.
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
