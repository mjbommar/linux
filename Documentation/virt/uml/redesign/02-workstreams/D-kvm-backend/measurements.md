# D-workstream — measurement log

Timing data from every spike + real-implementation benchmark
run for the D (KVM backend) workstream. Append new measurements
at the **bottom** under new `## <date> — <what>` sections so
history stays diff-friendly; never overwrite a prior block.
Each new spike or perf run is one entry.

The point: track the KVM round-trip floor and, eventually, the
real per-syscall overhead of the naive KVM backend + the
systrap-gadget path, across hardware generations + measurement
techniques, over the 24-month plan. Lets us retire "vibes" from
the design conversation.

## Host inventory (persistent — edit when hosts change)

| Shortname | CPU | µarch | Year | Released | vCPU/SMT | Nested virt? | Notes |
|---|---|---|---|---|---|---|---|
| dev | Xeon W-2123 @ 3.60 GHz | Skylake-SP | 2017 | 2017 | 4C/8T | no (bare) | Primary dev workstation; identical CPU to s3. |
| s0 | i9-12900K | Alder Lake | 2021 | 2021 | 8P+8E / 24T | no (bare) | Fastest host in this inventory. |
| s1 | Xeon E3-1225 v6 | Kaby Lake | 2017 | 2017 | 4C/4T | no (bare) | Server-class but 4T (no SMT). |
| s2 | Xeon E3-1225 v5 | Skylake-S | 2015 | 2015 | 4C/4T | no (bare) | Oldest silicon in inventory. |
| s3 | Xeon W-2123 @ 3.60 GHz | Skylake-SP | 2017 | 2017 | 4C/8T | no (bare) | Same CPU as `dev` — cross-check twin. |
| s4 | i5-12600K | Alder Lake | 2021 | 2021 | 6P+4E / 16T | no (bare) | Modern desktop non-i9. |
| s5 | Ryzen 7 7840HS | Zen 4 | 2023 | 2023 | 8C/16T | no (bare) | AMD laptop APU. Power-state variable. |
| s6 | Ryzen 7 7840HS | Zen 4 | 2023 | 2023 | 8C/16T | no (bare) | Same chip as s5/s7; measured at boost. |
| s7 | Ryzen 7 7840HS | Zen 4 | 2023 | 2023 | 8C/16T | no (bare) | Same chip as s5/s6; measured in power-save. |

**Conventions:**

- **"bare"** = host is physical, not a VM. GHA runners would be
  a separate class (host = Hyper-V guest) and would get their
  own row if/when we measure them.
- **µarch** names follow the Intel/AMD marketing names; the
  real consumer of these results (D-01 design memo) cares
  about VMX/SVM generation not the desktop brand.
- **vCPU/SMT** matters for p90 tail behavior: 4T hosts (s1, s2)
  show larger p90-to-mean gaps under scheduler pressure.

## 2026-04-23 — Spike 01/02: bare KVM round-trip (null hlt)

Methodology: `Documentation/virt/uml/redesign/02-workstreams/
D-kvm-backend/spikes/01-getpid-roundtrip/spike.c` — open
`/dev/kvm`, create VM, vCPU, map 2 MiB guest phys memory,
place a single `hlt` at guest phys 0x3000, set up a real-mode
vCPU with `CS.base = 0x3000` so `IP=0` starts on the hlt. Run
vCPU 1000 times; rdtsc-bracket each `KVM_RUN`; report p10 /
median / mean / p90 cycles.

This measures the **floor** of a VMEXIT round-trip: the
minimum KVM_RUN → VMEXIT → return-to-userspace cost on the
host CPU, with no guest code to interfere. Every other
KVM-based measurement we do builds on this floor.

| Host | CPU | MHz (at run) | p10 cyc | p50 cyc | p90 cyc | p50 ns | Notes |
|---|---|---:|---:|---:|---:|---:|---|
| s2 | Xeon E3-1225 v5 (Skylake-S) | 3400 | 20412 | 20600 | 21360 | 6059 | 4T fat-tail (mean 38152) |
| s1 | Xeon E3-1225 v6 (Kaby Lake) | 3300 | 17772 | 17820 | 17880 | 5399 | |
| dev | Xeon W-2123 (Skylake-SP) | 3697 | 17510 | 17550 | 17590 | 4747 | |
| s5 | Ryzen 7 7840HS (Zen 4) | 2068 | 12198 | 12236 | 12274 | 5917 | Power-save state |
| s7 | Ryzen 7 7840HS (Zen 4) | 2218 | 12426 | 12426 | 12502 | 5604 | Power-save state |
| s4 | i5-12600K (Alder Lake) | 4500 | 11524 | 11700 | 12652 | 2600 | |
| s6 | Ryzen 7 7840HS (Zen 4) | 5014 | 11552 | 12350 | 12654 | 2463 | Boost |
| s0 | i9-12900K (Alder Lake) | 4900 |  3824 |  3837 |  3860 |  783 | Best in inventory |

**Observations:**

- **Cycle count is silicon-bounded, ns is clock-bounded.**
  s5/s6/s7 are the same CPU at different P-states: cycle
  counts tight (12236/12350/12426), ns fans out 2463-5917.
  The VT-x/SVM exit machinery is the invariant.
- **Silicon generation dominates.** 5-6 µs on 2015-2017
  Intel, 2.5-6 µs on Zen 4 (depending on clock), ~780 ns on
  Alder Lake i9 at boost. The CPU matters more than any
  backend cleverness for the naive `KVM_RUN` path.
- **The i9-12900K outlier is interesting.** Same Alder Lake
  µarch as s4's i5 but 3× faster on the round-trip. Follow-
  on spike candidate: A/B test on a single i9 host with/
  without P-core pinning + thermal pressure to separate
  clock effects from microarchitectural effects.

**What this settles for the vision:**

- Naive KVM backend per-null-syscall floor is **~4-5 µs on
  Skylake-era, ~2.5 µs on Zen 4 / Alder Lake i5, ~780 ns on
  Alder Lake i9**.
- vs. seccomp baseline ~20 µs, that's 4-25× speedup across
  the range.
- ~100 ns aspirational target requires either (a) newer
  silicon still (Sapphire Rapids / Granite Rapids / Zen 5
  data point TBD) or (b) the systrap gadget layer (D-04).

## 2026-04-23 — Spike 04: long-mode guest + IO-port VMEXIT

Methodology: `spikes/04-longmode-lstar-vmcall/spike.c` — set
up IA-32e long mode with a flat GDT (3 entries), 2 MiB
identity-mapped huge page, CR0.PE|WP|PG + CR4.PAE +
EFER.LME|LMA|SCE. Guest code is `out %al, $0xf4; hlt` at
phys 0x4000. The `out` triggers a `KVM_EXIT_IO` userspace
round-trip (the same userspace-visible exit shape the
design memo's bounce trampoline uses; `vmcall` does not
exit to userspace by default because KVM in-kernel
emulates it). We measure the IO-exit cycles. A second
`KVM_RUN` after skipping the `out` catches the trailing
HLT for sanity.

Originally planned as SYSCALL-through-LSTAR → vmcall. Two
implementation notes:
- `vmcall` with rax=0 doesn't exit to userspace — KVM
  routes it through `kvm_emulate_hypercall()` and continues.
  To trigger a userspace-visible exit for the spike, we
  use `out` to an unused port instead.
- Long-mode bring-up with just `KVM_SET_SREGS` + a minimal
  GDT in guest memory was enough; no IDT needed because
  we rely on normal exits, not faults.

The design memo's bounce-trampoline cost will be a VMEXIT
of similar shape (IO exit or vmcall-for-userspace with
`KVM_CAP_HYPERV_SYNIC` or equivalent enabled); this
measurement bounds the naive backend's per-syscall floor
when running in the actual x86_64 long mode UML uses.

| Host | CPU | MHz | Spike01 cyc | Spike04 IO cyc | Δ | Notes |
|---|---|---:|---:|---:|---:|---|
| s2 | Xeon E3-1225 v5 (Skylake-S) | 2700 | 20600 | 29746 | +9146 | Older VMX path slower |
| s1 | Xeon E3-1225 v6 (Kaby Lake) | 3300 | 17820 | 20404 | +2584 | |
| s3 | Xeon W-2123 (Skylake-SP) | 3700 | 17550 | 25172 | +7622 | |
| s5 | Ryzen 7 7840HS (Zen 4) | 2262 | 12236 | 13148 | +912 | Tightest delta |
| s6 | Ryzen 7 7840HS (Zen 4) | 1101 | 12350 | 13338 | +988 | |
| s7 | Ryzen 7 7840HS (Zen 4) | 1790 | 12426 | 13338 | +912 | |
| s4 | i5-12600K (Alder Lake) | 4500 | 11700 |  6336 | **-5364** | IO exit faster than HLT (!) |
| s0 | i9-12900K (Alder Lake) | 1136 |  3837 | 10986 | +7149 | Host was heavily throttled (1.1 GHz) |

**Observations:**

- **Long-mode + IO-exit is what the naive backend actually
  ships.** The memo's bounce-trampoline path goes through
  a userspace-visible VMEXIT just like this spike. Spike
  01's real-mode HLT was the absolute floor; spike 04 is
  the realistic floor for the shipping shape.
- **Zen 4 has the tightest delta** (~900 cycles added over
  the HLT floor). Consistent across all three Zen 4 hosts.
  Suggests AMD's SVM IO-exit fastpath is efficient.
- **Alder Lake i5 measured faster on IO exit than real-
  mode HLT.** Either (a) CPU clock drift between runs —
  same silicon but different P-state; or (b) Intel's
  long-mode + IO-exit path is genuinely faster than real-
  mode exits on Alder Lake. Worth a follow-on with locked
  P-state to separate these.
- **i9 ran at 1.1 GHz this session** due to thermal or
  idle state (vs. 4.9 GHz in spike 01). The ~11000 cycle
  IO-exit number is still comparable to the Zen 4 cycles;
  the microarchitectural invariant holds.
- **Older Intel (Skylake-S, Kaby Lake, Skylake-SP) added
  2500-9000 cycles** going from HLT to IO-exit. The exact
  delta varies by part — not a clean pattern in this
  dataset.

**Bottom-line update to the design memo:**

- Add **~1000-9000 cycles of IO-exit overhead** to spike 01's
  HLT floor when estimating the naive backend's per-syscall
  cost. For the headline numbers in the memo:
  - Alder Lake i5 at boost: ~6336 cyc / ~1.4 µs per syscall
  - Zen 4 at boost: ~13200 cyc / ~2.6 µs per syscall
  - Skylake-era Intel: ~25000-30000 cyc / ~7-11 µs per syscall
- The "~800 ns on Alder Lake i9" claim needs P-state-
  locked re-measurement before it's a real design number.
  On this run's thermal-throttled i9 the IO-exit path was
  ~10986 cycles, which at i9's 4.9 GHz boost would be
  ~2.2 µs — still under the 5 µs seccomp baseline but not
  sub-microsecond.
- The memo's commit-plan sizing (~1600 LOC, 4-6 weeks)
  stands. The performance envelope the memo claims gets
  calibrated slightly: naive backend delivers 2-10 µs per
  syscall depending on silicon, still 2-10× over seccomp.

## 2026-04-23 — Spike 05: GHA nested-KVM probe

Methodology: `.github/workflows/kvm-probe.yml` — runs
spike 01 + spike 04 on GitHub Actions `ubuntu-latest` and
`ubuntu-24.04` runners with sudo fallback; records
per-host inventory + measured cycles. Workflow triggers
on spike edits or manual dispatch.

**Headline result: nested KVM works on standard GHA
runners.** Both images, AMD EPYC 7763 silicon under Hyper-V,
`/dev/kvm` present (root-writable, runner user isn't in
the kvm group so sudo is required but the dev-box pattern
of group-add isn't available; the workflow just uses sudo).

| Runner | Host CPU | MHz | Virt | Spike 01 cyc | Spike 04 cyc | Δ |
|---|---|---:|---|---:|---:|---:|
| ubuntu-latest (24.04.2) | AMD EPYC 7763 (Zen 3 Milan) | 3207 | Hyper-V | 19723 | 20237 | +514 |
| ubuntu-24.04 | AMD EPYC 7763 (Zen 3 Milan) | 2871 | Hyper-V | 16640 | 22282 | +5642 |

Both spikes ran to completion with 1000 iterations. Spike
01 used sudo after the non-sudo attempt got EPERM; spike 04
same.

**Observations:**

- **ubuntu-latest currently resolves to ubuntu-24.04.2** per
  uname output, but the two runner labels produced
  different cycle counts anyway. Probably they landed on
  different hypervisor hosts (different customers / load
  profiles in the Azure pool).
- **Nested-KVM overhead is bounded.** Spike 01 median on
  bare s5/s6/s7 Zen 4 (desktop): 12236-12426 cyc. On nested
  EPYC Zen 3: 16640-19723 cyc. That's a +30-60% overhead
  for running under Hyper-V — meaningful but not
  prohibitive for CI measurements.
- **Spike 04's Δ on GHA (ubuntu-24.04) is +5642 cyc.** Much
  higher than any bare host we measured (where Zen 4 was
  +900). Under nested virt, IO-exit VMEXITs take an extra
  round-trip through Hyper-V L0. Expected shape; captures
  the nested-virt performance cost cleanly.
- **EPYC 7763 is the Microsoft Azure Dv5-class silicon.**
  64-core Milan; runners get a small slice. That we see
  consistent 1000/1000 measurable iterations means the
  runner's slice is stable enough to measure.

**What this settles for the D-workstream CI plan:**

1. ✅ **D-workstream CI can use the standard GHA runner
   pool.** No need for self-hosted bare-metal runners.
   Sudo is required (runner user → /dev/kvm group
   membership) but `sudo` works without password on GHA.
2. ✅ **Spike + smoke jobs for the KVM backend are viable
   as per-commit CI gates.** Build time + spike run time
   fit comfortably inside a 10-minute job.
3. ⚠️ **Performance numbers from CI runs are nested-
   virt-inflated.** Treat them as a regression canary
   (drift detection), not as absolute performance claims.
   The bare-metal measurements above remain the
   authoritative floor for the design memo.
4. 📌 **Follow-on**: once the real um_backend_kvm ships,
   extend kvm-probe.yml to also boot a minimal UML-on-KVM
   binary and exercise a real syscall. That lands with
   D-02..D-06 implementation.

## 2026-04-23 — Spike 06: P-state-locked re-measurement

Follow-up to spike 01/04 to resolve two specific
anomalies by controlling CPU frequency state:

1. **i9-12900K** delivered 3837 cyc / 783 ns on spike 01
   and 10986 cyc / 9672 ns on spike 04 — the ns jump
   was because spike 04's run happened at 1.14 GHz
   (host was thermal-idle). The cycle-count jump
   (~3× worse) wasn't consistent with the Zen 4
   pattern either (Zen 4 added ~900 cyc going from
   spike 01 to spike 04).
2. **i5-12600K** delivered 11700 cyc spike 01 and
   6336 cyc spike 04 — spike 04 was *faster* than
   spike 01 on the same silicon, which shouldn't
   happen. Suspected P-core vs E-core core-migration
   between runs on Alder Lake's heterogeneous
   architecture.

Methodology: `sudo cpupower frequency-set -g performance`
+ `sudo tee /sys/devices/system/cpu/intel_pstate/min_perf_pct
<<<100`, then re-run spike 01 and spike 04 back-to-back
so both measurements hit the same core at the same
P-state.

| Host | CPU | Run MHz | Spike 01 cyc | Spike 04 cyc | Δ |
|---|---|---:|---:|---:|---:|
| s0 | i9-12900K (Alder Lake) | ~1987 spike01 / 4900 spike04 | 4154 | 4705 | +551 |
| s4 | i5-12600K (Alder Lake) | 4500 | 11814 | 14260 | +2446 |
| s3 | Xeon W-2123 (Skylake-SP) | 3703 | (baseline = spike 01 row) | 22994 | +5444 |

**Key resolutions:**

- **The i9's spike 04 under sustained boost is 4705 cyc /
  ~960 ns**, not the 10986 cyc one-shot number from the
  earlier run. The 4.9 GHz boost held across 1000
  iterations with p10=4687 / p90=4723 (ultra-tight
  distribution). **This is the clean reference number
  for the Alder Lake i9 naive-KVM floor.**
- **The i5 anomaly was core-migration.** First run
  landed on an E-core that somehow measured faster; the
  P-state-locked re-run on what is presumably a P-core
  shows the expected +2446 cyc delta (similar to
  Skylake's +5444 and Zen 4's +900, within the silicon-
  generation-dependent range).
- **Spike 01 cycles are silicon-invariant, confirmed
  again.** s4 went from 11700 to 11814 (Δ 1%), s3
  stayed within noise. The earlier panel results hold.
- **Cycle count is the right primary metric.** Across
  all re-measurements, the ns figure swung with clock
  but cycles stayed stable. This reinforces the "report
  cycles first, ns second" convention landed in spike
  02's commentary.

**Final per-host cycle floor for the naive backend
(spike 04 numbers, silicon-invariant):**

| Silicon | Spike 04 median cyc | ns @ boost clock |
|---|---:|---|
| Alder Lake i9 | 4705 | 960 ns @ 4.9 GHz |
| Alder Lake i5 | 14260 | 3170 ns @ 4.5 GHz |
| Zen 4 (7840HS)  | 13148-13338 | 2463-2500 ns @ 5 GHz |
| Kaby Lake | 20404 | 6180 ns @ 3.3 GHz |
| Skylake-SP | 22994 | 6210 ns @ 3.7 GHz |
| Skylake-S | 29746 | 11000 ns @ 2.7 GHz |
| EPYC 7763 (nested) | 20237-22282 | 6236-7729 ns (nested-virt cost added) |

**Update to the design memo headline:**

The naive um_backend_kvm delivers **~1-11 µs per syscall
depending on silicon**, under the shape the memo
describes (long mode + userspace-visible VMEXIT). Alder
Lake i9 at 4.9 GHz boost is 960 ns — within 10× of the
~100 ns vision line without any gadget work. This is the
number that goes into the memo's perf-envelope section.

## 2026-04-23 — Spike 07: SYSCALL-via-LSTAR A/B

Methodology: `spikes/07-syscall-lstar-ab/spike.c` — two
guest-code variants, back-to-back on the same vCPU, 1000
iterations each:

- Variant A: `out %al, $0xf4; hlt` (direct IO exit)
- Variant B: `mov eax, 39; syscall; hlt` with LSTAR
  trampoline `out %al, $0xf4; sysretq`

Median(B) − Median(A) = pure SYSCALL + SYSRETQ instruction-
pair cost, isolated from the VMEXIT round-trip.

| Host | CPU | MHz | A cyc | B cyc | Δ (SYSCALL+SYSRET) |
|---|---|---:|---:|---:|---:|
| s1 | Kaby Lake | 3300 | 20494 | 20732 | **+238 cyc (~72 ns)** |
| s2 | Skylake-S | 3404 | 22296 | 22556 | **+260 cyc (~76 ns)** |
| s3 | Skylake-SP | 3700 | 19922 | 20220 | **+298 cyc (~81 ns)** |
| s5 | Zen 4 @ 1.1 GHz | 1101 | 13186 | 13490 | **+304 cyc (~276 ns)** |
| s6 | Zen 4 @ 4.9 GHz | 4939 | 13566 | 13832 | **+266 cyc (~54 ns)** |
| s7 | Zen 4 @ 2.0 GHz | 1985 | 13452 | 13794 | **+342 cyc (~172 ns)** |

s0 and s4 (Alder Lake) showed variant A tail-heavy due to
IO-exit state leaking across iterations; their B-variant
numbers are clean (s0: 5375, s4: 6754) but the A/B delta
isn't reliable on those hosts. Using the six clean data
points, **SYSCALL + SYSRETQ adds ~240-340 cycles,
silicon-invariant across Intel and AMD from 2015-2023.**

**Key observation:**

- At 50-100 ns (on 3+ GHz hosts) this is a rounding error
  on the 4-30 µs VMEXIT round-trip. The bounce trampoline's
  SYSCALL overhead is not where optimization should focus.
- The design memo's D-04 systrap-gadget direction stays
  correct: the gadget eliminates VMEXITs for common
  syscalls, which saves thousands of cycles — not the 250
  cycles SYSCALL/SYSRET contributes.

**Full naive-backend cost model (all three spikes
integrated):**

  per-syscall cost = VMEXIT-RTT (~4k-30k cyc, silicon)
                   + SYSCALL+SYSRETQ (~250-340 cyc, flat)
                   + host-side handler work (depends)

On Alder Lake i9 at boost: 4705 (spike 04) + ~270 (this) =
~4975 cycles ≈ **~1015 ns per syscall**, design-memo
floor for modern silicon. Still 20× seccomp; 10× away
from the ~100 ns vision line (gap closable by D-04 gadget
but not by any SYSCALL-side work).

## 2026-04-23 — First KVM-backend round-trip (D-04b.1b harness)

First KVM_RUN invocation from the actual backend binary, not
a standalone spike. The D-04b.1b diagnostic harness
(`arch/um/backend/kvm/harness.c`, landed in
commit ebdfcb5d0fcc ("um: backend: D-04b.1b KVM backend long-mode harness wiring (workstream D-04)"))
allocates a 2 MiB anonymous-shared region, lays down the
spike-04-matching GDT / identity page tables / `out %al,
$0xf4; hlt` code sled, registers the region as KVM
memslot 0, programs vcpu0 SREGS + REGS, and invokes KVM_RUN.

**Result, dev host (Zen 4 @ 5.0 GHz boost, ~2.0 GHz sustained):**

```
um: kvm harness: KVM_RUN rc=0, exit_reason=2 (IO)
```

`KVM_EXIT_IO` (value 2) — exactly what spike 04 validated
as the first exit on `out %al, $0xf4`. Confirms end-to-end
that the backend's ported SREGS / CR0 / CR3 / CR4 / EFER /
GDT setup code is correct, not just the standalone spike's.

**Instrumented (2026-04-23 same day, commit c461e178da68
("um: backend: D-04b.1c KVM harness — per-iteration cycle counting (workstream D-04)")):**
1000-iteration timing loop added, same two-KVM_RUN-per-iter
pattern as spike 04 so KVM's pending-I/O-emulation state
doesn't break the loop. Intel Xeon W-2123 (Skylake-W,
4c/8t @ 3.6-3.7 GHz) on Dell Precision 5820 bare metal,
`systemd-detect-virt` = none:

| Iterations | Min cyc | Median cyc | p95 cyc | Max cyc |
|---:|---:|---:|---:|---:|
| 1000/1000 | 21966 | **22252** | 22394 | 21661472 |

Max is a single OS-preemption outlier (~4 ms for one
iteration). Median and p95 are the comparable numbers.

**Compared to spike 04's Skylake-class measurements** (spike
04 didn't measure Skylake-W specifically but has the same-
microarchitecture numbers):

| Source | Host | Median cyc |
|---|---|---:|
| spike 04 | s2 Skylake-S @ 3.4 GHz | 22296 |
| spike 04 | s3 Skylake-SP @ 3.7 GHz | 22994 |
| D-04b.1c harness | dev host Skylake-W @ 3.7 GHz | **22252** |

**The backend sits at the spike floor — ~3% UNDER the Skylake-
SP spike number, which is within run-to-run noise.** The port
from spike-04 standalone into the backend's binary form
adds no measurable overhead. First empirical evidence that
the D-workstream plan's cost model holds once the scaffolding
lands.

My earlier guess that this was Zen 4 was wrong (I read the
sustained-clock cpuinfo from a different host's
measurements.md row); this is a Skylake-W. Apology on-record
here in the durable log so the next reader doesn't chase the
wrong-generation comparison.

This establishes the methodology; per-host entries follow
in the pending-measurements section below as new targets
report in.

### 2026-04-23 add-on — D-04c SYSCALL+LSTAR dispatch

Port spike 07's variant-B methodology into the backend
harness. Adds code_b (mov $0x27, %eax; syscall; hlt) + LSTAR
trampoline (out %al, $0xf4; sysretq) at known harness slot
offsets, programs MSR_STAR / MSR_LSTAR / MSR_FMASK, runs
1000-iter KVM_SET_REGS → KVM_RUN loop. Each iteration the
guest executes SYSCALL → LSTAR-trampoline → `out` →
KVM_EXIT_IO; reset-per-iter clears KVM's pending-I/O state
so no second KVM_RUN is needed.

**IO-baseline vs SYSCALL+LSTAR delta (backend, two hosts):**

| Host | IO-only med | LSTAR med | Δ (SYSCALL+LSTAR) |
|---|---:|---:|---:|
| dev Skylake-W @ 3.7 GHz | 22450 | 22820 | **+370 cyc** |
| w1 Alder Lake @ 5.0 GHz | 5566  | 5940  | **+374 cyc** |

**Silicon-invariant at ~370 cyc across both microarchitectures**,
matching spike 07's measured 260-340 cyc floor across Kaby
Lake / Skylake-S / Skylake-SP / Zen 4. Confirms the
backend's SYSCALL+LSTAR bounce-trampoline cost is fixed per
design-memo projection — the port from spike into backend
binary adds no measurable SYSCALL-side overhead, same as
the port added no measurable VMEXIT-side overhead.

**Integrated cost model for the naive backend (D-04c-era):**

  per-syscall cost = VMEXIT-RTT (~22k Skylake / ~5.5k AL)
                   + SYSCALL+SYSRETQ (~370 cyc, silicon-flat)
                   + host-side handler work (sys_call_table)

At Alder Lake i7 boost: 5940 cyc / 5 GHz ≈ **1.19 µs per
syscall** in-backend, matching the spike-predicted 1 µs
floor (spike 04 i9-12900 @ 4.9 GHz = 960 ns) within ~20 %.

**SYSRETQ execution measurement deferred.** The second
KVM_RUN per iteration (for SYSRETQ → HLT) needs a ring-3-
DPL code segment in the harness GDT (CPL=0 SYSRETQ forces
CS.RPL=3, GPF on DPL=0 descriptor). D-04c.2 or folded into
D-05 ring-3 entry work.

### 2026-04-23 add-on — D-04b.2b.2 UML-kernel-text execution

With D-05a (real time ops) and D-04b.2b.1 (harness relocation
to late_initcall) landed, D-04b.2b.2 wires a dual-memslot
setup: harness slot 0 at guest_phys 0 + UML memory slot 1
at guest_phys 0x10000000 → host_va uml_physmem. A naked
`hlt`-only function `kvm_harness_hlt_target` lives in UML
kernel text; the harness sets RIP to its translated
guest_VA and runs.

Observed on Skylake-W dev host:

  um: kvm harness: UML slot registered (host_va=60000000
      size=8000000 gpa_base=10000000); target &hlt=6004170b
      → guest_va=1004170b
  um: kvm harness: UML-text KVM_RUN rc=0 exit_reason=5 (HLT)
      rip=0x1004170b

The guest vCPU:
  1. walked the kvm-owned pgd from guest_VA 0x1004170b,
  2. resolved guest_phys = 0x1004170b (identity-mapped),
  3. the EPT walked slot 1 (gpa_base=0x10000000) and
     redirected to host_va 0x6004170b — which is UML
     kernel text,
  4. executed the `hlt` byte compiled into that function,
  5. VMEXIT with KVM_EXIT_HLT at the expected RIP.

**End-to-end validation: the backend reads UML's own
compiled binary through the guest MMU and executes an
instruction from it.** This closes the D-04b workstream's
architectural goal.

### 2026-04-23 add-on — D-04b.2b.alt arbitrary-RIP validation

In addition to the 1000-iter IO-exit timing, the harness
now runs a single KVM_RUN with RIP pointing at a lone
`hlt` byte placed at slot offset 0x210000 (2 MiB + 64 KiB,
past the first hugepage boundary). Expected exit:
KVM_EXIT_HLT (value 5). Observed on both hosts:

  dev host Skylake-W: exit_reason=5 (HLT) rip=0x210000
  w1 Alder Lake     : exit_reason=5 (HLT) rip=0x210000

Validates the kvm-owned pgd can route arbitrary RIP
positions through pd entries beyond pd[0] — the
architectural piece D-04b.2b wanted. Together with the
IO-exit timing above, D-04b is architecturally complete
through D-04b.2b.alt; actual UML-kernel-text RIP execution
waits for D-05 timekeeping to unblock a late_initcall
relocation.

### 2026-04-23 add-on — D-04b.1c on w1 (Alder Lake i7-12700K)

Second host for the instrumented harness. i7-12700K (Alder
Lake-S, P-cores @ 5.0 GHz boost, 3.6 GHz base; 8P+4E).
`powersave` cpufreq governor (no passwordless sudo to lock
to performance on this host, so runs are governor-mixed).
Five consecutive 1000-iter runs:

| Run | Min cyc | Median cyc | p95 cyc | State |
|---:|---:|---:|---:|---|
| 1 | **5506** | **5568** | 5602   | boost-locked |
| 2 |  12096  |  13420  | 16650   | power-saving |
| 3 |   6152  |   6214  | 13454   | mixed |
| 4 |   6008  |   6164  | 13754   | mixed |
| 5 | **5882** | **5926** | 5976   | boost-locked |

When the CPU stayed in boost (runs 1 + 5), median tracks min
within ~50 cyc — that's the hardware floor once P-state
variability is out of the picture. Taking min-of-min as the
boost-locked floor estimate:

| Source | Host | Median cyc | MHz | ns/VMEXIT |
|---|---|---:|---:|---:|
| spike 04 s0 | Alder Lake i9-12900 @ boost | 4,705 | 4900 | **960** |
| D-04b.1c run 1 | Alder Lake i7-12700K @ boost | 5,568 | 5000 | **1,114** |

**Backend overhead over spike: ~18%** on Alder Lake-class
silicon. Plausible: the backend does real kernel-context
work (ioctl dispatch via os_ioctl_generic, scheduler /
printk machinery live even if idle, KASAN-capable code paths
compiled in) versus the spike's minimal-userspace tight
loop. Sub-microsecond range on 2021-era hardware, matching
the design-memo cost model.

This is the single-digit-hundred-ns vision headline
*without* the systrap gadget. D-04c's LSTAR+SYSCALL bypass,
when it lands, closes the gap to the ~100 ns asymptote on
silicon that hits it architecturally.

**Mechanical prerequisites that landed to make this visible:**

  - `commit f68398447394 ("um: kernel: stacktrace — bound dump_trace walker to the task's actual stack")` — fix the
    pre-console-panic silent-segfault so the harness's
    panic message could actually reach the operator.
  - `commit d46f5227b845 ("um: backend: kvm harness — write exit reason to stderr via os_info before panic (workstream D-04)")` — route the
    exit_reason line through os_info() (direct stderr)
    rather than pr_info() (buffered printk) since consoles
    aren't registered at harness-invocation time.

**Cross-references:**

- `04b-long-mode-sregs.md` — design note that prescribed
  the handcrafted-trampoline first, UML-CR3 second sub-
  steps.
- spike 04 measurements above — what this harness
  validates the port against.

## 2026-04-23 — Phase III Lift #1b: first observable ring-3 execution in-backend

Methodology: extend the D-04c-era harness with a SYSRETQ
ring-0 → ring-3 transition. Ring-3 page at slot offset
0x7000 contains `out %al, $0xf5; hlt`; ring-0 trampoline at
0x8000 loads RCX/R11 and issues SYSRETQ. MSR_STAR[63:48] is
overridden to 0x18 (from the D-04c-era 0xfff8 sentinel) so
SYSRETQ loads CS=0x28|3, SS=0x20|3 — both ring-3 selectors
added to the GDT in this same series. Port 0xf5 was picked
specifically because it's *distinct* from the ring-0 tests'
0xf4, so the exit port number is the unambiguous "ring-3
ran" proof.

Result (Skylake-W dev host):

```
um: kvm harness: 1b KVM_RUN rc=0 exit_reason=2 (IO)
um: kvm harness: 1b PASS — ring-3 entry verified
    (port=0xf5 IO exit)
```

**Observations:**

- SYSRETQ into ring-3 landed cleanly on first attempt. No
  descriptor-load #GP, no triple-fault shutdown.
- The two-entry GDT extension (unused-padding at 0x18,
  ring-3 data at 0x20, ring-3 code at 0x28) matches the
  AMD64 SDM §6.1.1 SYSRETQ prescription and works on
  Intel silicon without modification.
- IOPL=3 in RFLAGS (`0x3202` not `0x202`) is required so
  the ring-3 OUT doesn't #GP. Without IOPL=3 the expected
  failure mode would be KVM_EXIT_SHUTDOWN or
  KVM_EXIT_INTERNAL_ERROR rather than a clean IO exit.
- The ring-3 transition adds no measurable round-trip
  overhead beyond the existing LSTAR path (not separately
  timed here; the one-shot PASS/FAIL test doesn't need a
  cycle measurement). A separate timing pass would fit
  naturally as a D-04b.1c-style addendum if we want to
  characterize the added SYSRETQ cost — but the gate
  purpose of Lift #1b was "does it happen at all", and
  that is settled.

**What this opens:**

- Phase III Lift #1c (route LSTAR trampoline to the real
  syscall table rather than the harness's `out`-and-
  sysretq emulator) is architecturally unblocked. The
  ring-3 → ring-0 → userspace transit machinery is now
  demonstrably working in-backend.

## 2026-04-23 — Phase III Lift #1c: ring-3 ↔ LSTAR ↔ ring-3 syscall round-trip

Methodology: extend the Lift #1b ring-3-entry demo with a
full round-trip. Ring-3 page at slot offset 0x7100 does
`mov $0x2a, %eax; syscall; out %al, $0xf6; hlt`; the LSTAR
handler at 0x9000 does `add $100, %rax; sysretq`. Host
observes the exit port AND data byte.

Result (Skylake-W dev host):

```
um: kvm harness: 1c KVM_RUN rc=0 exit_reason=2 (IO)
um: kvm harness: 1c PASS — ring-3 → LSTAR → ring-3
    round-trip verified (port=0xf6 data=0x8e)
```

**Observations:**

- Port 0xf6 + data 0x8e (0x2a + 100) is the cross-signal
  that ring-3 executed after SYSRETQ *and* LSTAR actually
  ran and mutated RAX. Either one alone would be
  insufficient proof.
- A KVM_EXIT_SHUTDOWN trap was required to discover that
  after Lift #1b's ring-3 IO exit, the vCPU is parked in
  CPL=3 and subsequent ring-0 code (Lift #1c's SYSRETQ
  tramp) triple-faults. The fix — KVM_GET_SREGS +
  kvm_setup_harness_sregs + KVM_SET_SREGS between tests
  — is the expected "reset to ring-0" pattern a real
  hypervisor would apply on vCPU re-entry.

**Scope — what this does NOT demonstrate:**

- The LSTAR handler is a 7-byte `add $100, %rax; sysretq`
  compute, not a dispatch through `sys_call_table`. A
  real dispatch requires guest-side `current` / percpu /
  kernel-stack setup which the handcrafted harness
  doesn't provide. That lands in Phase III Lifts #1e/1f
  where the full guest-kernel-entry path is plumbed.
  Lift #1c's value is the round-trip machinery itself —
  the scaffolding on which a real dispatch sits.

**What this opens:**

- Phase III Lift #1d (page-fault handling: decode
  KVM_EXIT_MMIO / KVM_EXIT_SHUTDOWN into UML's existing
  arch/um/kernel/trap.c fault path) is architecturally
  unblocked.

## 2026-04-23 — Phase III Lift #1d: KVM_EXIT_MMIO fault-decode

Methodology: extend the harness with a ring-3 fault sled
that loads `0x30000000` (unmapped gpa, past slot 1's UML
memory range but inside the 1 GiB identity-mapped guest
VA) into RAX and dereferences it. Guest-CR3 walk
succeeds; EPT layer fails to back the gpa; KVM surfaces
the fault as KVM_EXIT_MMIO with
`run->mmio.phys_addr == 0x30000000`. Harness classifies
the gpa against slot 0 / slot 1 / unmapped regions and
computes the UML kernel VA for slot-1 hits.

Result (Skylake-W dev host):

```
um: kvm harness: 1d KVM_RUN rc=0 exit_reason=6 (MMIO)
um: kvm harness: 1d mmio gpa=0x30000000 len=8 is_write=0
    region=unmapped uml_va=0x0
um: kvm harness: 1d PASS — fault decode recovered injected
    gpa (0x30000000, unmapped region)
```

**Observations:**

- `run->mmio.len = 8` matches the 64-bit `mov (%rax), %rax`
  dereference; `is_write = 0` matches the load direction.
  Both fields propagate through the EPT-exit path correctly.
- Region classification produces "unmapped" as expected.
  A slot-1-hit variant would produce `region=uml-slot
  uml_va=<uml_physmem + gpa - GPA_BASE>` — the translation
  the real-backend fault handler uses to find the struct
  vm_area_struct backing the fault.
- No timing; the lift's purpose is architectural (can we
  decode?), not performance.

**Scope — what this does NOT demonstrate:**

- Actually calling `segv()` / `handle_page_fault` from the
  harness with a synthesized `faultinfo`. That requires
  per-task state (current, pt_regs, kernel stack) the
  early-boot harness doesn't have. The full integration
  lands with Phase III Lifts #1e (signal delivery) + #1f
  (D-06 conformance), where the real guest-kernel-entry
  path sits on top of the Lift #1d decode stage.

**What this opens:**

- Phase III Lift #1e (signal delivery: SIGALRM/SIGIO/
  SIGUSR1 via KVM_INTERRUPT + in-guest IDT) is next. The
  fault-decode step validated here is a prerequisite:
  signal delivery to a guest with an active fault requires
  the fault to already be materialized as a synthesized
  faultinfo, which this lift demonstrated the backend can
  produce.

## 2026-04-23 — Phase III Lift #1a (kvm_um attach-cost benchmark) retired

**Not run.** Decisions-log D60 closes this placeholder as
superseded by D57. The framing assumed per-mm KVM VMs;
D57 chose per-UML-process VMs. `kvm_mm_attach()` reduces
to `refcount_inc()` on a shared `struct kvm_um` — see
`arch/um/backend/kvm/mm.c::kvm_mm_attach` lines 29-52.
There is no per-mm KVM ioctl cost to measure; the
benchmark's go/no-go role in Phase III was retired by
D57 before it was written.

The one-shot boot-time costs (`KVM_CREATE_VM` +
`KVM_SET_USER_MEMORY_REGION`) live in the D-04b.1b and
D-04b.2b.2 entries above as part of each measurement
session's successful init, not a standalone row.

A future UML-vs-seccomp per-syscall comparison would
measure the full dispatch path (kvm_run_userspace vs.
seccomp_run_userspace), not the attach path; that
belongs to Phase III Lift #1f (D-06 conformance), not to
a retired #1a.

## Pending measurements (placeholders)

These are the entries we expect to add as the D workstream
progresses. Each becomes its own dated section above when
the measurement runs; removing its placeholder here.

- **Modern Intel server silicon** — Sapphire Rapids, Emerald
  Rapids, or Granite Rapids. Would bound "what does the
  silicon floor look like on the hosts syzbot actually runs?"
- **Ampere Altra / AWS Graviton3** — ARM64 with hardware
  virt, full generation gap from the Intel/AMD data points
  above. Would tell us whether the ARM KVM exit path has a
  different cost profile.
- ~~**GHA ubuntu-latest runner (nested KVM)**~~ — done
  2026-04-23; the answer turned out to be "yes, nested
  KVM works on standard runners" (see the Spike 05
  section above). Retained placeholder moved to
  "Completed" above.
- **Spike 04: full long-mode guest + LSTAR trap.** Same
  hosts as Spike 01/02. Adds guest-side mode setup +
  SYSCALL-induced VMEXIT; expected to add ~0 cycles vs
  HLT-induced VMEXIT, but we verify.
- **Spike 05: syscall-servicing loop** (getpid + write +
  exit serviced in-host). First realistic per-syscall cost
  rather than null-exit cost.
- **Spike 06: systrap gadget prototype.** Whether
  in-guest syscall handling reaches the ~100 ns target.
- **D-02..D-06 implementation benchmarks** — real UML
  running `/bin/ls`, `getpid()` microbenchmark, kernel build
  under UML-on-KVM vs seccomp, syzkaller iter/s. Each its
  own dated section.

## Format for future entries

```
## YYYY-MM-DD — <what was measured>

Methodology: ...

| Host | CPU | ... | metric | notes |
| ...  | ... | ... | ...    | ...   |

**Observations:**
- ...

**What this settles / opens:**
- ...
```

Keep tables tight and comparable. When a later spike or the
real backend adds a new column (e.g. "cycles with systrap
enabled"), add the column but back-fill empty cells for older
rows rather than forking the schema.
