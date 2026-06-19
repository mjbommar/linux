# UML — O1 made correct, and stacking with the clock gadget

**Date:** 2026-06-19
**Method:** built + measured experimental kernels from current tree; source
reverted after measurement. Patch: `upstream-patches/2026-06-19-seccomp-o1-plus-clock-gadget.patch`.

Follow-up to the clock gadget. A guest-side syscall census first established
*where* the remaining cost is, then O1 was implemented correctly and shown to
stack with the clock gadget.

## Census — what real workloads actually trap

`strace -f -c` *inside the guest* on a mixed workload (file reads + loopback
sockets + clock + compute). Top trapping syscalls: `clock_gettime` (gadgeted),
then `fstat`/`lseek`/`read`/`openat`/`close` (file I/O) and
`socket`/`connect`/`sendto`/`recvfrom`/`accept4` (sockets). **Every hot syscall
except clocks needs the kernel thread** — not gadgetable. So beyond clocks, the
only lever is reducing the per-crossing handoff cost, which O1 does.

## O1 — correct single-thread-mm gate (was an upper bound, now real)

`stub_signal_interrupt` does `arch_prctl(GET_FS)`+`GET_GS` every trap to catch
untrappable `wrfsbase`. UML now stamps `mm_single_threaded` (from
`handle_syscall`, `mm_users == 1`); the stub skips the GET when set. A `clone()`
that shares the mm bumps `mm_users` and crosses, re-stamping 0 before the new
thread runs.

- **getpid: 31,985 → 28,720 cyc (~10%)**, the full arch_prctl cost recovered.
- **correctness:** single-thread `wrfsbase` SURVIVED; **4×CLONE_THREAD wrfsbase
  across switches: clob=[0,0,0,0] CLEAN** (the gate keeps MT correct).
- **workload:** 500k `os.stat` (syscall-bound, single-threaded) 5.60 → 5.24 s
  (**~6.5%**). Real but modest — arch_prctl is ~10% of each trap.

## Stacking O1 + clock gadget

Combined build (both opts). One bug found + fixed along the way: a lost
`stub_exe.c` filter edit dropped `clock_gettime` from the stub allowlist, so the
gadget's `clock_gettime` was seccomp-killed ("lost MM child"). Re-adding the
filter entry fixed it — **not** a fundamental conflict; they target disjoint
costs.

Combined results (vs baseline):
| workload | baseline | combined | speedup | source |
|---|---|---|---|---|
| getpid | 31,985 cyc | 29,453 cyc | ~8% | O1 |
| clock_gettime | 32,228 cyc | 8,628 cyc | 3.7× | gadget |
| **logging 200k** (clock+syscall) | 10.16 s | **5.74 s** | **1.77×** | both (beats gadget-alone 6.0s) |
| stat 500k (syscall only) | 5.60 s | 5.24 s | ~6.5% | O1 |

Correctness on combined: MONOTONIC monotonic + 50 ms-sleep delta exact, no
host-time leak; MT-wrfsbase CLEAN; **cpython-test SUCCESS rc=0**.

## Disposition

Two correct, validated, stackable in-tree optimizations:
- **Clock gadget** — the broad workload lever (~3.9× clock-bound; UML's vDSO
  forces every clock read to trap).
- **O1** — ~6–10% on every crossing syscall for single-threaded processes,
  helps the syscall-bound remainder the gadget can't.

Together: clock-bound code ~3.9×, syscall-bound ~6%, mixed (logging) ~1.8×.
Source reverted; both carry the same hardening notes as before (clock: REALTIME
re-stamp budget + observational-equivalence audit; O1: already validated).
Remaining big lever is still the handoff schedule itself (O4, out-of-tree).
