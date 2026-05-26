# D-05: Nested-virt detection and seccomp fallback

**Status:** planned
**Effort:** 2 weeks
**Dependencies:** D-02
**Blocks:** prod-fast profile correctness

## Goal

Detect nested-virt environments where KVM backend would be slower
than seccomp (per gVisor's documented behavior). Auto-fall back
to seccomp in those cases. Allow explicit override.

## Approach

1. **At init**: detect:
   - Is `/dev/kvm` accessible?
   - Are we ourselves running inside a VM? (`cpuid` hypervisor
     bit)
   - If nested: measure a few syscalls in both KVM and seccomp
     mode; pick faster.
2. **Boot param**: `backend=kvm` forces KVM (panic if
   unavailable). `backend=auto` does the probe.
3. **Document** when each is preferred:
   - Bare metal: KVM
   - Nested (VMware, GCE, AWS, Azure): usually seccomp
   - Container without `/dev/kvm`: seccomp (KVM unavailable)
   - Container with `/dev/kvm` (some k8s configs): seccomp
     (nested anyway)

## Deliverable

- `arch/um/backend/kvm/probe.c` — detection logic
- `arch/um/backend/multi/auto.c` — selection logic
- Documentation in `arch/um/Documentation/backends.rst`

## Validation

- On bare metal: `backend=auto` selects KVM
- In a nested VM (e.g., test under QEMU-KVM): `backend=auto`
  selects seccomp (or KVM if measured faster)
- `backend=kvm` in a no-/dev/kvm environment panics with a
  clear message

## Open questions

- **Q1**: How fine-grained is the perf measurement? (Plan: 1000
  `getpid()`s in each mode at boot; ~1 ms total.)
- **Q2**: Does this slow boot? (Plan: by ~1 ms; negligible.)

## Risk

Auto-detect makes wrong choice for some workload → user blames
backend, not nested-virt. Common in microbenchmarking.

**Mitigation:**
- Log the choice clearly (`um_backend: selected seccomp (nested
  virt detected, KVM 2.3× slower)`)
- Document the override
