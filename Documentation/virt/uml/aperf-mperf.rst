.. SPDX-License-Identifier: GPL-2.0

==========================================
APERF / MPERF MSR passthrough (kvm-v2)
==========================================

.. contents:: :local:

Summary
=======

The ``kvm-v2`` backend can ask KVM to skip the rdmsr intercept on
IA32_APERF (0xE7) and IA32_MPERF (0xE8) so the UML guest reads the
real host performance counters instead of the all-zeros KVM stub
return.  The mechanism is a single ``KVM_ENABLE_CAP`` ioctl with
``KVM_CAP_X86_DISABLE_EXITS`` / ``KVM_X86_DISABLE_EXITS_APERFMPERF``
(bit 4) issued at VM creation time.

This page documents how to turn the feature on, how to verify it is
working from inside the guest, and the QEMU / libvirt gap that
motivated exposing it.

Why this exists
===============

KVM has supported per-VM disable-exits for several MSR classes for
years; the APERFMPERF bit was added more recently.  QEMU's
``-overcommit cpu-pm=on`` plumbs the older bits
(``MWAIT`` / ``HLT`` / ``PAUSE`` / ``CSTATE``) but does **not** set
``KVM_X86_DISABLE_EXITS_APERFMPERF``, and libvirt does not surface a
``<features><kvm>`` property for it either.  The result: even when
QEMU advertises the feature via guest-visible CPUID, every guest
rdmsr on 0xE7 / 0xE8 returns zero.

``kvm-v2`` is its own KVM userspace VMM — it opens ``/dev/kvm``,
creates the VM, and runs vCPUs itself, without QEMU in the path.  So
adding the cap-enable is a five-line change and does not depend on
the QEMU patch landing.  Two practical uses:

1. **Guest-side host-counter observation.**  cpupower, turbostat,
   custom frequency-governor code, and any workload that wants to
   compute "what fraction of mperf did we actually run at" can do so
   from inside the guest.

2. **Repro evidence for the QEMU patch.**  Booting ``kvm-v2`` with
   the feature on gives non-zero rdmsr results; booting with it off
   gives zero.  That isolates the bug to the userspace VMM (QEMU)
   rather than KVM itself — useful when arguing for the QEMU/libvirt
   patches that surface the bit.

Build
=====

Enable the Kconfig symbol::

  CONFIG_UM_BACKEND_KVM_V2=y
  CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y

The symbol gates three things: the ``KVM_ENABLE_CAP`` call in
``arch/um/backend/kvm-v2/context.c::kvm_v2_vm_create``, the boot-param
parser, and the debugfs probe.  With ``=n`` the backend behaves
exactly as before — no ioctl issued, guest rdmsr returns zero.

Boot
====

The feature is **on by default** when the Kconfig is ``=y``.  Two
ways to flip it from the cmdline:

================================== =========================================
``kvm_v2_aperfmperf=on``           Force passthrough on (default when Kconfig=y)
``kvm_v2_aperfmperf=off``          Force passthrough off
================================== =========================================

The Kconfig must also be ``=y`` for either override to apply — when
``=n`` the cmdline parser is not compiled in and the bit is never
sent regardless.

You must also actually run under ``kvm-v2``::

  ./linux backend=force=kvm-v2 [...other params...]

Under the ``seccomp`` backend the UML kernel runs directly on the
host without a KVM context, so rdmsr touches the real host CPU and
the toggle has no meaning.  The selftest gates on
``backend=force=kvm-v2`` for that reason.

Host requirements
=================

The host CPU must have ``X86_FEATURE_APERFMPERF`` set.  KVM's
``kvm_get_allowed_disable_exits()`` masks the bit off when the host
CPU lacks the feature; the ``KVM_ENABLE_CAP`` then returns
``-EINVAL``.  This is handled non-fatally: ``kvm_v2_vm_create()``
logs a warning and continues.  Visible behavior in that case is
identical to the Kconfig=n / boot-param-off path — guest rdmsr
returns zero.

Check via::

  grep -o aperfmperf /proc/cpuinfo | head -1

If the line is empty, the host CPU cannot pass the bit through.
Most Intel CPUs from Nehalem (2008) onward and AMD CPUs from
Bulldozer (2011) onward have the feature.

Ordering constraint
===================

``KVM_CAP_X86_DISABLE_EXITS`` is rejected with ``-EINVAL`` once any
vCPU has been created — KVM enforces ``kvm->created_vcpus == 0`` in
``arch/x86/kvm/x86.c::kvm_vm_ioctl_enable_cap``.  The v2 backend
issues the cap from ``kvm_v2_vm_create`` (which runs in
``init_backend()``), strictly before ``kvm_v2_vcpu_create`` (which
runs immediately after).  Do not move the ioctl past that boundary.

Verifying from inside the guest
===============================

The backend exposes a debugfs read file that issues rdmsr from the
UML kernel context — which IS the KVM guest's CPL=0 context under
the ``kvm-v2`` backend.  From inside the running guest::

  # cat /sys/kernel/debug/um/kvm_v2/aperf_mperf
  toggle=on
  aperf_rc=0
  mperf_rc=0
  aperf=84823592197
  mperf=83110028441
  ratio_pct=102

With the feature **off** (boot with ``kvm_v2_aperfmperf=off`` or
build with the Kconfig=n) the output reads::

  toggle=off
  aperf_rc=0
  mperf_rc=0
  aperf=0
  mperf=0
  ratio_pct=n/a

The non-zero / zero split is exactly the symptom Anderson reported
against QEMU+libvirt.

Selftest
========

A boot-and-read kselftest lives at
``tools/testing/selftests/um/aperf-mperf-smoke/``.  It boots a
``kvm-v2`` UML kernel, reads the debugfs probe twice with a busy
delay in between, and asserts:

1. Both probes return ``aperf > 0`` and ``mperf > 0``.
2. The second probe's counters strictly exceed the first.
3. The ratio ``aperf / mperf`` is within a sane range (0.5 .. 2.0).

Run::

  cd tools/testing/selftests/um/aperf-mperf-smoke
  ./run.sh /path/to/uml-kernel-binary

The test prints ``PASS`` on success and ``FAIL: <reason>`` on
failure.  The kernel binary must be built with
``CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y`` and the host
CPU must support ``X86_FEATURE_APERFMPERF``; both prerequisites are
checked at the start of the script.

Bridging back to QEMU / libvirt
===============================

For maintainers who want to fix the QEMU side after using ``kvm-v2``
as the repro, the patch lives in
``target/i386/kvm/kvm.c::kvm_arch_init`` (or wherever ``kvm_arch_init``
calls ``kvm_vm_ioctl(KVM_ENABLE_CAP, ...)`` with
``KVM_CAP_X86_DISABLE_EXITS``).  Today that path ORs in
``KVM_X86_DISABLE_EXITS_HLT | _MWAIT | _PAUSE | _CSTATE`` based on
``-overcommit cpu-pm=on``; the change is to add a new machine
property (suggested name: ``cpu-pm-aperf=on``) that ORs in
``KVM_X86_DISABLE_EXITS_APERFMPERF`` with the same ``before any
vCPU is created`` ordering.

libvirt would then surface the knob as a sub-element of
``<features><kvm>``.

See also
========

- ``arch/um/backend/kvm-v2/aperfmperf.c`` — the toggle + probe.
- ``arch/um/backend/kvm-v2/context.c::kvm_v2_vm_create`` — the
  ``KVM_ENABLE_CAP`` call site.
- ``include/uapi/linux/kvm.h`` — flag definitions
  (``KVM_X86_DISABLE_EXITS_APERFMPERF``).
- ``arch/x86/kvm/x86.c::kvm_vm_ioctl_enable_cap`` —
  KVM-side handler, including the ``created_vcpus`` constraint.
- ``tools/testing/selftests/um/aperf-mperf-smoke/`` — kselftest.
