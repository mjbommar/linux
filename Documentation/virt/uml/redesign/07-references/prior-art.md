# Prior art — what we steal from each

## gVisor (Google)

**What we steal:**

- Backend abstraction with multiple implementations (ptrace,
  systrap, KVM). Direct inspiration for Layer 1.
- KVM platform design: Sentry as ring-0 inside KVM guest, app
  in ring-3, MSR_LSTAR trap. Direct template for workstream D.
- Systrap's JIT-patching of syscall instructions to skip the
  trap on hot paths. Maybe v2; not in initial scope.
- Lisafs-over-FD-donation for fast host FS access (similar
  idea to UML's hostfs).

**What we don't:**

- Sentry's reimplementation-of-Linux-in-Go model. UML is
  Linux. We don't reimplement.
- Compatibility-first design. We're Linux-fidelity-first.

**Reference:**
- https://gvisor.dev/docs/architecture_guide/
- https://github.com/google/gvisor/tree/master/pkg/sentry/platform/kvm
- https://gvisor.dev/blog/2023/04/28/systrap-release/

## LKL (Linux Kernel Library)

**What we steal:**

- Library-mode entry point: `lkl_sys_*()` callable from host
  code. Workstream C's library profile is direct LKL pattern
  reuse.
- LD_PRELOAD-style hijack for transparent syscall redirection
  (consider for v2).
- 60× fuzzing speed advantage. Library profile gets this
  property by design.

**What we don't:**

- Forked-from-mainline maintenance model. We stay on
  mainline.
- No-user/kernel-split as the only model. Library profile is
  one of many, not the only.

**Reference:**
- https://github.com/lkl/linux
- https://lwn.net/Articles/662953/
- https://lwn.net/Articles/804177/ (Tazaki's unification RFC)

## Firecracker (AWS)

**What we steal:**

- Boot-time minimization. 125 ms cold start is the bar.
- Minimal device list — drop legacy hardware emulation.
  Sandbox profile follows this.
- Single-binary VMM as operational unit. Crosvm-style launcher
  (C-10) follows.
- Jailer-style seccomp shell on the host. Direct inspiration
  for sandbox profile + crosvm launcher.

**What we don't:**

- Hardware-virt-only model. UML serves environments without
  KVM.

**Reference:**
- https://firecracker-microvm.github.io/
- https://github.com/firecracker-microvm/firecracker/blob/main/docs/design.md

## Cloud Hypervisor / Kata

**What we steal:**

- Rust VMM crates (vm-memory, kvm-ioctls, vm-virtio, vhost,
  vhost-user-backend). Crosvm launcher (C-10) consumes these.
- Device hotplug model.
- OCI/CRI integration patterns (for Kata-style integration if
  someone wants it).

**What we don't:**

- Windows guest support. Out of scope.
- Live migration. Out of scope for v1.

**Reference:**
- https://github.com/cloud-hypervisor/cloud-hypervisor
- (Kata) https://katacontainers.io/

## crosvm (Google)

**What we steal:**

- Process-per-device architecture with per-device seccomp
  filters. Direct template for workstream C-10.
- Narrow control sockets / FD passing between device processes.
- Sandboxing patterns (mount-ns, user-ns, AppArmor profile).

**What we don't:**

- ChromeOS-specific assumptions.

**Reference:**
- https://crosvm.dev/book/architecture/overview.html
- https://crosvm.dev/book/appendix/sandboxing.html
- https://crosvm.dev/book/appendix/seccomp.html

## Unikraft

**What we steal:**

- Modular library-OS build model (each subsystem opt-in).
  Inspiration for fine-grained Kconfig profiling.
- Binary-compat layer for unmodified Linux ELFs (consider for
  research-profile CVE reproduction).

**What we don't:**

- Unikernel model (single address space; no user/kernel
  split).
- Per-app specialization at build time.

**Reference:**
- https://github.com/unikraft/unikraft
- https://www.usenix.org/conference/atc21/presentation/kuo

## MirageOS / IncludeOS

**What we steal:**

- Type-safe network stack lessons (long-term inspiration).
- Single-binary deployment ergonomics.

**What we don't:**

- OCaml/C++ unikernel models. We stay in C and Linux.

**Reference:**
- https://mirage.io/

## seL4 / Bao / Jailhouse

**What we steal:**

- Single-digit-microsecond interrupt latency as a target
  (informs time-travel-research profile).
- Capability-based isolation as a long-term aspiration.

**What we don't:**

- Microkernel architecture. UML is Linux; not negotiable.
- Formal verification (out of scope; too expensive).

**Reference:**
- https://sel4.systems/
- (Bao) https://sandro2pinto.github.io/files/ew2020-bao.pdf

## Nabla Containers (IBM Research)

**What we steal:**

- ~7-syscall host attack surface as a design target. Sandbox
  profile + crosvm launcher.
- Solo5-style minimization mindset.

**What we don't:**

- Solo5 unikernel base.

**Reference:**
- https://nabla-containers.github.io/
- https://github.com/nabla-containers/runnc

## WASI / Wasm-as-substrate

**What we steal:**

- Verifier-backed sandbox model (long-term inspiration for
  device-driver auditability).
- "Thin kernel interfaces" pattern (EuroSys 2025) — informs
  how Layer 1 ops should look.

**What we don't:**

- Wasm as the substrate. Linux IS the substrate.

**Reference:**
- EuroSys 2025: "Empowering WebAssembly with Thin Kernel
  Interfaces"

## eBPF

**What we steal:**

- Verifier model (eventually for device drivers).
- LSM hook framework as a model for our own hook architecture
  (Layer 2 gates).
- "Cheap when off, expensive when on" pattern via JIT.

**What we don't:**

- Full eBPF programmability inside UML at Layer 1. We just
  need eBPF JIT in research profile (C-06).

**Reference:**
- https://ebpf.io/
- https://eunomia.dev/blog/2025/02/12/ebpf-ecosystem-progress-...

## BULKHEAD (NDSS 2025)

**What we steal:**

- Intra-kernel compartmentalization via Intel PKS. Long-term
  inspiration for sandbox profile evolution; not in v1 scope.
- 2% overhead is achievable for compartments — sets a target.

**What we don't:**

- Initial scope. v2+.

**Reference:**
- NDSS 2025 paper

## WSL1 / WSL2 (cautionary tale)

**What we learn:**

- WSL1's syscall-emulation-in-NT-kernel-pico-drivers model
  failed because semantic drift with upstream Linux was
  endless.
- WSL2 abandoned reimplementation for a real Linux VM.

**What we don't do:**

- Reimplement Linux semantics anywhere. UML's value is that it
  IS Linux. Stay there.

**Reference:**
- https://docs.microsoft.com/en-us/windows/wsl/

## Tazaki's "Unify LKL into UML" RFC v8 (2019)

**What we learn:**

- The technical idea is right; the execution stalled
  politically.
- Need: clear architectural commitment up front; small
  incremental series; maintainer engagement at design time.

**What we do:**

- All of the above, documented in
  `04-risks/political-lkml-acceptance.md`.

**Reference:**
- LWN article on the RFC; LKML threads
