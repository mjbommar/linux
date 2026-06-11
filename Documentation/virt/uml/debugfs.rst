.. SPDX-License-Identifier: GPL-2.0

========================
UML debugfs runtime API
========================

The UML kernel exposes runtime-flippable observability hooks under
``/sys/kernel/debug/um/`` when built with ``CONFIG_DEBUG_FS=y``. The
interface is the user-facing side of UML's static-key hook
infrastructure.

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
  │   └── perf_dispatch         (rw, 0600)  0|1
  └── stats              (ro, 0400)  per-hook on-state + hit counter

``backend``
-----------

Plain text, one line, the current backend's name: ``ptrace``,
``seccomp``, or a KVM backend name when KVM is enabled. Empty backend
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

Gate semantics:

``trace_syscalls``
    Fires the trace slow-path on every syscall entry/exit, page
    fault, context switch, and IRQ. Destined for the kernel's
    ftrace/trace-events subsystem.

``kcov_enabled``
    Fires on syscall entry. Builds without a live KCOV consumer use
    it as a counter-only hook.

``time_travel_active``
    Fires on clock reads. Engaged by the time-travel profile to
    swap in deterministic timekeeping.

``kfence_sample``
    Fires on clock reads. Reserved for KFENCE sampling control.

``record_replay``
    Fires at every gate site. Destined for the record-replay
    subsystem.

``perf_dispatch``
    Fires on syscall entry and context switch. Feeds the kernel's
    perf subsystem.

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

Cost impact
===========

With every gate off (default), the steady-state cost is ~1 ns per
gate per hook pass.
Flipping a gate on adds the cost of the slow-path implementation
on *every* pass through that hook site; see ``stats`` to monitor
hit rate.

When ``CONFIG_HAVE_ARCH_JUMP_LABEL=y`` is enabled, the off-state
cost drops further to a 5-byte NOP at each gate. Until then, the C
fallback form is used.

Absent ``CONFIG_DEBUG_FS=y``
============================

The ``sandbox`` profile sets ``CONFIG_DEBUG_FS=n`` and therefore
has no ``/sys/kernel/debug/um/`` tree. The gates themselves are
still compiled in (or can be compiled out via per-gate Kconfig
symbols). Without the debugfs interface, gates remain in their
Kconfig-chosen default state for the lifetime of the boot — nothing
flips them.
