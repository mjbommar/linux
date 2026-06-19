# UML — vDSO coarse-clock page: ~2,186× (the real ceiling, no trap)

**Date:** 2026-06-19
**Patch:** `upstream-patches/2026-06-19-um-vdso-coarse-clock.patch` (104 lines,
4 files). Source reverted after validation.

The clock gadget removed the *handoff* from clock reads (~32,000 → ~8,500 cyc) but
still pays the SIGSYS signal-delivery floor, because the read still traps. The
real ceiling is **not trapping at all** — what mainline Linux does with a vDSO
that reads a shared `vvar` page. UML's vDSO deliberately syscalls everything and
maps no data page. This adds the data page for the **coarse** clocks.

## Mechanism

- `arch/x86/um/vdso/vma.c`: allocate a second page (the data page) and map it
  immediately **below** the vDSO code page (`[data][code]`, ELF base unchanged at
  `um_vdso_addr`). Add `um_vdso_update_coarse()`.
- `arch/um/kernel/time.c`: call it from `timer_handler` each tick (gated on
  `TT_MODE_OFF`) — publishes `ktime_get_coarse_ts64` + `_real_` as sec/nsec under
  a seqlock.
- `arch/x86/um/vdso/um_vdso.c`: `__vdso_clock_gettime(CLOCK_MONOTONIC_COARSE |
  CLOCK_REALTIME_COARSE)` finds the data page (RIP & ~0xfff − PAGE_SIZE), reads
  the snapshot under the seqlock, returns — **no syscall, no trap.** Precise
  clocks fall through to the syscall (the gadget handles those).

Coarse clocks are *defined* at tick resolution, so a per-tick snapshot is exact —
no TSC calibration, no division, no rdtsc. Hugely common in real code (glibc
`time()`, logging, monitoring, anything wanting cheap timestamps).

## Results (vs baseline, 1 CPU)

| `clock_gettime(CLOCK_MONOTONIC_COARSE)` via glibc vDSO | baseline | vvar |
|---|---|---|
| per call | 34,980 cyc (full trap) | **16 cyc** |
| 2,000,000-call loop | ~19 s | **<1 s** |

**~2,186×.** This is by far the largest win of the trap-path investigation,
because it removes the trap *entirely* for the coarse-clock class.

## Correctness + regression (all on the vvar kernel)

- **Coarse vs precise:** skew **6 ms** (inside the 10 ms HZ=100 tick resolution —
  exactly coarse semantics).
- **Tracks real time:** over a 2 s sleep, coarse advanced **exactly 2000 ms**.
- **Monotonic:** confirmed advancing.
- **cpython-test: SUCCESS.** **tls-test: PASS** (32-thread). The new 2-page vDSO
  (mapped into *every* process) breaks nothing.

## Disposition

The strongest, cleanest win in the series, and the most upstreamable: small,
correct, validated, regression-clean, and it's literally how mainline does
clocks. The hierarchy of clock optimizations is now complete:

| clock read | cost | mechanism |
|---|---|---|
| precise, baseline | ~32,000 cyc | full trap + handoff |
| precise, gadget | ~8,500 cyc | in-stub, removes handoff (still traps) |
| **coarse, vvar** | **~16 cyc** | vDSO reads page, no trap |

Natural follow-on: extend the vvar to **precise** MONOTONIC/REALTIME with a TSC
mapping (invariant TSC is present) — that would bring precise clocks to ~30–50 cyc
too, the full mainline-vDSO design. Larger (TSC calibration + the mult/shift
math) but the coarse page proves the infrastructure works end to end.
