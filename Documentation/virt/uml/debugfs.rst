.. SPDX-License-Identifier: GPL-2.0

========================
UML debugfs runtime API
========================

The UML kernel exposes runtime-flippable observability hooks under
``/sys/kernel/debug/um/`` when built with ``CONFIG_DEBUG_FS=y``. The
interface is the user-facing side of the Layer 2 static-key gate
infrastructure described in
``Documentation/virt/uml/redesign/01-architecture/three-layers.md``.

Requires root (files are mode ``0400``/``0600``).

Layout
======

::

  /sys/kernel/debug/um/
  ├── backend            (ro, 0400)  current backend name
  ├── hooks/
  │   ├── trace_syscalls        (rw, 0600)  0|1
  │   ├── kcov_enabled          (rw, 0600)  0|1
  │   ├── time_travel_active    (rw, 0600)  0|1
  │   ├── kfence_sample         (rw, 0600)  0|1
  │   ├── record_replay         (rw, 0600)  0|1
  │   ├── perf_dispatch         (rw, 0600)  0|1
  │   └── sanitize_paranoid     (rw, 0600)  0|1
  └── stats              (ro, 0400)  per-hook on-state + hit counter

``backend``
-----------

Plain text, one line, the current backend's name: ``ptrace``,
``seccomp``, or ``kvm`` (when workstream D lands). Empty backend
shows ``(uninitialized)`` — should never be observed on a booted
kernel.

``hooks/<gate>``
-----------------

Each file is a single bit: ``0`` when the gate is off (default),
``1`` when on. Reading returns the current value; writing flips
the underlying ``static_branch``::

  # cat /sys/kernel/debug/um/hooks/trace_syscalls
  0
  # echo 1 > /sys/kernel/debug/um/hooks/trace_syscalls
  # cat /sys/kernel/debug/um/hooks/trace_syscalls
  1

Writing a value other than ``0`` or ``1`` returns ``-EINVAL``.
Toggling is effective immediately: the next syscall / page fault /
context switch / IRQ / clock read that crosses the relevant hook
site will fire the slow path.

Gate semantics (workstream C fills in real consumers; today the
slow paths bump a hit counter in ``stats``):

``trace_syscalls``
    Fires the trace slow-path on every syscall entry/exit, page
    fault, context switch, and IRQ. Destined for the kernel's
    ftrace/trace-events subsystem.

``kcov_enabled``
    Fires on syscall entry. Destined for the KCOV coverage
    collector. Requires workstream C to port KCOV to UML; today
    a stub counter.

``time_travel_active``
    Fires on clock reads. Engaged by the time-travel profile to
    swap in deterministic timekeeping.

``kfence_sample``
    Fires on clock reads. Engaged by a future KFENCE port (work-
    stream C) to trigger periodic sampling.

``record_replay``
    Fires at every gate site. Destined for the record-replay
    subsystem (workstream C).

``perf_dispatch``
    Fires on syscall entry and context switch. Feeds the kernel's
    perf subsystem.

``sanitize_paranoid``
    Reserved for aggressive sanitizer mode. Currently unused on
    any hook site; exists for forward compatibility.

``stats``
---------

One line per gate, YAML-ish. Three columns: gate name, on/off state,
cumulative hit count since boot. Example on an idle system with
``trace_syscalls`` flipped on briefly::

  # cat /sys/kernel/debug/um/stats
  trace_syscalls       on=0 hits=147
  kcov_enabled         on=0 hits=0
  time_travel_active   on=0 hits=0
  kfence_sample        on=0 hits=0
  record_replay        on=0 hits=0
  perf_dispatch        on=0 hits=0
  sanitize_paranoid    on=0 hits=0

Cost impact
===========

With every gate off (default), the steady-state cost is ~1 ns per
gate per hook pass — well inside invariant I3 (see
``Documentation/virt/uml/redesign/01-architecture/invariants.md``).
Flipping a gate on adds the cost of the slow-path implementation
on *every* pass through that hook site; see ``stats`` to monitor
hit rate.

When ``CONFIG_HAVE_ARCH_JUMP_LABEL=y`` is enabled (workstream B-04
work), the off-state cost drops further to ~0.3 ns (a 5-byte NOP at
each gate). Until then, the C fallback form is used — see
``redesign/04-risks/decisions-log.md`` D19.

Absent ``CONFIG_DEBUG_FS=y``
============================

The ``sandbox`` profile sets ``CONFIG_DEBUG_FS=n`` and therefore
has no ``/sys/kernel/debug/um/`` tree. The gates themselves are
still compiled in (or can be compiled out via per-gate Kconfig
symbols added in workstream C). Without the debugfs interface,
gates remain in their Kconfig-chosen default state for the
lifetime of the boot — nothing flips them.
