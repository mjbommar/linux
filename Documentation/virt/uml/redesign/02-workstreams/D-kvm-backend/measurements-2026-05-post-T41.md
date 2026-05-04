# UML backend timing — post-T41 update (2026-05-03)

This memo refreshes the cross-backend cyc/getpid table after the
SMP-T41 root-cause fix landed (`af659ad4297d`) and after the v1
KVM backend was archived.

## What changed since `measurements.md`

The original lab table (`measurements.md` lines 1672-1693) was
captured **2026-04-22..2026-04-24** against the **v1 KVM backend**
("gadget" + "fallback" columns). Two structural changes since then
make the original table partly obsolete:

1. **v1 KVM backend was archived** (memo 25 R10/R11) and removed
   from the active build. The `kvm-fallback` and `kvm-gadget`
   columns no longer reflect anything that ships. Treated as
   historical reference only.
2. **ptrace backend was deleted entirely** (memo 25 R11,
   commit `06c88545ae2c`, ~600 LoC removed from
   `arch/um/backend/ptrace/`). The `ptrace` column is unmeasurable
   on the current tree.

The active backend for v2-era measurement is **`kvm-v2`** — a
fundamentally different design (per-host-CPU vCPU pool, per-syscall
KVM_RUN dispatch). Its trap-mechanism cost is in a different class
than v1's gadget (87 cyc) but well below v1's fallback (~83000 cyc
on Zen 4).

## Methodology

Same `tools/testing/selftests/um/perf-getpid/getpid-loop` static
binary as the original measurements. Boot UML with the binary as
`init=`. The binary issues N=100000 `SYS_getpid` calls,
rdtsc-bracketed, and writes one summary line:

    PERF_GETPID: n=100000 cycles=<T> ns=<NS> ns_per_call=<NPC>
                 cyc_per_call=<CPC> sink=<S>

The **only difference** vs `measurements.md`'s methodology is
the wrapper: today's `umlctl up --file <toml>` boots UML rather
than the original `run-perf-getpid.sh`. The `getpid-loop` binary
itself is unchanged. Cycle counts are directly comparable across
the two harnesses (rdtsc reads the same TSC).

The host-side `run-perf-getpid.sh` script was designed for v1's
3-backend matrix (`ptrace seccomp kvm`); it returns exit-code 4
on builds where v1 is absent (today's tree). The direct-boot path
sidesteps that.

## Host inventory (May 2026)

Same physical hosts as `measurements.md` lines 17-27, with one
addition. **server7** is the host this run was captured on; the
slug `s7` in the original `measurements.md` table referred to a
**different physical Ryzen 7840HS host** (one of three same-CPU
hosts in the lab). Today's measurement names this physical host
`server7` to disambiguate.

| Tag      | CPU                       | µarch        | C/T        | Notes                                      |
|----------|---------------------------|--------------|------------|--------------------------------------------|
| dev / s3 | Xeon W-2123 @ 3.60 GHz    | Skylake-SP   | 4C/8T      | Primary dev workstation; twin of `s3`.     |
| s0       | i9-12900K                 | Alder Lake P | 8P+8E/24T  | Fastest in inventory.                      |
| s1       | Xeon E3-1225 v6           | Kaby Lake    | 4C/4T      | Server-class no-SMT.                       |
| s2       | Xeon E3-1225 v5           | Skylake-S    | 4C/4T      | Oldest silicon in inventory.               |
| s4       | i5-12600K                 | Alder Lake   | 6P+4E/16T  | Modern desktop non-i9.                     |
| s5       | Ryzen 7 7840HS            | Zen 4        | 8C/16T     | AMD APU; 2026-04-23 measured power-save.   |
| s6       | Ryzen 7 7840HS            | Zen 4        | 8C/16T     | Same chip as s5/s7; 2026-04-23 boost.      |
| s7       | Ryzen 7 7840HS            | Zen 4        | 8C/16T     | Same chip; 2026-04-23 power-save.          |
| **server7** | **Ryzen 7 7840HS**     | **Zen 4**    | **8C/16T** | **Active dev host as of 2026-05-03; performance gov + ~5 GHz boost.** |

## Cross-backend cyc/getpid — post-T41 (2026-05-03)

**Captured on server7** (the only host this turn could measure):

- **CPU**: AMD Ryzen 7 7840HS (Zen 4)
- **Governor**: `performance`
- **Run-time clock**: ~5004 MHz boost (per `/proc/cpuinfo`)
- **Host kernel**: 7.0.0-15-generic + custom kvm.ko/kvm-amd.ko
- **glibc**: 2.43-2ubuntu2 / **gcc**: 15.2.0
- **3 runs per cell, median reported, ≤1% variance across runs**

| Build / backend                                 | cyc/getpid | ns/getpid | vs seccomp (same build) |
|-------------------------------------------------|----------:|----------:|------------------------:|
| **kvm-v2 UP** (uml-clean `ad18db7c3768`)         | **24 484** | **6 455** | **0.39× (~2.5× faster)** |
| **kvm-v2 SMP** (uml-smp-t41fix `b1e0a3bb3673` + T41) | **36 808** | **9 704** | **0.49× (~2.0× faster)** |
| seccomp UP (same `uml-clean` kernel)            | 62 322    | 16 425   | 1.00× (reference)       |
| seccomp SMP (same `uml-smp-t41fix` kernel)      | 74 903    | 19 748   | 1.00× (reference)       |
| ptrace                                          | (n/a — backend removed in `06c88545ae2c`) | | |
| kvm-v1 fallback                                  | (n/a — backend archived in memo 25 R10) | | |
| kvm-v1 gadget                                    | (n/a — same archive) | | |

### Headline results

- **kvm-v2 is 2.0× faster than seccomp** on per-syscall round-trip on
  this host (SMP build). The advantage **widens to 2.5×** on the UP
  build.
- **SMP-build overhead** is real: kvm-v2 pays +50% per-syscall
  (24 484 → 36 808 cyc, UP→SMP), seccomp +20% (62 322 → 74 903).
  The kvm-v2 SMP overhead is mostly migrate_disable + per-mm
  tlb_gen accounting + always-`KVM_GET_FPU` (T26/T27).
- **Python startup ratio (0.83) is much weaker than per-syscall
  ratio (0.49)** because `python3 -c "import math; print(...)"`
  is dominated by boot-init scaffolding, exec, libc/libdl
  initialization, and printk-timestamp granularity (10 ms),
  none of which scale with the trap-mechanism cost.

## Comparison vs the historical (v1-era) Ryzen 7840HS rows

`measurements.md` lines 1678-1681 (2026-04-23, v1 build):

| Tag | ptrace | seccomp | kvm-fallback | kvm-gadget |
|-----|-------:|--------:|-------------:|-----------:|
| s5  | 40 150 | 29 093 | 82 584       | 87         |
| s6  | 40 580 | 29 768 | 81 556       | 89         |
| s7  | 40 868 | 29 747 | 83 379       | 88         |

vs today (`server7`, 2026-05-03, post-T41 v2 build):

| Backend                                    | Then (v1, UP) | Now (v2-era, UP) | Delta |
|--------------------------------------------|--------------:|-----------------:|------:|
| seccomp                                    | 29 093 cyc    | 62 322 cyc       | **+2.14× slower** |
| kvm-v2 (vs v1 fallback, the closest analogue) | 82 584 cyc | **24 484 cyc**   | **−3.4× (3.4× faster)** |
| kvm-v2 (vs v1 gadget — different design)   | 87 cyc        | 24 484 cyc       | **+281×** (gadget stays in guest; v2 doesn't — apples vs oranges) |

Two big shifts:

- **seccomp regressed +2.1× on the same chip** between 2026-04-23
  and 2026-05-03. Independent of v2 (seccomp doesn't run any v2
  code path). Suspect causes: cumulative hot-path additions in
  the SMP-track work (e.g., `mm/mmu_gather.c::tlb_batch_pages_flush`
  changes, `arch/um/kernel/tlb.c` per-mm tlb_gen, the umlctl-deploy
  branch's signal-handling refactors that touch the seccomp trap
  loop). **Tracked as a follow-up to SMP-T55.**
- **kvm-v2 fixed the v1-fallback's pathological cost**. v1's
  fallback path (the "naive heavy-vCPU" baseline) was 82 584 cyc
  on Zen 4. v2's full-fidelity dispatch is **24 484 cyc** — 3.4×
  faster than v1 fallback while retaining v1's syscall ABI and
  adding SMP. v2 doesn't and won't reach v1's gadget cost (87 cyc):
  the gadget is a fundamentally different design that stays in the
  guest for whitelisted syscalls; v2 always vmexits.

## What still needs measuring

To complete the lab table for 2026-05-03 against v2:

- [ ] `dev` / `s3` (Xeon W-2123 / Skylake-SP)
- [ ] `s0` (i9-12900K / Alder Lake P)
- [ ] `s1` (Xeon E3-1225 v6 / Kaby Lake)
- [ ] `s2` (Xeon E3-1225 v5 / Skylake-S)
- [ ] `s4` (i5-12600K / Alder Lake)

For each:

  1. `git checkout b1e0a3bb3673` (or current tip).
  2. `make ARCH=um O=$BUILD defconfig && make ARCH=um O=$BUILD -j$(nproc)`
     for the UP build, OR clone the SMP `.config` from
     `~/src/uml-builds/uml-smp-t41fix/.config`.
  3. `cpupower frequency-set -g performance` (refused by the
     canonical `uml-perf-capture.sh` otherwise).
  4. Boot `kvm-v2` with `init=getpid-loop` (1-CPU pinned), capture
     `PERF_GETPID:` line. 3 runs.
  5. Boot `seccomp` same kernel, capture. 3 runs.
  6. Append a row here.

## Per-host baseline file

A new informational JSON for `server7` lives at
`Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/`
`perf-baseline-ryzen-7-7840hs.json`. Schema follows the existing
`perf-baseline-xeon-e3-1225-v6.json` convention but is **flagged
as informational** because:

- It captures `kvm-v2` and `seccomp` only (no ptrace/v1-kvm rows
  to compare).
- It uses `getpid-loop` cyc/getpid as the unit, not the
  `uml-perf.sh` cycles+instructions+task_clock triple.
- It's directly captured (not via the canonical
  `uml-perf-capture.sh`) — the canonical wrapper assumes v1's
  3-backend matrix.

**Future work**: extend `uml-perf-capture.sh` to detect `v2`
backends + emit the new schema. Tracked as part of SMP-T55.

## Cross-link to other tables

- **Historical v1-era cross-host table**: `measurements.md` §
  "Backends — full measurement (post-round-6 audit closure)".
- **v1 gadget design rationale**: `02-categories.md` (`run_userspace`
  is HOT, "100 ns KVM target" — that target was for the gadget
  layer, NOT v2's vCPU dispatch).
- **Why v1 was archived**: memo 24 §"10 clean-slate items",
  memo 25 R10/R11.
- **v2 fix stack** (correctness, not perf): SMP-T25 / T26 /
  T27 / T29 / T33 / T36 / T37 / T41 — see
  `state-audit/21-smp-t41-pf-stub-rax-recovery-FIXED.md` for
  the full list and `STATUS.md` for the current state of each.

## Related ratios from the python-startup gate

For end-to-end Python startup wall-clock (different metric — boot
+ init + python + exit, dominated by non-trap costs):

| Build / backend          | Median wall-clock | Ratio vs seccomp |
|--------------------------|------------------:|-----------------:|
| kvm-v2 UP                | 0.04 s (04-30 baseline) ¹ | 0.44× |
| kvm-v2 UP                | 0.06 s (today)            | 0.67× |
| kvm-v2 SMP T41-fix       | 0.10 s                    | 0.83× |
| seccomp UP (04-30)       | 0.09 s                    | 1.00 |
| seccomp UP (today)       | 0.09 s                    | 1.00 |
| seccomp SMP (today)      | 0.12 s                    | 1.00 |

¹ The 04-30 → today UP-build python-startup widening (0.44 →
0.67) is the SMP-T55 follow-up: cumulative correctness fixes that
required draining pending signal/scheduler work after every
syscall (`bd435856948e` + others). Not a single defensive guard
that can be relaxed without regressing substrate parity.
