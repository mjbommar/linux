# C-10: Crosvm-style host launcher

**Status:** planned
**Effort:** 6 weeks
**Dependencies:** A, B
**Blocks:** sandbox profile having strong host-side isolation

## Goal

A host-side launcher (`uml-launcher`) that spawns the UML kernel
in a process-per-device topology with per-device seccomp filters,
modeled on crosvm. Deliverable: sandbox profile UML has a host
attack surface comparable to crosvm or Firecracker.

## Approach

1. **Architecture**:
   - One process: UML kernel (the guest).
   - One process per virtual device: net helper, block helper,
     console helper, etc.
   - Communication: vhost-user sockets between kernel and devices.
2. **Per-device seccomp filters**: each device process applies a
   filter limiting it to ~7-15 host syscalls (Nabla-inspired).
3. **Namespace stack**: launcher places everything in
   user/mount/network/pid namespaces.
4. **Optional LSM profile**: ship reference AppArmor/SELinux
   profiles for the launcher.
5. **Replaces** UML's existing `linux ... ubd0=... eth0=...`
   approach with a launcher that spawns the right helpers.

## Deliverable

- `tools/uml/launcher/` — Rust source for the launcher
  (Rust because it shares vm-virtio crates with crosvm)
- Per-device helper binaries (or compile-in via crosvm's
  approach — TBD per task design)
- Reference AppArmor profile
- Reference seccomp filters per device type
- Documentation

## Validation

- Sandbox profile UML running under launcher passes a host-
  syscall audit (only allowed syscalls invoked)
- Privilege escalation from inside guest doesn't escape device
  jails (independent security review)
- Launcher boots a useful UML in <500 ms (not as fast as
  Firecracker but not embarrassing)

## Open questions

- **Q1**: Rust dependency for kernel-adjacent tooling — is
  upstream OK with this? (Plan: tools/ already has Python and
  shell; Rust is gradually accepted. Talk to maintainers.)
- **Q2**: Do we ship per-device binaries or a single multi-call
  binary (like busybox)? (Plan: single binary, multi-call.
  Easier to package.)
- **Q3**: Cohabitation with existing UML invocation patterns?
  (Plan: launcher is opt-in; classic `linux ...` still works.)

## Risk

Host-side launcher work is wide and touches packaging, distros,
documentation. Easy to underestimate.

**Mitigation:**
- Stage in: first ship a launcher with one-process-per-virtio
  device but no seccomp filters. Iterate to filtered version.
- Engage crosvm authors for guidance on what their pain points
  were.
