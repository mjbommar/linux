# UML — seccomp in-stub gadget fast path (O7): empirical results

**Date:** 2026-06-18
**Method:** built experimental kernels (getpid gadget + clock gadget) from the
current tree; measured against baseline on the `perf-getpid`-style bookends and a
real python workload. Single-CPU pinned. Source reverted after measurement
(validated experiments, not shipped — same discipline as the O1 work).

Follow-up to `2026-06-18-seccomp-trap-path-optimizations.md` O7. That doc
proposed handling stateless syscalls in the stub without the futex handoff; this
one **builds it and measures it**, and finds the real workload lever.

---

## Mechanism

The seccomp stub's `SIGSYS` handler (`stub_signal_interrupt`) normally captures
the trap and futex-hands-off to the UML kernel thread (~32,000 cyc round-trip,
~48% of which is the handoff itself). The gadget intercepts chosen syscalls *in
the stub* before the handoff: compute/fetch the result, write it into the
trap's `mcontext` RAX, and return — `rt_sigreturn` resumes the guest with the
result. No futex, no schedule, no UML kernel thread. (Same idea as the kvm-v2
LSTAR gadget, applied to the seccomp backend.)

## Result 1 — getpid gadget: 4.5×, correct, but NOT a workload lever

UML stamps the guest TGID into the shared stub page on the first crossing
getpid (`handle_syscall`); the stub answers subsequent getpids from it.

- **getpid micro: 31,800 → 7,030 cyc/call (−78%, 4.5×)**, 3 reps. `sink`
  correctness check intact (pid=1 → sink=101000).
- **cpython-test: SUCCESS, wall 33.5 → 32.9 s (flat).** glibc caches getpid in
  userspace, so real programs rarely trap it — the microbench win does not
  translate (the O1 lesson again).
- **Correctness caveat:** a per-mm cached TGID is correct for `CLONE_THREAD`
  (threads share the TGID) but wrong for `CLONE_VM`-without-`CLONE_THREAD`
  (vfork / `clone(CLONE_VM)` — different TGID, shared mm). Production needs an
  mm-occupant gate (disable when the mm hosts tasks of differing TGID). Rare
  (vfork children usually exec immediately), but real.

## Result 2 — clock gadget: 4× on a REAL workload (the lever)

**Key discovery:** UML's vDSO (`arch/x86/um/vdso/um_vdso.c`) deliberately turns
every `clock_gettime`/`gettimeofday`/`time` into a syscall — its own comment:
*"This vDSO turns all calls into a syscall so that UML can trap them."* Real
Linux reads a shared page (no syscall). So **every clock read in a UML guest
pays the full ~32,000-cyc trap** — and clock reads are frequent in real code
(logging, asyncio, datetime, DB/web timestamps).

Gadget (perf-ceiling build: stub forwards `clock_gettime` to the host clock):

- **clock_gettime micro: 32,318 → 8,541 cyc/call (−74%, 3.8×).**
- **python 2,000,000 `time.monotonic()`: 19.36 → 4.89 s (−75%, 4.0×).**

This is a genuine **workload-level** win, unlike getpid — because the UML vDSO
forces the trap and the calls are hot.

### Correctness gap + the viable correct version

The perf-ceiling build forwards to the *host* monotonic clock, which differs
from the guest's by the boot offset. Measured directly: baseline guest
`CLOCK_MONOTONIC` = **0 s** (just booted); gadget (host) = **1,318,991 s** ≈ host
uptime. So the offset is `host_uptime_at_guest_boot` — **large but a stable
constant**. The correct version is therefore cheap and preserves the win:

1. UML publishes `mono_offset = os_nsecs() - ktime_get_ns()` (and the realtime
   analogue) into the stub page — a near-constant, refreshed opportunistically.
2. Stub: `guest_clock = host_clock_gettime(id) - offset` — trivial arithmetic,
   so the ~4× perf win is retained.
3. Gate **off** under time-travel (the guest clock is virtual then — same gating
   kvm-v2 already applies to its gadget), and only gadget clock-ids with a known
   offset (MONOTONIC/MONOTONIC_RAW/REALTIME); fall through to the handoff for the
   rest.

The proper end state mirrors real Linux: a UML **vvar time page** the vDSO reads
with *no syscall at all* (~32,000 → ~50 cyc, not just handoff removal). That is
larger (timekeeping update path, time-travel interaction) but is the real
ceiling; the stub gadget is the tractable first step that already gets ~4×.

---

## Disposition

- **getpid gadget:** mechanism proven (4.5×, correct), but not a workload lever
  (glibc caches). Low priority on its own; useful as the gadget scaffold.
- **clock gadget:** the **real workload lever** (~4× on clock-bound code), and
  high-value because UML's vDSO makes *all* clock reads trap. Worth building
  correctly: stub gadget with published offset (near-term), vvar page (end state).
- This is the first lever in the whole trap-path investigation that moves a real
  workload — O1/O2/O9 were edge micro-opts; the gadget removes the crossing
  entirely for the syscalls that matter. Confirms the proposal's ranking: attack
  the crossings (O7) and the handoff (O4), not the trap's edges.

**Discipline note:** both gadgets are validated experiments, not shipped — the
clock build intentionally returns wrong monotonic values to isolate the perf
ceiling, and the correct version (offset + time-travel gate + per-id handling)
plus the getpid mm-occupant gate are specified above for a proper patch cycle.
