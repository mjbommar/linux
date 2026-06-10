# Fork-Server And Pool Status

This file summarizes the current UML template-pause and pool work. It is
separate from the KVM v2 backend publication series. The pool feature is a
generic UML facility built around the seccomp backend and the template-pause
ready point.

## Current Scope

The active pool surface is:

- `/proc/um/template_pause` ready-point support;
- `um_template_pause=fork` fork-on-resume mode for seccomp-backed UML;
- per-member identity data for instance name, MAC address, host TAP name,
  IPv4 address, and default gateway;
- kernel-side identity parsing and application;
- optional vector2 TAP fd reopen for per-member networking;
- `umlctl pool spawn` for direct one-shot use;
- `umlctl pool serve` plus `pool take`, `pool status`, and daemon-routed
  destroy requests;
- daemon-routed `umlctl exec` and `umlctl port-forward` requests where the
  member has the required mconsole and network plumbing.

The KVM backend is intentionally refused for fork-on-resume. Forking a process
that owns `/dev/kvm` fds and vCPU mmap state would alias backend state into the
child. KVM-compatible pool support would need a separate design.

## What Works

Template pause can stop a UML instance at a chosen ready point, accept updated
identity data, and resume under supervisor control.

The private-stack clone path is the default fork-on-resume implementation.
That gives the child a private stack immediately after clone, avoiding shared
stack corruption between the paused master and the child. The older plain fork
path remains available only as a diagnostic fallback.

The pool daemon can boot one master, expose a Unix socket under the UML runtime
directory, and answer pool requests. Client commands can take a member, query
daemon state, and route destroy, exec, and port-forward operations through the
daemon socket.

Identity application is covered in the kernel:

- parse and validate the fixed-size identity blob;
- locate the target UML netdev;
- apply MAC, IPv4 address, netmask, and default route;
- reopen vector2 TAP-backed netdevs onto the requested host TAP when vector2
  support is built;
- rebind per-member mconsole state where the member provides a distinct
  mconsole path.

Selftest coverage exists under `tools/testing/selftests/um/` for the template
pause path, fork-on-resume, pool member creation, pool daemon operation, pool
benchmarks, daemon-routed exec, and daemon-routed port forwarding.

## Known Boundaries

The pool work is useful, but it is not yet a general fast-spawn substrate for
every UML configuration.

- KVM-backed fork-on-resume is refused by design.
- SMP pool-member operation is not the baseline path.
- The daemon has the socket API and request handling, but pre-warmed replenish
  policy is not the core contract yet.
- Daemon-routed exec requires working mconsole plumbing for the selected
  member.
- Per-member networking requires vector2 TAP support and a valid host TAP name.

These boundaries should stay explicit in user-facing docs and tests. The pool
feature should fail clearly when a prerequisite is missing.

## Validation Surface

Before considering this feature publishable, verify:

- UML builds with `CONFIG_UM_TEMPLATE_PAUSE=y`;
- fork-on-resume builds with `CONFIG_UM_TEMPLATE_PAUSE_FORK=y`;
- identity parsing KUnit passes when enabled;
- template-pause selftests pass for the configured backend;
- pool daemon selftests pass;
- pool benchmark checks report real member measurements, not master-only
  measurements;
- daemon-routed exec and port-forward paths either work end-to-end or return a
  clean structured failure when the member lacks the required channel;
- KVM-backed fork-on-resume requests are rejected with a clear error.

## Publication Guidance

Keep pool documentation separate from KVM v2 backend documentation.

The KVM v2 series can reference the pool work as adjacent UML infrastructure,
but it should not depend on pool internals. Likewise, pool docs should describe
runtime behavior and prerequisites rather than patch sequencing.
