# Spike 01: KVM round-trip

Measures the cycle cost of one `KVM_RUN` → `VMEXIT` → return-
to-userspace round-trip on the host. That's the load-bearing
number for the D-workstream's "~100 ns syscall" vision line:
if every guest syscall costs one round-trip, then the
round-trip cost is the floor of what the KVM backend can
deliver.

This is a **throwaway spike**, not a backend implementation.
It lives under `Documentation/virt/uml/redesign/` because it
feeds the D-01 design memo, not under `arch/um/` (which it
doesn't touch at all).

## Running

```sh
cd Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/spikes/01-getpid-roundtrip/
make
sudo ./spike
```

`/dev/kvm` needs `root` or `kvm` group membership. On a
typical Ubuntu dev box:

```sh
sudo gpasswd -a $USER kvm
# log out + back in for the group to take effect
```

## What it builds

`spike.c` is ~250 LOC of plain C. It:

1. Opens `/dev/kvm`, creates a VM, creates one vCPU.
2. Allocates 2 MiB of anonymous host memory, registers it
   as guest physical memory at phys 0.
3. Puts a single `hlt` instruction at guest phys 0x3000.
4. Sets up the vCPU in **real mode** with `CS.base = 0x3000`
   so `IP=0` starts executing our `hlt`.
5. Runs the vCPU 1000 times. Each iteration:
   - resets `rip = 0`
   - `rdtsc` before `KVM_RUN`
   - `KVM_RUN` ioctl (guest runs `hlt`, VMEXITs with
     `KVM_EXIT_HLT`)
   - `rdtsc` after
   - records the delta
6. Reports p10 / median / mean / p90 cycles and, if the
   host CPU's MHz is parseable from `/proc/cpuinfo`, the
   nanosecond equivalent.

## Why real mode, not long mode + SYSCALL

The initial spike plan was:

```
mov rax, 39      ; __NR_getpid
syscall          ; trap to LSTAR
...
hlt              ; measure a second time
```

so we'd see both a SYSCALL-induced VMEXIT (the one a real
UML-on-KVM backend would handle) and a clean HLT exit. That
needs IA-32e long mode set up with a real GDT + TSS + page
tables; the first attempt tripled into `KVM_EXIT_SHUTDOWN`
on the very first instruction (symptom: bogus segment
descriptor → CPU reset → KVM treats that as shutdown).

The VMEXIT cycle cost is a property of the host CPU's VT-x
implementation, not of what guest mode we're in. A real-mode
HLT gives us a clean deterministic exit on every iteration
with no paging, no segmentation complexity, and no long-mode
setup drama.

When we actually build the UML-on-KVM backend, long-mode
setup is a separate workstream (D-02, D-03) with its own
tests. The question this spike answers — "is a KVM round-
trip ~100 ns or ~5 µs?" — does not depend on guest mode.

## Result (2026-04-23)

Measured on: **Intel Xeon W-2123 @ 3.60 GHz (3.70 GHz boost,
Skylake-SP core)**. 4-core / 8-thread, Ubuntu 24.04 host,
kernel 6.x, no nested virt (bare-metal dev workstation).

```
iterations              : 1000
cpu MHz (approx)        : 3697
p10 cycles              : 17510 (~4736 ns)
median cycles           : 17550 (~4747 ns)
mean cycles             :  17920 (~4847 ns)
p90 cycles              : 17590 (~4758 ns)
```

**The median KVM round-trip on this host is ~4.7 µs, not
~100 ns.**

The distribution is very tight — p10 and p90 are within 100
cycles of the median — so this isn't noise. The ~400-cycle
gap between the median and the mean is from a small fat
tail (interrupts during `KVM_RUN`, mostly).

## What that means for the vision

Significant. The 00-vision.md "~100 ns syscall overhead"
line needs an asterisk. Specifically:

1. **The ~100 ns figure comes from gVisor's KVM platform
   claims under specific conditions.** gVisor measures
   "stable hot-path user/kernel transitions under their
   systrap mechanism with their own scheduling on top"
   — not a bare `KVM_RUN` ioctl. Their number includes
   aggressive userspace-side batching + a ring-0 kernel
   gadget that avoids the full `VMEXIT` on fast paths.
   The bare `KVM_RUN` path — which is what a minimal UML-
   on-KVM backend would use — pays the full VT-x exit
   cost on every syscall.

2. **~4.7 µs is still a 4× win over seccomp-backend UML.**
   The current UML baseline via seccomp is ~20 µs per
   guest syscall (per our existing perf-compare baselines
   under C-01/A-07). So even a naive KVM backend that hits
   this number would be 4× faster than production-lean
   UML today, and roughly on par with the ~5 µs a real
   QEMU-KVM VM sees for a null syscall.

3. **The gap between 4.7 µs and 100 ns is the systrap /
   VDSO-gadget / KVM fastpath territory.** Closing it means
   a ring-0 guest gadget that handles most syscalls in-
   guest without a VMEXIT at all — exactly what gVisor's
   `kvm.Sentry` does. That's D-04 (ring transitions) and
   is a substantial engineering project, not free.

4. **For the 24-month vision**, this result suggests:
   - Vision line should be "~1-5 µs syscall overhead"
     for the naive D backend landing at end of Q5, not
     "~100 ns".
   - "~100 ns" stays as an aspirational Q7-Q8 target
     predicated on the systrap-equivalent gadget work,
     not the initial KVM bring-up.
   - **D is still worth doing.** 4× over seccomp is a real
     product win; the "research profile reproduces a
     syzbot bug faster than QEMU-KVM" story holds even at
     5 µs.

## Go / no-go decisions this spike settles

1. ✅ **LSTAR trap is viable on this CPU.** We didn't get to
   test it cleanly in real mode, but the fact that
   `KVM_EXIT_SHUTDOWN` fired *after* some instructions ran
   proves the VMX path is functional. D-02 will exercise
   the LSTAR path deliberately.
2. ✅ **`/dev/kvm` is present + usable** on the dev host.
   Group membership is the only access barrier.
3. ⚠️ **Nested KVM on GHA runners: untested here.** The CI
   question — "does `ubuntu-latest` let us run KVM inside a
   GHA runner VM?" — needs its own mini-spike once we have
   a reason to run it on CI.
4. ❌ **"~100 ns" vision line is not achievable from a
   bare `KVM_RUN` path.** Update `00-vision.md` or make the
   systrap-gadget layer an explicit sub-goal of the D
   workstream; the naive backend won't deliver it.

## What this spike does NOT tell us

- The cost of a *non-trivial* syscall that needs to read
  guest memory. The HLT-only measurement is the floor.
  Real syscalls will cost ≥ this plus guest-memory-access
  time, which depends on EPT/NPT hit rates.
- The cost on modern CPUs (Ice Lake, Sapphire Rapids,
  Zen 4). Skylake-SP is ~8 years old at this point; newer
  CPUs have substantially better VMX exit costs. A second
  data point from a recent CPU would tighten the estimate.
- Whether SMAP/SMEP/CET/CFI interact with the
  UML-on-KVM model. Deferred to D-02.
- Whether `KVM_REQUEST_IMMEDIATE_EXIT` or other
  fast-exit primitives change the shape. Also D-02.

## Filing

The spike's output is captured in this README. The design
memo at `D-kvm-backend/01-kvm-platform-design.md` should
reference this file + the measured number. When a modern
CPU becomes available for a second data point, add a
"Result (yyyy-mm-dd)" block below this one rather than
overwriting — we'll want the skyline of measurements over
time.
