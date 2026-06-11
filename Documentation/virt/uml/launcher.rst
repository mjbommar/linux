.. SPDX-License-Identifier: GPL-2.0

=================
UML host launcher
=================

``uml-launcher`` is a host-side Rust binary that invokes a UML
kernel. It replaces the hand-rolled shell wrappers people write
to set up rootfs, console, and command-line arguments, and it
exposes a declarative CLI with structured logging, signal
forwarding, and the AFL forkserver fd protocol built in.

The base command supervises a single UML process. The launcher also
contains per-device vhost-user backend helpers for isolated virtio
devices where a profile or command line requests them.

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
* ``sandbox.toml`` — sandbox-profile launch knobs; host-side
  isolation is configured through vhost-user helpers and policy.
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

Per-device decomposition (--virtio)
===================================

The launcher's ``run`` subcommand accepts repeatable
``--virtio <class>[:<args>]`` flags that spawn dedicated
backend processes over vhost-user:

   * ``--virtio console`` — console backend; bytes flow
     through the launcher's stdio like the bare-kernel path
     did, but the TX / RX / stdin work happens in its own
     process with a seccomp filter applied.
   * ``--virtio net:<tap>`` — net backend; attaches to a
     pre-created TAP interface on the host via
     ``ioctl(TUNSETIFF, IFF_TAP | IFF_NO_PI)``. Operator
     sets up the tap with ``ip tuntap add <name> mode tap``
     before launch.
   * ``--virtio block:<image>[,ro]`` — block backend;
     file-backed virtio-blk with optional read-only.

Each backend runs with:

   * The same binary (``uml-launcher backend <class>``), so
     packaging is one ELF + one systemd unit + one
     AppArmor/SELinux profile per class.
   * An ``seccompiler`` allowlist: ~40 syscalls baseline
     (epoll, recvmsg, eventfd2, mmap, etc.) plus class
     extras (``preadv``/``pwritev`` for block,
     ``TUNSETIFF`` is pre-seccomp). Anything outside →
     ``SIGSYS`` via ``SECCOMP_RET_KILL_PROCESS``.
   * An AppArmor sub-profile transition
     (``uml-launcher//backend_{console,net,block}``) via
     ``aa_change_profile()`` when the profile is loaded;
     silent skip otherwise.

Example::

   # Console + a block disk, no network:
   uml-launcher run --kernel ./linux --init /bin/sh --mem 512M \\
       --virtio console \\
       --virtio block:/srv/uml/rootfs.img

   # All three classes:
   sudo ip tuntap add tap0 mode tap user $USER
   sudo ip link set tap0 up
   uml-launcher run --kernel ./linux --init /sbin/init \\
       --virtio console \\
       --virtio net:tap0 \\
       --virtio block:/srv/uml/rootfs.img,ro

The launcher spawns each backend before it starts UML, waits
for each socket to appear, appends
``virtio_uml.device=<socket>:<id>`` entries to the kernel
cmdline, then supervises both UML and the backends. On UML
exit (or SIGTERM to the launcher), each backend is sent
``SIGTERM``, given 500 ms to clean up, then ``SIGKILL`` if
still alive, and its socket file is unlinked. No orphan
processes survive a clean exit.

Implemented capabilities
========================

Base supervision
  * Spawn, supervise, reap. One UML process.
  * CLI + env + TOML config merge.
  * Signal forwarding + exit-code passthrough.
  * Forkserver fd plumbing.
  * hostfs root, stdio/null console.

Device backends
  * Per-device host processes over vhost-user: console
    (full TX + RX + stdin reader), net (TAP, no offloads),
    block (preadv/pwritev, FLUSH, GET_ID, RO gate).
  * ``seccompiler``-authored per-class allowlist filters.
  * Reference AppArmor profile + runtime
    ``aa_change_profile()`` into per-backend sub-profiles.
  * Reference SELinux refpolicy module
    (``selinux-policy-dev`` builds ``uml_launcher.pp``).
  * ``--virtio <class>[:<args>]`` orchestration.

v3 (later)
  * VIRTIO_NET_F_MRG_RXBUF + CSUM/GSO offloads.
  * virtio-blk DISCARD / WRITE_ZEROES / O_DIRECT.
  * Multi-instance of the same class (two disks etc.).
  * Multi-instance daemon with JSON-RPC control.
  * Systemd unit template.
  * Distro packaging (``.deb``, ``.rpm``).

Further reading
===============

* ``Documentation/virt/uml/snapshot.rst`` — snapshot / forkserver
  surface that ``--forkserver`` interoperates with.
* ``tools/uml/uml-launcher/README.md`` — build / install / local
  quickstart.
* crosvm (https://crosvm.dev/) — architectural prior art for
  the v2 per-device-process model.
* rust-vmm (https://github.com/rust-vmm) —
  ``vhost`` / ``vhost-user-backend`` / ``seccompiler`` crates
  the v2 work will consume.
