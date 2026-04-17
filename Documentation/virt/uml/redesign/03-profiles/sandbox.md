# Profile: sandbox

**One-line:** Minimum TCB; seccomp-only; no debug surfaces;
hostile-workload-ready.

## Intended user

- Running untrusted code in UML as defense-in-depth.
- CI/CD pipelines that need an isolated kernel without
  /dev/kvm access.
- Anyone treating UML as a sandbox primitive (rather than a
  research tool).

## Defining features

- Backend: seccomp ONLY (compiled inline; no ptrace, no KVM in
  binary).
- Layer 2: NOT COMPILED IN. No debugfs surface. No flippable
  hooks.
- Sanitizers: none (TCB minimization).
- Tracers, mconsole, KGDB, KCOV: none.
- Crosvm-style host launcher: required (per-device jails).
- Per-device seccomp filters: applied.
- Mount/user/network namespaces: required.
- Optional LSM (AppArmor/SELinux) profile.

## Kconfig fragment

```
CONFIG_UM_BACKEND_SECCOMP=y
CONFIG_UM_BACKEND_SECCOMP_ONLY=y   # only one backend, inlined

# Layer 2 — entirely absent
CONFIG_UM_HOOKS=n
CONFIG_UM_HOOKS_DEBUGFS=n

# Layer 3 — nothing
# CONFIG_KASAN is not set
# CONFIG_KMSAN is not set
# CONFIG_KCSAN is not set
# CONFIG_KFENCE is not set
# CONFIG_UBSAN is not set

# Debug surfaces — all off
# CONFIG_MCONSOLE is not set
# CONFIG_KGDB is not set
# CONFIG_KCOV is not set
# CONFIG_FUNCTION_TRACER is not set
# CONFIG_KPROBES is not set
# CONFIG_BPF_JIT is not set

# Minimal
CONFIG_DEBUG_INFO=n
CONFIG_KALLSYMS=n
CONFIG_KALLSYMS_ALL=n
```

## Host-side requirements

- Launch via `uml-launcher --profile sandbox <kernel-image>`
- Launcher applies:
  - mount-ns isolating to a chroot
  - user-ns mapping uid 0 → unprivileged host uid
  - net-ns (or no network)
  - per-device seccomp filters (~7-15 syscalls each)
  - cgroup-v2 resource limits
  - optional AppArmor profile

## Cost

- Binary size: ~15 MB (smallest)
- Boot: ~250 ms (no debug, fast init)
- Syscall: ~505 ns (seccomp baseline)
- RAM: ~25 MB

## Threat model

- **In scope**: a malicious workload running inside the UML
  guest cannot escape to the host beyond what the host syscall
  filter allows; cannot read/modify host files outside the
  chroot; cannot use the network beyond what net-ns permits.
- **Out of scope**: side-channel attacks on the host CPU
  (Spectre, Meltdown — that's the host kernel's problem);
  kernel bugs in UML itself that allow privilege escalation
  inside the guest (still bounded by the host filter).

## Validation

- Audit: `strace` the running launcher + UML; verify only
  whitelisted host syscalls invoked
- Pen-test: run a known kernel-escalation exploit inside the
  guest; verify host is unaffected
- Resource: confirm cgroup limits enforce CPU/memory caps

## What this profile is NOT

- Not for development. Use `research`.
- Not faster than `prod-fast`. Sandbox costs include namespace
  setup, per-device proxies, and the seccomp filter overhead.
- Not a magic security wrapper. Misconfigured launcher can
  expose host surface; review the launcher config carefully.
