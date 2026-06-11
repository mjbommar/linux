.. SPDX-License-Identifier: GPL-2.0

======================
KVM v2 State Trace Ring
======================

``CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE`` builds a private KVM v2 diagnostic
ring for focused backend triage. It is disabled by default, depends on
``CONFIG_DEBUG_FS``, and compiles out completely when the Kconfig option is
off.

Scope
=====

The state trace ring records compact KVM_RUN dispatch metadata:

* sequence number;
* monotonic timestamp in nanoseconds;
* host CPU and UML task pid;
* operation name;
* KVM exit reason and I/O port;
* RIP, RSP, and RAX from the KVM sync-regs snapshot.

The ring deliberately does not dump full task, vCPU, mm, FPU, or KVM memslot
state. Those broad dumps belonged to the historical investigation trace and
are not part of the current debug ABI. Use normal ``um_backend`` tracepoints
for general observability; use this ring when a retained in-guest debugfs dump
is more useful than a host-side trace session.

Debugfs ABI
===========

Mount debugfs inside the UML guest::

  mount -t debugfs none /sys/kernel/debug

The files live under ``/sys/kernel/debug/um/``:

``kvm_v2_state_trace_ctl``
    Write ``enable`` to turn on the static-key guarded hook sites, ``disable``
    to turn them off, and ``clear`` to reset retained entries and counters.

``kvm_v2_state_trace_status``
    Read-only counters::

      enabled: 0
      capacity: 4096
      entry_size: 64
      entries: 0
      sequence: 0
      overwritten: 0

``kvm_v2_state_trace_dump``
    Read-only text dump. The first line is a header. Data lines use stable
    ``key=value`` fields::

      seq=0 ts_ns=123 cpu=0 pid=1 op=run_enter exit_reason=0 io_port=0 rip=0x401000 sp=0x7fff0000 ax=0

The current operation names are ``run_enter``, ``run_exit``, and
``run_eintr``.

Validation
==========

The kselftest is::

  tools/testing/selftests/um/kvm-state-trace-smoke/run-kvm-state-trace-smoke.sh

It boots KVM v2, enables the ring through debugfs, runs a small guest workload,
disables the ring, dumps the retained events, clears the ring, and validates
the dump with ``parse-kvm-state-trace.py``. The parser requires monotonically
increasing sequence numbers and at least one ``run_enter`` and one ``run_exit``
entry.
