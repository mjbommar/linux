# Vision

## What we're building

A version of `arch/um/` that delivers, **from one source tree**:

- **~1-11 µs syscall overhead** on naive KVM backend
  (vs. ~20 µs seccomp baseline today — 2× on Skylake-S,
  ~20× on Alder Lake i9 @ 960 ns). **~100 ns** as an
  aspirational later target, likely reachable with a
  systrap-equivalent in-guest gadget layer. Empirical
  floor measured 2026-04-23 across 8 hosts spanning
  Skylake-S through Zen 4 / Alder Lake, plus GHA nested-
  virt confirmed working (EPYC 7763 under Hyper-V),
  P-state-locked re-runs resolving initial measurement
  variance. See `02-workstreams/D-kvm-backend/spikes/`
  for spike harnesses + `02-workstreams/D-kvm-backend/
  measurements.md` for the durable timing log that
  later spikes + real-implementation benchmarks extend.
- **Full instrumentation** — KASAN, KMSAN, KCSAN, KFENCE, KCOV,
  kprobes, ftrace, BPF JIT, KGDB — all working, with research mode
  defaults that turn them on.
- **Deterministic time-travel and record-replay**, promoted from
  Kconfig footnote to first-class.
- **Snapshot/forkserver startup** in <50 ms for fuzzing.
- **Three explicit profiles** (research / fuzz / sandbox) with
  inverted defaults, plus production, library, embedded, and
  time-travel siblings.
- **Runtime-flippable hooks**: ship a fast binary, hit a crash,
  enable tracing without rebuilding or restarting.
- **Backend abstraction**: ptrace, seccomp, and KVM as swappable
  trap mechanisms.
- **A `vm/uml` syzkaller backend** so the world's most-used
  kernel fuzzer can target UML.

## What we're not building

- **A reimplementation of Linux.** UML is the upstream kernel
  recompiled. Sentry-style "rewrite the kernel in $LANGUAGE" is
  the wrong shape for our use case (kernel security research) and
  the wrong shape for upstream (semantic drift, see WSL1).
- **A unikernel.** Single address space loses the
  user/kernel boundary that's essential for studying real syscall
  behavior.
- **A microVM.** Firecracker exists. We are not competing with
  Firecracker for cold-start; we are competing with QEMU-KVM for
  *kernel research and fuzzing*, where Firecracker doesn't play.
- **A container runtime.** gVisor and runc exist. We are not in
  that market.
- **A from-scratch hypervisor.** KVM exists. The KVM backend rides
  on KVM, doesn't replace it.

## Why now

Five things converged in 2025-2026 that make this the right moment:

1. **Seccomp mode merged in 6.16** (Benjamin Berg). The
   architectural prerequisite for replacing ptrace as the trap
   mechanism — and the proof that someone can land non-trivial
   UML platform work upstream.

2. **SMP landed (kernel-side)** as Tiwei Bie's v4. The serialization
   walls that made profile-driven design pointless are gone.

3. **gVisor's systrap and KVM platform** demonstrate that swappable
   backends with vastly different cost models are buildable —
   gVisor proved the idea works, even if their target market is
   different from ours.

4. **LKL got there first on fuzzing speed (60×)** with a worse
   architecture. The opportunity to absorb LKL's wins into
   something with full ring-split fidelity is open and aging.

5. **The kernel community now has Documentation/process/coding-
   assistants.rst** (April 2026) — AI-assisted patches are
   sanctioned. The 24-month engineering effort below is feasible
   for a small team using AI tooling that wasn't available when
   Tazaki tried in 2019.

Five years from now, either UML has caught up or LKL + Rust VMM
crates + crosvm + eBPF together have eaten the niche. The
window is open now and is not large.

## What success looks like

After 24 engineer-months across the four workstreams:

- `make ARCH=um uml/research` produces a debug-friendly UML with
  all sanitizers and tracing on; reproduces a syzbot CVE in <30 s.
- `make ARCH=um uml/fuzz` produces a fuzzer-optimized UML; runs
  syzkaller against it at >1000 iter/s with KCOV.
- `make ARCH=um uml/prod-with-hooks` produces a production UML
  with ~100 ns syscall and the runtime ability to flip on
  tracing without rebuilding.
- `make ARCH=um uml/sandbox` produces a minimized-TCB UML
  suitable for running untrusted workloads.
- The same source tree builds all of the above. Patches that touch
  one profile's defaults must declare what happens in the others.
- syzbot has a UML kernel in its rotation alongside QEMU-KVM.
- ARM64 and RISC-V hosts are buildable.

## What success doesn't require

- **Replacing QEMU-KVM in syzbot's primary fleet.** UML-as-target
  is additive; QEMU-KVM stays.
- **Bare-metal performance parity in every profile.** Sandbox
  mode pays seccomp cost; research mode pays sanitizer cost.
  prod-fast on KVM is the only profile that targets near-native.
- **Adoption by every kernel subsystem.** Some subsystems
  (drivers depending on real PCIe enumeration, GPU stacks) will
  never run usefully under UML. That's fine.

## Non-goals worth being explicit about

- **Windows guest support.** Cloud Hypervisor's domain.
- **Live migration.** Maybe later, but not in scope for v1.
- **GPU passthrough.** The hardware-virt land grab. Out of scope.
- **Replacing QEMU TCG for cross-arch emulation.** UML is host-arch
  only by design.
- **Becoming a microservices runtime.** That's containers' job.

## Stakeholders

- **Upstream maintainers**: Richard Weinberger (nominal), Johannes
  Berg, Benjamin Berg, Anton Ivanov, Tiwei Bie. Their buy-in is
  the gating constraint on landing.
- **Fuzzing community**: syzkaller team (Dmitry Vyukov), kBdysch
  (LKL fuzzers), Janus.
- **Research community**: groups using UML for network simulation
  (Intel WiFi), kernel security (us), distributed systems
  (NDSS/OSDI fuzzing papers).
- **Distros/CI**: anyone running kernel CI in containers where
  KVM isn't available — UML is the only realistic option.

## The honest worry

The 24-month plan presumes a small team. UML has historically
been driven by 2-4 part-time engineers. If this plan is to land
upstream, it needs either:

- A funded effort (1-2 FTEs for 2 years), OR
- A community of contributors organized around the workstream
  decomposition, OR
- A staged rollout where each workstream lands independently and
  the architecture emerges from the union.

Tazaki's "Unify LKL into UML" RFC stalled at v8 not because the
idea was wrong but because no one had bandwidth to drive it
through 8 review cycles. We need a different organizational
strategy than "one person, repeated RFCs".

See `04-risks/political-lkml-acceptance.md`.
