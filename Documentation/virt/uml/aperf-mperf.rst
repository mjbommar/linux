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
QEMU support for the same bit.  Two practical uses:

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

The backend exposes a debugfs **status** file that reports the
architectural plumbing — toggle state, whether vm_create issued the
ioctl, the ioctl return code, host CPU feature availability — so a
userspace test can confirm the cap was actually requested and
accepted.  The probe does NOT issue rdmsr from the UML kernel
context: UML's "kernel" code runs as a host userspace process at
host CPL=3, where rdmsr always ``#GP``s regardless of the
disable-exits bit.  The bit only affects code running at GUEST
CPL=0 inside the KVM guest — for a real Linux guest under QEMU,
that means the guest kernel.

From inside the running guest::

  # cat /sys/kernel/debug/um/kvm_v2/aperf_mperf
  toggle=on
  ioctl_attempted=1
  ioctl_rc=0
  host_feature_aperfmperf=1
  status=enabled

The ``status`` line decodes to one of four values:

================== ===========================================================
``enabled``        vm_create issued KVM_ENABLE_CAP and KVM accepted it.
                   APERF/MPERF rdmsr at guest CPL=0 will pass through to
                   hardware.
``rejected``       vm_create issued the ioctl but KVM refused (rare; usually
                   an SMT-RSB-mitigation overlap — see
                   ``arch/x86/kvm/x86.c::kvm_vm_ioctl_enable_cap``).
``host_no_feature`` vm_create issued the ioctl but KVM masked the bit off
                   because ``boot_cpu_has(X86_FEATURE_APERFMPERF)`` is false.
``disabled``       vm_create skipped the ioctl entirely — either the Kconfig
                   is ``=n`` or the cmdline overrode the default to off.
================== ===========================================================

With the feature **off** (boot with ``kvm_v2_aperfmperf=off`` or
build with the Kconfig=n) the output reads::

  toggle=off
  ioctl_attempted=0
  ioctl_rc=0
  host_feature_aperfmperf=1
  status=disabled

To actually read APERF/MPERF *values* from a guest, see the example
README at ``Documentation/virt/uml/examples/aperf-mperf/README.md``
under "Adapting for guest-side counter reads".  Three options
documented: real Linux guest under QEMU (the upstream-VMM case), an LSTAR
gadget extension, or in-kernel nested KVM.

Selftest
========

A boot-and-read kselftest lives at
``tools/testing/selftests/um/aperf-mperf-smoke/``.  It boots a
``kvm-v2`` UML kernel, runs the freestanding demo at
``Documentation/virt/uml/examples/aperf-mperf/aperf-mperf-demo`` as
``init=``, reads the status probe, and asserts the demo emits
``APERF_MPERF_DEMO: PASS plumbing_ok=1`` -- i.e., vm_create issued
``KVM_ENABLE_CAP`` and KVM accepted the cap.  It does NOT assert
non-zero counter values (see "Verifying from inside the guest"
above for why).

Run::

  cd tools/testing/selftests/um/aperf-mperf-smoke
  UML_BINARY=/path/to/uml-kernel-binary ./run-aperf-mperf-smoke.sh

The test prints ``PASS`` on success and ``FAIL: <reason>`` on
failure.  Skips when the kernel binary or demo binary is absent,
when ``/dev/kvm`` is unreadable, or when the host CPU lacks
``X86_FEATURE_APERFMPERF``.

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
