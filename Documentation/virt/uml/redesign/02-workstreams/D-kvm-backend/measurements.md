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
- **GHA ubuntu-latest runner (nested KVM)** — baseline for
  "can D's CI story use standard runners?" Probably not
  (runners are Hyper-V guests, nested typically off), but
  the measurement is worth capturing so the failure mode
  is diffable not re-debated.
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
