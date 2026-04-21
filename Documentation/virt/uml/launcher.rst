.. SPDX-License-Identifier: GPL-2.0

=================
UML host launcher
=================

``uml-launcher`` is a host-side Rust binary that invokes a UML
kernel. It replaces the hand-rolled shell wrappers people write
to set up rootfs, console, and command-line arguments, and it
exposes a declarative CLI with structured logging, signal
forwarding, and the AFL forkserver fd protocol built in.

Workstream C-10 of the UML redesign (see
``Documentation/virt/uml/redesign/02-workstreams/
C-profiles-and-gaps/10-host-launcher-crosvm.md``). v1 scope is
single-process supervision; v2 will add per-device vhost-user
helpers with per-device seccomp filters for the sandbox
profile.

Install
=======

``uml-launcher`` lives at ``tools/uml/uml-launcher/`` in the
kernel source tree. Build with ::

   make -C tools/uml/uml-launcher

and install with ::

   make -C tools/uml/uml-launcher install PREFIX=/usr/local

The Makefile auto-detects ``cargo``; if Rust isn't installed the
target becomes a polite no-op. Matches the optional-tool pattern
used by ``tools/bpf/bpftool``.

MSRV: Rust **1.74**. Debian stable + Fedora ship newer than that;
the toolchain requirement is satisfied on every supported distro.

Usage
=====

Basic spawn::

   uml-launcher run \\
       --kernel ./linux \\
       --init /bin/sh \\
       --mem 512M

Equivalent to (and replaces) ::

   ./linux rootfstype=hostfs rootflags=/ init=/bin/sh mem=512M \\
           con=null con0=fd:0,fd:1 root=/dev/root rw

Run ``uml-launcher run --help`` for the full flag surface.

Configuration precedence
------------------------

Sources are merged highest-priority first:

1. CLI flags (``--mem 512M``)
2. Environment variables (``UML_MEM=512M``)
3. TOML config file (``--config ~/uml.toml``)
4. Built-in defaults

Example TOML::

   # ~/uml.toml
   kernel = "/home/alice/src/linux/linux"
   init = "/bin/sh"
   mem = "512M"
   root = "hostfs"

Invoke with ::

   uml-launcher run --config ~/uml.toml

CLI flags override file values; file values override env; env
overrides defaults.

Copy-paste starting points for common profiles ship in
``tools/uml/uml-launcher/examples/``:

* ``fuzz.toml`` — fuzz-profile binary + forkserver-ready init.
* ``research.toml`` — research-profile (KASAN/KFENCE/ftrace/
  kprobes + BPF JIT) interactive shell. Note: KCOV coverage is
  intentionally off in ``research`` (see the profile doc for
  rationale); use ``fuzz`` / ``fuzz-deep`` when KCOV is needed.
* ``sandbox.toml`` — sandbox-profile v1 shape (v2 will add
  per-device isolation declarations).
* ``dev.toml`` — daily-driver defconfig + ``/bin/sh``.

Signal handling
---------------

``uml-launcher`` catches ``SIGINT``, ``SIGTERM``, ``SIGHUP``,
and ``SIGQUIT`` on a dedicated thread and forwards them to the
UML child via ``kill(2)``. If the child handles the signal and
exits cleanly, the launcher waits for it and returns its exit
code. If the child is killed by a signal, the launcher returns
``128 + signum`` (conventional shell exit-code mapping).

Example ::

   uml-launcher run --kernel ./linux --init /bin/sh &
   LP=$!
   # ... do stuff ...
   kill -TERM $LP     # launcher forwards SIGTERM to UML; waits for exit.
   wait $LP
   echo "exit = $?"   # whatever UML returned, or 128+SIGTERM if it
                      # was unhandled inside.

Logging
-------

Via ``tracing`` and ``tracing-subscriber``:

* ``-v`` info, ``-vv`` debug, ``-vvv`` trace. Logs go to
  stderr; UML's stdout/stderr still go to the launcher's.
* ``--log-format json`` emits JSON lines on stderr for
  machine consumption.
* ``RUST_LOG=uml_launcher=trace`` overrides the verbosity
  flag (matches the standard ``log`` ecosystem).

Example::

   RUST_LOG=uml_launcher=debug uml-launcher --log-format json run \\
       --kernel ./linux --init /bin/true

AFL forkserver interop
======================

``uml-launcher`` can plumb host file descriptors into the UML
child as fds **198** (fuzzer → kernel, ctl) and **199**
(kernel → fuzzer, status), matching the AFL forkserver protocol
that the UML snapshot/forkserver seam speaks
(see ``Documentation/virt/uml/snapshot.rst``).

Usage pattern ::

   # Fuzzer opens its pipes on arbitrary fds, then hands them off:
   exec 3<> /dev/null   # placeholder; real fuzzer pipes the fuzzer end
   exec 4<> /dev/null
   uml-launcher run --forkserver 3,4 \\
       --kernel /path/to/fuzz-build/linux \\
       --init /my/fuzz-ready.sh

The launcher ``dup2``s 3 → 198 and 4 → 199 inside
``CommandExt::pre_exec``, clears ``FD_CLOEXEC`` on both, and
then ``execve`` s the UML binary. From the fuzzer's point of
view the UML kernel comes up with fds 198/199 already
populated; no shell-side fd juggling needed.

``snapshot-smoke-driver.py`` (in the selftest at
``tools/testing/selftests/um/snapshot-smoke/``) is the reference
implementation on the fuzzer side; it uses ``os.dup2`` directly,
but an AFL++ or syzkaller driver would typically hand off
already-open pipes and let ``uml-launcher`` do the plumbing.

Roadmap
=======

v1 (this release)
  * Spawn, supervise, reap. One UML process.
  * CLI + env + TOML config merge.
  * Signal forwarding + exit-code passthrough.
  * Forkserver fd plumbing.
  * hostfs root, stdio/null console.

v2 (planned; see design doc)
  * Per-device host processes over vhost-user. Each helper
    (net, block, console) gets its own process with a
    ``seccompiler``-authored allowlist of ~7-15 syscalls.
    Matches crosvm / Firecracker isolation posture.
  * Reference AppArmor + SELinux profiles.
  * Block-device (``ubd``) root support.
  * PTY console.

v3 (later)
  * Multi-instance daemon with JSON-RPC control.
  * Systemd unit template.
  * Distro packaging (``.deb``, ``.rpm``).

Further reading
===============

* ``Documentation/virt/uml/snapshot.rst`` — C-09 snapshot /
  forkserver surface that ``--forkserver`` interoperates with.
* ``Documentation/virt/uml/redesign/02-workstreams/
  C-profiles-and-gaps/10-host-launcher-crosvm.md`` — v1 design
  + v2 roadmap + crate-stack rationale.
* ``tools/uml/uml-launcher/README.md`` — build / install / local
  quickstart.
* crosvm (https://crosvm.dev/) — architectural prior art for
  the v2 per-device-process model.
* rust-vmm (https://github.com/rust-vmm) —
  ``vhost`` / ``vhost-user-backend`` / ``seccompiler`` crates
  the v2 work will consume.
