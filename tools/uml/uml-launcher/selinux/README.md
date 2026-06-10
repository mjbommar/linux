# SELinux reference policy for uml-launcher

Reference-policy module for the UML host launcher — parallel
to the AppArmor profile under `../apparmor/`. Ship either
one; both together is redundant-safe (defense-in-depth).
The AppArmor profile is the more-detailed confinement today
(per-backend sub-profiles); this module currently declares
only the parent `uml_launcher_t` domain. Sub-domains
(`uml_backend_console_t`, `_net_t`, `_block_t`) can parallel the
AppArmor sub-profile split when finer SELinux separation is needed.

## Files

- `uml_launcher.te` — type enforcement rules.
- `uml_launcher.fc` — file-context labeling for the binary
  (`/usr/bin/uml-launcher`, `/usr/local/bin/uml-launcher`)
  and the runtime dir (`/run/uml-launcher`).
- `uml_launcher.if` — interface file exposing
  `uml_launcher_domtrans` and `uml_launcher_run` for
  other modules that want to legitimately transition in.

## Building

Needs `selinux-policy-dev` (Debian/Ubuntu) or
`selinux-policy-devel` (Fedora/RHEL). Both ship
`/usr/share/selinux/devel/Makefile` which knows how to
compile a loadable `.pp` module:

```sh
cd tools/uml/uml-launcher/selinux/
make -f /usr/share/selinux/devel/Makefile uml_launcher.pp
```

Success = a `uml_launcher.pp` file (~120 KiB).

## Installing

```sh
sudo semodule -i uml_launcher.pp
sudo restorecon -Rv /usr/bin/uml-launcher
sudo restorecon -Rv /usr/local/bin/uml-launcher 2>/dev/null || true
```

Confirm with:

```sh
sudo semodule -l | grep uml_launcher
ls -Z /usr/bin/uml-launcher       # → system_u:object_r:uml_launcher_exec_t:s0
```

## Removing

```sh
sudo semodule -r uml_launcher
```

## Runtime enforcement wire-up (follow-on)

Analogous to the AppArmor profile's `aa_change_profile()`
call in `src/backend/apparmor.rs`, the SELinux counterpart
is `setcon(3)` / `selinux_setcon(2)` from the same three
backend entry points. That lands when the C-10 v2
orchestrator (commit 8) gains a fork+exec path that can
decorate each child with the right context via
`setexeccon()` at the parent side — matching libvirt's
pattern for domain transitions around `qemu-system-*`
subprocesses.

Until that lands, enforcement is coarse: the whole
launcher + all its backend subcommands run in
`uml_launcher_t`. Still useful — it tightens the syscall
and file surface relative to `init_t` or
`unconfined_service_t`, and the AppArmor side already
provides the per-backend granularity on distros that ship
AppArmor (Ubuntu, SUSE).

## Coexistence with AppArmor

On hosts with both LSMs available (which is rare but
possible — AppArmor + SELinux can coexist on modern
kernels if neither is primary), both profiles apply.
Denials from either are independent; neither overrides
the other. In practice distros pick one LSM per install.

## Relationship to the seccomp filter

As with the AppArmor profile: seccomp gates which syscalls
are reachable, SELinux (or AppArmor) gates which files /
sockets / network ops those syscalls can target. Either LSM
alone is some protection; the LSM + seccomp together match
the crosvm / Firecracker isolation posture.
