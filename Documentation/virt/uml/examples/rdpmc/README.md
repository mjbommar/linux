# `rdpmc` from guest userspace — CR4.PCE plumbing

With `CONFIG_UM_BACKEND_KVM_V2_RDPMC=y` the kvm-v2 backend sets
`CR4.PCE = 1` in the guest's per-vCPU control registers at the
first-dispatch sregs install.  That flips the `rdpmc` instruction
from privileged to unprivileged so it executes at guest CPL=3
without `#GP` at the hardware level — guest userspace can read
performance-monitor counters directly, one instruction, no
gadget round-trip.

For the broader background — why this is in scope, the trade-off
ladder for "let guest userspace observe hardware state", and how
this compares to the LSTAR-gadget consumers — see the
[`aperf-mperf`](../aperf-mperf/README.md) sibling example and the
zoom-out commentary in
[`Documentation/virt/uml/aperf-mperf.rst`](../../aperf-mperf.rst).


## Honest scope

This change plumbs the **CPU-side** prerequisite for guest
userspace `rdpmc`.  It does NOT, on its own, guarantee that
`rdpmc` will return a meaningful value.  Two things must also be
true:

1. **The host KVM module must emulate `rdpmc` and have a working
   vPMU for this vCPU.**  On Intel hosts with `kvm.enable_pmu=Y`
   (the default) this Just Works.  On AMD hosts the behavior
   depends on `kvm-amd.enable_mediated_pmu` and the guest CPUID
   advertising `PERFCTR_CORE` (CPUID 0x80000001 ECX bit 23) — the
   legacy AMD vPMU initializes per-vCPU counters lazily on
   `kvm_pmu_refresh`, which is driven by `KVM_SET_CPUID2`.

2. **The counters must be armed.**  Linux `perf_event_open` in the
   guest is the canonical way to program counter-selector MSRs
   (which are still privileged and KVM-intercepted regardless of
   `CR4.PCE`).  Without it, the programmable counters PMC0..N are
   not running and `rdpmc(N)` returns 0.  Intel fixed counters
   stay at 0 unless `IA32_FIXED_CTR_CTRL` is programmed.

In other words: this commit removes the `#GP` *at the CPU level*.
It does not replace `perf` or `perf_event_open` for actually
configuring which event a counter tracks.


## What you get

* `perf record` inside the guest (when run with appropriate
  events) can use `rdpmc` for its mmap-based sample reads instead
  of round-tripping through `read(perf_fd, ...)`.  Same on bare
  metal; the kernel's `perf_event_mmap` userspace fast path
  depends on `CR4.PCE` being set on the running CPU.
* JIT engines (PyPy, V8, ...) that want to self-sample cycles or
  instructions via `rdpmc` work as on bare metal.
* The freestanding demo here can read `rdpmc(0)` without `#GP`,
  which is the architectural signal of "PCE is on."


## Demo

```bash
$ cd Documentation/virt/uml/examples/rdpmc
$ make
$ UML_BINARY=/path/to/kvm-v2-kernel make run
```

Three possible outcomes, each with a clear signal:

| Outcome | What you see | What it means |
|---|---|---|
| PASS | `RDPMC_DEMO: PASS rdpmc_works=1 counters_advanced=1` | `CR4.PCE` is on AND vPMU armed fixed counters; reading and counting works. |
| Partial | `RDPMC_DEMO: FAIL rdpmc_works=1 counters_advanced=0 reason=fixed_counters_not_armed` | `CR4.PCE` is on, `rdpmc` doesn't `#GP`, but counters return 0.  Host vPMU didn't arm fixed counters; you'd need `perf_event_open` in-guest to get real values. |
| Host vPMU rejects | No `RDPMC_DEMO` line; init segfaults at `rdpmc` | Host KVM intercepted `rdpmc` and injected `#GP` (most common on AMD with `enable_mediated_pmu=N` and no `PERFCTR_CORE` in guest CPUID).  This is not a bug in UML — the CPU-side `CR4.PCE` is correctly set; the host KVM emulation path is what failed.  Try `intel` hardware or `kvm-amd enable_mediated_pmu=Y`. |

The kselftest under
`tools/testing/selftests/um/rdpmc-smoke/` treats both PASS and
Partial as success (`rdpmc_works=1`) and SKIPs cleanly on the
host-vPMU-rejection case.


## Adapting

If you need `rdpmc` values to actually count something specific,
the path is:

1. In the guest, open a `perf_event_open` for the event of
   interest (use `PERF_EVENT_IOC_ENABLE`).
2. `mmap` the resulting fd.
3. Read the `pmc_id` from the mmap header, then `rdpmc(pmc_id)`
   directly in your hot loop — no syscall round-trip.

This is exactly how userspace `perf` works on bare metal.  Our
change makes the guest the same shape as bare metal in that
regard.

## Files

| File | Purpose |
|------|---------|
| `rdpmc-demo.c` | Freestanding init that does `rdpmc(0)` and three fixed-counter reads.  No libc, no rootfs. |
| `Makefile`     | Builds + runs the demo under UML. |
| `run-output.log` | Captured demo output from a working environment. |
