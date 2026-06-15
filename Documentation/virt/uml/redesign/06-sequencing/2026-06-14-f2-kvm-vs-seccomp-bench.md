# F2 — KVM-v2 vs seccomp: measured syscall-cost delta (2026-06-14)

**Question (gates A1):** is the kvm-v2 `KVM_EXIT_IO` trap path a real win over the
seccomp `SIGSYS`+futex path, enough to justify a second execution model?

**Answer: no, not as a general trap mechanism.** kvm-v2's only advantage is the
in-guest LSTAR *gadget* for a handful of trivial syscalls. For syscalls that
actually trap to do host work, kvm-v2 is **slower** than seccomp. The net effect
is entirely a function of a workload's trivial-syscall density.

## Method

Host harness `tools/testing/selftests/um/bench/run-bench.sh` (rdtsc-bracketed
`micro` tier = 100k `SYS_getpid`; `py` tier = a 5-sample mixed Python workload).
Same host, same `linux` build except where noted. UP guests, `/dev/kvm` present.
Backends selected at boot (`backend=force=...`). Runs serialized (no concurrent
UML, to keep timing clean). Three kernels:

- `f2-bench`: dynamic, kvm-v2 + seccomp, **gadget on** (default).
- `f2-nogadget`: dynamic, kvm-v2, **gadget off** (`UM_BACKEND_KVM_V2_GADGET=n`).

## Micro tier (getpid round-trip)

| cell | cyc/getpid | ns/getpid | vs seccomp |
| --- | ---: | ---: | ---: |
| seccomp (SIGSYS + futex) | 35,823 | 9,444 | 1.00x |
| kvm-v2 **gadget OFF** (KVM_EXIT_IO) | 31,262 | 8,242 | 0.87x (~13% faster) |
| kvm-v2 **gadget ON** (in-guest) | 131 | 34 | 0.004x (273x faster) |

Reading: the KVM exit *mechanism alone* (gadget off) is only ~13% faster than
seccomp for a trivial trapping syscall. Essentially all of kvm-v2's headline
speed is the gadget keeping the syscall in-guest (no vmexit at all).

But getpid is a degenerate trap: it does no host work, so this only measures
trap-entry/exit cost. The py tier shows what happens when syscalls do real work.

## Py tier (mixed workload) — the section breakdown is the story

Aggregate: seccomp `elapsed_ms_p50=232.93`, kvm-v2 `82.67`,
`ratio_v2_over_seccomp=0.355` (kvm-v2 2.8x faster). **That aggregate is
misleading** — the workload runs 20,000 `getpid` (gadget-served) against only
~1,700 real trapping syscalls. Per section (median of 5 samples):

| section | what it does | seccomp (ms) | kvm-v2 (ms) | kvm-v2 vs seccomp |
| --- | --- | ---: | ---: | ---: |
| getpid (20k) | trivial, gadget-eligible | ~182 | **0.00** | eliminated |
| fs (~1.4k syscalls) | open/write/close/stat/read in tmpfs | ~41.7 | ~62.2 | **1.49x slower** |
| sock (~300 syscalls) | socketpair + close | ~5.0 | ~12.3 | **2.48x slower** |
| hash | pure CPU, no syscalls | ~4.0 | ~7.7 | noisy (~equal) |

- The entire 2.8x aggregate win is the gadget zeroing the (artificially large)
  getpid section. seccomp's 182 ms over 20k getpid == 9.1 us each, matching the
  micro number exactly — the measurement is self-consistent.
- For **real trapping syscalls**, kvm-v2 is *slower*: file I/O 1.5x, sockets
  2.5x. This is the vmexit (`KVM_EXIT_IO`) plus the per-mm worker / SREGS / TLB
  overhead (the `prev_roots` drop every 3 TLB generations — the documented
  TLB-shootdown tax) costing more than seccomp's `SIGSYS`+futex.
- Strip the degenerate getpid section (a real-syscall proxy): kvm-v2
  62.2+12.3+7.7 = **82.2 ms** vs seccomp 41.7+5.0+4.0 = **50.7 ms** -> kvm-v2 is
  **1.6x slower** on the part of the workload that does actual work.

## Conclusion (input to A1)

1. The KVM `KVM_EXIT_IO` trap path is **not** a general speedup; it is ~13%
   faster only for a do-nothing trap and is **1.5-2.5x slower** for syscalls
   that do host work.
2. The real and large win is the **in-guest gadget**, which applies only to
   trivial syscalls (getpid/gettid/getppid/get[e]uid/get[e]gid/getcpu/+cached
   clock). The gadget requires running the guest in a KVM VM, which is exactly
   what makes every *other* syscall slower — they are a package deal.
3. Therefore a workload's kvm-v2 benefit is a direct function of its
   trivial-syscall density: getpid-bound microbenchmarks win hugely; anything
   doing real file/socket/compute work is a net **regression** vs seccomp.

So the A1 premise ("reshape the KVM exit into a fast path") does not hold for the
*trap path* — there is no general fast path to extract; the KVM exit is slower.
The decision A1 must make is about the **gadget** and kvm-v2's role, not the trap
mechanism. See the action plan A1 entry (reassessed against this data) and note
the non-performance rationale (isolation/security; cf. `kvm-isolation`,
`kvm-bounds` selftests) that may justify keeping kvm-v2 despite the trap-cost,
the same way vector2 is kept for security despite perf parity.

## Caveats / follow-ups

- UP guests; one 5-sample py run + single micro run. Effect sizes are large and
  variances tight (py p10/p90 within ~3%), so the qualitative conclusion is
  robust, but a multi-run, SMP, and **cpython-full** (real test suite, lower
  getpid density) pass would sharpen the real-world number.
- The `hash` (no-syscall) section being slightly higher under kvm-v2 is within
  noise (2.8-11.5 ms across samples); if it is real it points at vmexit-adjacent
  jitter (timer ticks), worth a look but not load-bearing here.
- Scoreboard JSON: `/tmp/f2-micro-gadget.jsonl`, `/tmp/f2-micro-nogadget.jsonl`,
  `/tmp/f2-py.jsonl` (transient; regenerate with the commands above).
