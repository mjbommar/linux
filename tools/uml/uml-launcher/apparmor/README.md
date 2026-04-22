# AppArmor reference profile for uml-launcher

This directory ships an AppArmor profile covering the launcher
binary plus its three vhost-user backend classes (console, net,
block). It is a **reference** profile: distros are expected to
pull it in as-is for the common case and tighten or relax
per-site as needed.

## Files

- `uml-launcher` — the profile itself. Declares four named
  policies:
  - `uml-launcher`                  — main binary (`run`,
                                      `backend`, `version`).
  - `uml-launcher//backend_console` — console vhost-user backend.
  - `uml-launcher//backend_net`     — net vhost-user backend.
  - `uml-launcher//backend_block`   — block vhost-user backend.

  The parent grants broad orchestration permissions; each
  sub-profile narrows to exactly what its backend class needs.

## Installation

```sh
sudo cp uml-launcher /etc/apparmor.d/uml-launcher
sudo apparmor_parser -a /etc/apparmor.d/uml-launcher   # first time
sudo apparmor_parser -r /etc/apparmor.d/uml-launcher   # reload after edits
```

Verify the profiles are loaded:

```sh
sudo aa-status | grep uml-launcher
```

## Parse-check without installing

```sh
apparmor_parser --preprocess uml-launcher   # preprocess only
apparmor_parser -Q uml-launcher             # compile without loading
```

Both should exit 0. The launcher's `cargo test` suite runs the
latter automatically when `apparmor_parser` is on `$PATH`; the
test is skipped otherwise so hosts without AppArmor don't fail
CI.

## Automatic enforcement wire-up

The launcher's code does **not** yet call `aa_change_profile()`
to transition each backend into its sub-profile — that lands
in a follow-on commit per the C-10 v2 plan (decisions-log D52).
Today, enforcement is opt-in:

```sh
aa-exec -p uml-launcher//backend_console uml-launcher backend console --socket …
aa-exec -p uml-launcher//backend_net     uml-launcher backend net     --socket … --tap tap0
aa-exec -p uml-launcher//backend_block   uml-launcher backend block   --socket … --image …
```

When the wire-up commit lands, `uml-launcher run --virtio …`
will transition backend children automatically and this manual
`aa-exec` dance becomes unnecessary.

## Site-local tightening

The in-tree profile permits reasonably broad file-system
locations so site operators don't have to edit it for basic
use. Likely tightening knobs when hardening:

- **Block class**: replace the rw disk-image rules with `r`
  when the site runs exclusively with `--read-only`.
- **Net class**: replace the `capability net_admin` grant with
  a more specific capability set if the site uses precreated
  tap devices (no runtime TUNSETIFF required).
- **Console class**: replace the `/dev/tty*` + `/dev/pts/*`
  rules with a specific owner / fd pairing when the launcher's
  stdout is not a tty (for example, a named pipe into a log
  collector).

Paste local overrides into `/etc/apparmor.d/local/uml-launcher`
(the `include <local/uml-launcher>` line is standard on Ubuntu
profiles; add one to the parent profile if your distro expects
it).

## Profile assumptions

- UML guests run as the launching user, no privilege
  escalation. No `Csetuid` / `Cssetuid` capabilities are
  granted.
- Socket files live under `/tmp/` with names matching
  `uml-*.sock` or `uml-*-{console,net,block}-*.sock`. The
  orchestration commit will fix this naming scheme; the
  profile already allows both patterns so an early operator
  running the scaffold classes directly works today.
- Disk images live under the user's `$HOME` or `/tmp` or a
  shared `/srv/uml/images/`. Site-local paths outside this
  set require adding a rule.
- TAP devices are addressed by name via `/dev/net/tun`. If
  your site uses vhost-vdpa or kernel vhost-net instead
  (neither is in v2 scope), this profile doesn't cover it.

## Relationship to the seccomp filter

The seccomp filter (in the backend binary, applied before the
daemon event loop) and the AppArmor profile are
complementary: seccomp gates which syscalls are reachable;
AppArmor gates which files + sockets + network operations
those syscalls can target. Either alone is some protection;
both together are defense-in-depth. Neither replaces the
other.
