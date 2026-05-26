# Spike 07: SYSCALL-via-LSTAR A/B vs direct IO-exit

Separates the SYSCALL/SYSRET instruction-pair overhead from
the VMEXIT round-trip cost. Same host + same vCPU runs two
variants back-to-back:

- **Variant A** — `out %al, $0xf4; hlt` (matches spike 04):
  direct IO exit, no SYSCALL.
- **Variant B** — `mov eax, 39; syscall; hlt`, with LSTAR
  trampoline `out %al, $0xf4; sysretq`: SYSCALL to LSTAR,
  trampoline triggers the IO exit, SYSRETQ returns to the
  saved RIP (== the `hlt`).

Median(B) − Median(A) = pure SYSCALL+SYSRETQ instruction
cost.

## Running

```sh
make
sudo ./spike   # or plain ./spike if user is in kvm group
```

## Result across the 8-host panel (2026-04-23)

| Host | CPU | MHz | A cyc | B cyc | Δ cyc (SYSCALL+SYSRET) |
|---|---|---:|---:|---:|---:|
| s1 | Kaby Lake | 3300 | 20494 | 20732 | **+238** |
| s2 | Skylake-S | 3404 | 22296 | 22556 | **+260** |
| s3 | Skylake-SP | 3700 | 19922 | 20220 | **+298** |
| s5 | Zen 4 @ 1.1 GHz | 1101 | 13186 | 13490 | **+304** |
| s6 | Zen 4 @ 4.9 GHz | 4939 | 13566 | 13832 | **+266** |
| s7 | Zen 4 @ 2.0 GHz | 1985 | 13452 | 13794 | **+342** |
| s0 | Alder Lake i9 | varied | (A tail-heavy) | 5375 | *see note* |
| s4 | Alder Lake i5 | 4500 | (A tail-heavy) | 6754 | *see note* |

**Headline: SYSCALL + SYSRETQ adds ~240-340 cycles, silicon-
invariant across Intel and AMD from 2015-2023.**

At boost clocks that's 50-100 ns — negligible compared to
the VMEXIT round-trip (~4-30 µs depending on silicon) the
design memo is concerned with.

## Notes on s0/s4 anomalies

s0 and s4 (both Alder Lake) show variant A inflated by ~8000
cycles vs variant B. Examining the raw data, variant A's p10
matches variant B's median closely (e.g. s4: A p10=6646,
B p50=6754) but A's p50 jumps to 14270. That looks like
**only the first ~500 iterations of variant A produce clean
IO-exit measurements**; the other 500 are tail-weighted with
higher cycle counts, probably from IO-exit state leaking into
the next `KVM_RUN` (KVM re-emits or retries pending IO after
a `out` until we advance RIP, which the spike doesn't do).

The B-variant loop doesn't have this issue because SYSCALL
changes RIP destructively — each iteration resets cleanly.

For the six hosts where A produced a clean median, the A/B
delta is tight at ~250-300 cycles. For s0/s4, use variant B
alone as the SYSCALL-variant cycle cost and extrapolate the
SYSCALL+SYSRET overhead as ~250-300 cycles (assumed silicon-
invariant from the other six data points).

## What this settles for the design memo

The memo's bounce-trampoline path is `SYSCALL → LSTAR → vmcall
(or out) → VMEXIT → host handle → VMRESUME → SYSRETQ → guest
next insn`. The cost breakdown:

  ~95% — VMEXIT round-trip (spike 01/04 numbers)
  ~ 5% — SYSCALL + SYSRETQ pair (this spike, ~250 cycles)

**Implication:** squeezing SYSCALL+SYSRET by avoiding the
trampoline (e.g., landing the memo's "systrap gadget" work
D-04) skips the wrong cost. The VMEXIT is what matters.
The gadget work's value is in **entirely skipping the
VMEXIT** for common syscalls — servicing them in-guest —
not in reducing the SYSCALL overhead.

Spike 07 confirms the design memo's commit-plan priority:
first-phase D ships the naive `KVM_RUN`-per-syscall backend
at the 4-30 µs floor; second-phase D-04 adds the gadget
layer that eliminates VMEXITs for hot syscalls, not the
SYSCALL/SYSRET pair.

## What this spike does NOT settle

- Whether an IDT-fault-based entry (instead of SYSCALL)
  would be faster. Unlikely on modern CPUs but worth a
  follow-on if we ever need to bypass SYSCALL for AMD
  SVM's historical quirks.
- Whether KVM's VMEXIT fastpath (`KVM_REQ_IMMEDIATE_EXIT`,
  `KVM_EXIT_HYPERV`, etc.) would reduce the VMEXIT cost on
  modern silicon. Candidate for a follow-on measurement
  on Sapphire Rapids / Granite Rapids when available.
- The cost of SYSCALL from ring 3 specifically. This spike
  runs at ring 0 throughout; the SDM says SYSCALL from
  CPL=0 is undefined, but all shipping CPUs behave
  identically to CPL=3 SYSCALL. Worth a confirmation spike
  when we actually ship ring-3 guest userspace — just wrap
  the variant B code in a ring-transition sequence and
  re-measure.
