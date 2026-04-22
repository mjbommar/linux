.. SPDX-License-Identifier: GPL-2.0

===============================
UML snapshot / forkserver (v1)
===============================

The UML snapshot / forkserver surface landed in workstream C-09 of
the redesign (see
``Documentation/virt/uml/redesign/02-workstreams/
C-profiles-and-gaps/09-snapshot-forkserver.md``). It gives a UML
kernel a cooperative "ready point," a ``fork()``-based worker
spawner, and an AFL-compatible 12-byte-per-iteration wire protocol
on host file descriptors 198 and 199.

The v1 landing is intentionally narrow: it provides the kernel-
side plumbing and the protocol seam so an external fuzzer (AFL++,
syzkaller's upcoming ``vm/uml`` backend) can drive per-iteration
execution against trivial, short-lived guest payloads. A v2
redesign covering sustained fuzz on blocking guest syscalls — with
a freezer-cgroup pre-fork + per-task re-clone path — is outlined
in decisions-log ``D41`` / ``D42`` and in
``Documentation/virt/uml/redesign/08-future-phases/
02-snapshot-to-disk.md``.

Availability
============

Compiled only when a profile explicitly enables
``CONFIG_UM_SNAPSHOT_FORKSERVER``. The ``fuzz`` defconfig is the
expected consumer once commit 6 lands. Other profiles
(``prod-fast``, ``sandbox``, ``research``) intentionally leave it
off — the forkserver is not a zero-cost feature and has no
meaning outside a fuzz context.

To check whether a running UML kernel has the feature::

   # Exposed only when CONFIG_UM_SNAPSHOT_FORKSERVER=y
   cat /sys/kernel/um/state_version      # -> "1"

The sysfs integer is the wire-protocol version, not the kernel
version. A protocol-breaking change bumps it; a kernel bump that
preserves the 12-byte contract does not.

Wire protocol (AFL-compatible)
==============================

The host fuzzer opens file descriptors 198 (``ctl``, fuzzer →
kernel) and 199 (``status``, kernel → fuzzer) before ``exec()``-ing
the UML binary. The kernel then writes a 4-byte handshake on 199
once it reaches its first ready point, and enters the loop::

   parent -> 199: 4 bytes "AFL\\0"                     (handshake)

   LOOP:
       fuzzer -> 198: 4 bytes testcase descriptor      (ignored today)
       parent -> 199: 4 bytes worker host pid (LE int32)
       parent -> 199: 4 bytes worker exit status (LE int32,
                      placeholder 0 in v1)

   (fuzzer closes 198 to end the loop)

The reference driver in
``tools/testing/selftests/um/snapshot-smoke/snapshot-smoke-driver.py``
is the smallest self-contained example. AFL++'s
``src/afl-forkserver.c`` is the canonical reference against which
the wire format was cut (v1 uses the classic "AFL\\0" magic).

Ready points
============

A ready point is a name passed to ``um_snapshot_ready(name)``. The
kernel writes the handshake and enters the fork loop when it hits
one. Today three paths reach it:

1. **debugfs trigger.** Writing any string to
   ``/sys/kernel/debug/um/snapshot_ready`` invokes
   ``um_snapshot_ready`` with that string as the ready-point name.
   This is the usual in-guest entry; it lets an init script pick
   its own ready-point name.

2. **No-fuzzer clean-skip.** If fds 198 and 199 are not open when
   ``um_snapshot_ready`` fires, the kernel logs one line and
   returns ``-ENODEV`` without touching any state. Safe to hit
   from a casual boot.

3. **Snapshot API.** Out-of-tree callers can invoke
   ``um_snapshot_ready("name")`` directly; the function is
   exported as a kernel symbol gated on
   ``CONFIG_UM_SNAPSHOT_FORKSERVER=y``.

Signal-based triggers (``SIGRTMIN+N``, per decisions-log ``D38``)
will share the same entry point when they land.

How it works
============

Quiesce and fork
----------------

When ``um_snapshot_ready`` fires with both fds open, the kernel
(a) writes the AFL\\0 handshake on 199, (b) blocks the UML-
dispatched host signal set via the ``signals_enabled`` thread-
local gate (see decisions-log ``D41``), (c) reads one 4-byte
testcase descriptor on 198, (d) ``fork()``s a worker, and (e)
``waitpid``s on the worker. The parent writes the worker's pid
and exit status back on 199, then loops for the next testcase.

Worker reinit
-------------

The forked worker inherits the full UML kernel state CoW. Most of
it is safe to use as-is; a few host-side resources have stale
state (pthread handles, epoll fds targeting parent-only events,
POSIX timers targeting the parent's thread id). The worker's
first act is ``um_snapshot_worker_init``, which walks a small
"forget → detach scheduler tasks → rebuild" path before returning
to guest code. See decisions-log ``D41`` / ``D42`` for the design
rationale and the v1 ceiling.

Per-FD disposition
------------------

Every host file-descriptor creation site in ``arch/um/os-Linux/``
carries a ``/* FD disposition ... */`` comment naming how it
behaves across a snapshot fork: ``inherit``, ``worker-rebuild``,
``exec-transmit``, ``exec-probe``, and so on. See the commit-4
section of the C-09 design doc for the taxonomy.

Limitations (v1 ceiling)
========================

- **Short non-blocking guest programs only.** The worker can run
  a trivial ``execve(/bin/true)`` and exit cleanly. It cannot
  sustain ``execve + wait + filesystem I/O`` — the scheduler
  invariants inherited via CoW are not fully sanitized by the
  v1 ``sched_worker_detach_other_tasks`` helper alone. Observed
  as a KASAN slab-out-of-bounds in ``__set_next_task_fair`` on
  the first voluntary ``schedule()`` after a blocking syscall.
  v2 replaces the narrow helper with a freezer-cgroup pre-fork
  barrier + per-task re-clone.

- **Single status byte is a placeholder.** The 4-byte status sent
  back on fd 199 is zero in v1; a real exit-status bridge is
  deferred to v2. The obvious fix — a blocking ``waitpid`` between
  pid-write and status-write — crashes the parent reproducibly
  even with UML's signal gate + raw ``wait4`` syscall; three
  separate sessions have failed to root-cause the crash, and
  further attempts are explicitly paused pending v2's
  freezer-cgroup redesign. See the KNOWN LIMITATION comment at
  the top of ``arch/um/kernel/snapshot.c``'s per-iteration AFL
  protocol for the current working theory.

- **Zombies no longer accumulate** (commit `257b8cf61b84`,
  2026-04-21). Each forkserver iteration begins with a
  non-blocking ``wait4(-1, ..., WNOHANG)`` drain
  (``os_snapshot_reap_zombies()``), reaping any worker that
  exited during the prior iteration's think time. The WNOHANG
  flag keeps the drain off the crash path the blocking variant
  hit. Verified by inspecting ``ps --ppid <uml_pid>`` after a
  completed iteration — no surviving child processes.

- **No on-disk snapshot.** v1 is a live-fork forkserver only. An
  ELF-core-with-PT_NOTE on-disk snapshot / resume path is
  outlined in ``08-future-phases/02-snapshot-to-disk.md`` and is
  v2 or later.

- **No SMP.** CONFIG_SMP is not supported on UML in-tree; the
  forkserver is single-CPU only.

Selftest
========

``tools/testing/selftests/um/snapshot-smoke/`` contains the
regression guard::

   make -C tools/testing/selftests/um/snapshot-smoke run_tests

Two-part layout: part A verifies the sysfs / debugfs plumbing
from inside a booted UML; part B drives a full
handshake + fork + status round-trip via
``snapshot-smoke-driver.py``. Set ``UML_BINARY`` to the fuzz
build, ``PART_B=0`` to skip the host driver (useful when no
``python3`` is available), or ``UML_MEM`` to override the
default ``128M``.

Further reading
===============

- ``Documentation/virt/uml/redesign/02-workstreams/
  C-profiles-and-gaps/09-snapshot-forkserver.md`` — v1 design.
- ``Documentation/virt/uml/redesign/04-risks/decisions-log.md``
  — ``D35`` (scope), ``D36`` (on-disk format v2), ``D37`` (v1
  pull-forward items), ``D38`` (Mode A vs Mode B), ``D39`` /
  ``D40`` (commit-3 split), ``D41`` (signals_enabled contract),
  ``D42`` (CFS worker-detach helper).
- ``Documentation/virt/uml/redesign/08-future-phases/
  02-snapshot-to-disk.md`` — v2 on-disk design.
- AFL++ ``src/afl-forkserver.c`` — reference for the wire
  protocol.
