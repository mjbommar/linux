.. SPDX-License-Identifier: GPL-2.0

####################
UML Backends
####################

.. contents:: :local:

User-Mode Linux runs the upstream Linux kernel as a host process.
"Backend" is the host-side mechanism the UML kernel uses to
intercept its guest's syscalls, page faults, and signals — the
plumbing between the host kernel and the UML kernel.

Two backends ship today; a third is in development:

==========  =====================================  ============================
Backend     Mechanism                              Per-syscall cost on bare metal
==========  =====================================  ============================
ptrace      ``PTRACE_SYSEMU`` + ``waitpid``        ~1–5 µs (trap + dispatch)
seccomp     ``SECCOMP_RET_TRAP`` + ``SIGSYS`` +    ~300–500 ns (seccomp filter
            futex round-trip                       hits, futex wakes UML kernel)
kvm         ``KVM_RUN`` + ``MSR_LSTAR`` direct     ~100 ns target (gVisor-style;
            ring-0 entry                           workstream D)
==========  =====================================  ============================

ptrace is the historical UML mechanism (Jeff Dike, 1990s). seccomp
landed in 6.16 (Benjamin Berg). kvm is in design.

******************
Picking a backend
******************

Default builds compile in both ptrace and seccomp; the active
backend is chosen at boot. The defaults are sized so the typical
user gets the right behavior without thinking about it:

============================  ==============================================
You want…                     Use
============================  ==============================================
"It just works"               No flags needed. Default config compiles in
                              both backends; ``backend=auto`` picks ptrace
                              unless ``seccomp=on`` is also set.
Maximum speed                 ``backend=seccomp`` (or ``seccomp=on`` legacy).
                              ~3–4× faster than ptrace on syscall-heavy
                              workloads. Requires host seccomp filter
                              support.
Smallest binary / minimum     Build with ``CONFIG_UM_BACKEND_PTRACE_ONLY=y``
TCB (sandbox profile)         or ``CONFIG_UM_BACKEND_SECCOMP_ONLY=y``. The
                              unselected backend is excluded entirely;
                              dispatch inlines to direct calls.
A binary that runs            ``CONFIG_UM_BACKEND_DYNAMIC=y`` (default
anywhere                      when seccomp is available). Both backends
                              compiled in; the active one is picked at
                              boot via the host probe + ``backend=`` boot
                              param.
Force a specific backend      ``backend=force=ptrace`` or
(panic if unavailable)        ``backend=force=seccomp``. The kernel
                              panics during early init if the requested
                              backend isn't compiled in or its host probe
                              fails.
============================  ==============================================

******************
Boot parameters
******************

``backend=<auto|ptrace|seccomp|force=ptrace|force=seccomp>``
    Pick the trap mechanism. ``auto`` (default) defers to the legacy
    ``seccomp=`` alias. ``backend=seccomp`` triggers the host
    seccomp probe even if ``seccomp=`` is unset, and falls back to
    ptrace if the probe fails. ``force=`` makes the choice mandatory
    and panics if the requested backend isn't available.

``seccomp=<on|auto|off>`` (legacy alias)
    Preserved for one transitional release. Maps to:
    ``seccomp=on`` → ``backend=force=seccomp``;
    ``seccomp=auto`` → ``backend=auto``;
    ``seccomp=off`` → ``backend=ptrace``.

******************
Trap path diagrams
******************

ptrace
======

.. code-block::

    UML host process            stub child (per-mm)
    ────────────────            ──────────────────────
                                 1. Issues guest syscall
    ─── PTRACE_SYSEMU ────────► 2. Hardware traps to host
    ─── waitpid ──────────────► 3. Stub stops at SYSEMU pre-exec
    ◄── kernel runs syscall   ─ 4. PTRACE_GETREGS reads syscall #
                                 (handle_syscall in arch/um/kernel/
                                 skas/syscall.c)
    ─── PTRACE_SETREGS ───────► 5. Write return value
    ─── PTRACE_CONT ──────────► 6. Resume stub past the syscall

Round-trip cost: 4 host syscalls (SYSEMU + waitpid + GETREGS +
SETREGS) plus signal-stack fault delivery for SIGSEGV. ~1–5 µs.

seccomp
=======

.. code-block::

    UML host process            stub child (per-mm)
    ────────────────            ─────────────────────────
                                 1. Issues guest syscall
                                 2. Seccomp filter returns SIGSYS
                                 3. SIGSYS handler in stub copies
                                    regs/siginfo to shared
                                    `stub_data->sigstack[]`
                                 4. Stub waits on futex
    ◄── futex wake ───────────  5. UML kernel sees futex
    [kernel runs syscall in
     handle_syscall, then
     writes new regs into
     stub_data + clears futex]
    ─── futex wake ───────────► 6. Stub resumes, reads new regs

Round-trip cost: 1 SIGSYS delivery + 1 futex wait/wake pair +
shared-memory copy. ~300–500 ns.

kvm (workstream D)
==================

.. code-block::

    UML host process               KVM guest
    ───────────────                ────────────────────────
                                    1. Guest userspace runs
                                    2. SYSCALL instruction →
                                       MSR_LSTAR direct entry
                                       to UML-kernel ring-0
                                    3. UML kernel handles
                                       syscall in-VM
                                    4. SYSRET back to guest
    [kernel only sees a VMEXIT
     when guest does HLT or
     VMCALL — typically only
     for I/O or scheduling]

Round-trip cost: hardware-fast LSTAR entry. No host VMEXIT for
typical syscalls. Target: ~100 ns.

******************
Implementation map
******************

Code lives under ``arch/um/backend/<kind>/``. Each backend
implements the ops table declared in
``arch/um/include/shared/backend.h`` (``struct um_backend_ops``).
The kernel-side selection arbiter is ``init_backend()`` in
``arch/um/kernel/backend.c`` — it runs once at boot, validates the
chosen backend's HOT ops are non-NULL, and sets the global
``um_backend`` pointer.

Per-backend file layout (ptrace + seccomp follow the same shape):

==================================  ==============================
File                                Op(s)
==================================  ==============================
``backend.c``                       ``struct um_backend_ops``
                                    singleton + 18 op fields
``trap_user.c`` (USER TU)           ``run_userspace`` (the trap loop)
``mm.c``                            ``mm_attach``, ``mm_detach``,
                                    ``mm_map``, ``mm_unmap``
``thread.c``                        ``context_switch``,
                                    ``thread_create``,
                                    ``thread_start_idle``,
                                    ``ipi_send``
``time.c``                          ``read_clock_ns``,
                                    ``set_timer``,
                                    ``read_persistent_clock_ns``
``debug.c`` / ``debug_user.c``      ``init_thread_regs``,
                                    ``read_guest_regs``,
                                    ``write_guest_regs``
``lifecycle.c``                     ``probe``, ``init``,
                                    ``shutdown``
==================================  ==============================

******************
Compatibility
******************

==============  ==========================================================
Backend         Host requirements
==============  ==========================================================
ptrace          Any Linux host with PTRACE_SYSEMU support. Works on
                kernels back to 3.x.
seccomp         Host with ``CONFIG_SECCOMP_FILTER=y`` and seccomp filter
                installation permitted (typically requires no special
                privileges; some hardened/sandboxed environments may
                forbid it). Linux 6.16+ for the in-tree UML stub layout.
kvm             Host with ``/dev/kvm`` accessible to the running user.
                Linux 5.x+. (Workstream D — not yet shipped.)
==============  ==========================================================

******************
For developers
******************

If you want to write a third backend (KVM, or something else), see
:ref:`UML Backend Ops Contract <backend-contract>` for the
``struct um_backend_ops`` API and its semantics. Implement each op,
register a singleton, add a Kconfig option and ``init_backend()``
case, and run the conformance suite under
``arch/um/backend/contract/``.

For the broader architectural plan that produced this design, see
``Documentation/virt/uml/redesign/``.
