.. SPDX-License-Identifier: GPL-2.0

####################
UML Backends
####################

.. contents:: :local:

User-Mode Linux runs the upstream Linux kernel as a host process.
"Backend" is the host-side mechanism the UML kernel uses to
intercept its guest's syscalls, page faults, and signals — the
plumbing between the host kernel and the UML kernel.

One backend ships today; a KVM backend is being reimplemented:

==========  =====================================  ============================
Backend     Mechanism                              Per-syscall cost on bare metal
==========  =====================================  ============================
seccomp     ``SECCOMP_RET_TRAP`` + ``SIGSYS`` +    ~300–500 ns (seccomp filter
            futex round-trip                       hits, futex wakes UML kernel)
kvm         ``KVM_RUN`` + per-mapping memslots +   target ~100 ns (workstream D
            TDP via host ``mm->pgd``               v2 — in development)
==========  =====================================  ============================

seccomp landed in 6.16 (Benjamin Berg). kvm v1 reached integration
but hit structural shadow-PT issues; the implementation is archived
at ``arch/um/backend/kvm-v1-archive/`` (not built;
``CONFIG_UM_BACKEND_KVM_V1_ARCHIVE`` depends on ``BROKEN``) and the
v2 reimplementation is being built at ``arch/um/backend/kvm-v2/``.
See ``Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/``
memos 24-26 for design and 27 for the execution prompt.

The historical **ptrace** backend (``PTRACE_SYSEMU`` + ``waitpid``,
Jeff Dike, 1990s) was removed in memo 25 refactor 11. The archived
source is reachable via
``git show kvm-v1-archive-20260428:arch/um/backend/ptrace/``.
Hosts that genuinely lack ``CONFIG_SECCOMP_FILTER`` (mainline since
3.5 / 2012) should pin to UML v6.16 or earlier.

******************
Picking a backend
******************

Default builds compile in seccomp; the v2 KVM backend is in
development (memo 26). Today's defaults give the typical user the
right behavior without thinking about it:

============================  ==============================================
You want…                     Use
============================  ==============================================
"It just works"               No flags needed. Default config compiles in
                              seccomp; ``backend=auto`` selects it.
Maximum speed                 ``backend=seccomp``. (KVM v2 will rejoin this
                              spectrum once memo 26 Phase A wires it.)
Smallest binary / minimum     Build with
TCB (sandbox profile)         ``CONFIG_UM_BACKEND_SECCOMP_ONLY=y``. The
                              dispatch macro inlines to direct calls.
A binary that runs            ``CONFIG_UM_BACKEND_DYNAMIC=y`` (default
anywhere                      when seccomp is available).
Force a specific backend      ``backend=force=seccomp``. Panics during
(panic if unavailable)        early init if seccomp probe fails.
============================  ==============================================

******************
Boot parameters
******************

``backend=<auto|seccomp|kvm|force=seccomp|force=kvm>``
    Pick the trap mechanism. ``auto`` (default) runs the host
    seccomp probe at boot and picks seccomp when the host supports
    it. ``force=`` makes the choice mandatory and panics if the
    requested backend isn't compiled in or fails its host probe.
    ``kvm`` currently has no selectable backend — v1 archived to
    ``arch/um/backend/kvm-v1-archive/`` (depends on ``BROKEN``);
    v2 stub at ``arch/um/backend/kvm-v2/`` is not yet wired into
    dispatch (memo 26 Phase A.1 plumbs it in). Today
    ``backend=kvm`` falls back to seccomp and
    ``backend=force=kvm`` panics.

    ``backend=ptrace`` is parsed for compatibility but the ptrace
    backend was removed (memo 25 R11; archived at the
    ``kvm-v1-archive-20260428`` tag). Requests warn and fall
    through to seccomp; ``backend=force=ptrace`` panics with a
    pin-to-v6.16-or-earlier hint.

``seccomp=<on|auto|off>`` (legacy alias)
    Preserved for one transitional release. Maps to:
    ``seccomp=on`` → ``backend=force=seccomp``;
    ``seccomp=auto`` → ``backend=auto``;
    ``seccomp=off`` is now equivalent to ``backend=auto`` since
    ptrace is no longer a fallback.

******************
Trap path diagrams
******************

ptrace (archived)
=================

The ptrace backend was removed in memo 25 R11. Source is
preserved at the ``kvm-v1-archive-20260428`` tag::

    git show kvm-v1-archive-20260428:arch/um/backend/ptrace/trap_user.c

The trap path was the classic ``PTRACE_SYSEMU`` + ``waitpid`` +
``PTRACE_GETREGS`` + ``PTRACE_SETREGS`` + ``PTRACE_CONT`` round
trip, ~1–5 µs per syscall.

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

Per-backend file layout (current — seccomp; v2 KVM follows the
same shape per memo 26):

==================================  ==============================
File                                Op(s)
==================================  ==============================
``<kind>_backend.c``                ``struct um_backend_ops``
                                    singleton + ops fields
``trap_user.c`` (USER TU)           ``vcpu_run`` (the trap loop)
``mm.c``                            ``mm_create``, ``mm_destroy``,
                                    ``mm_region_added``,
                                    ``mm_region_removed``,
                                    ``mm_region_protected``
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

The HOT ops (``vcpu_run``, ``mm_region_added``,
``mm_region_removed``, ``context_switch``, ``read_clock_ns``) are
validated non-NULL at ``init_backend()`` time;
``mm_region_protected`` is optional (mm-arbiter falls back to
remove+add when NULL).
==================================  ==============================

******************
Compatibility
******************

==============  ==========================================================
Backend         Host requirements
==============  ==========================================================
seccomp         Host with ``CONFIG_SECCOMP_FILTER=y`` (mainline since
                3.5 / 2012) and seccomp filter installation permitted
                (typically requires no special privileges; some
                hardened/sandboxed environments may forbid it).
                Linux 6.16+ for the in-tree UML stub layout.
kvm             Host with ``/dev/kvm`` accessible to the running user.
                Linux 5.x+. (Workstream D — v1 archived, v2 in
                development per memo 26.)
ptrace          Removed (memo 25 R11). Pin to UML v6.16 or earlier
                if you need it.
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
