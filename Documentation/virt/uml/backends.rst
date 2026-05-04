.. SPDX-License-Identifier: GPL-2.0

####################
UML Backends
####################

.. contents:: :local:

User-Mode Linux runs the upstream Linux kernel as a host process.
"Backend" is the host-side mechanism the UML kernel uses to
intercept its guest's syscalls, page faults, and signals — the
plumbing between the host kernel and the UML kernel.

Two backends are buildable today; a third is archived:

==========  =====================================  ============================
Backend     Mechanism                              Per-syscall cost on bare metal
==========  =====================================  ============================
seccomp     ``SECCOMP_RET_TRAP`` + ``SIGSYS`` +    ~300–500 ns (seccomp filter
            futex round-trip                       hits, futex wakes UML kernel)
kvm-v2      ``KVM_RUN`` + IDT/IST exception        ~150 ns measured on minimal
            delivery + IO-port syscall trap        Python startup (~2× faster
                                                   than seccomp; memo 26 §H.1)
==========  =====================================  ============================

seccomp landed in 6.16 (Benjamin Berg). kvm v2 reached substrate
parity with seccomp at the 2026-04-30 milestone (PASS=25 / FAIL=3 /
EXPECTED_FAIL=3 on the ``regrtest-substrate`` gate, bit-for-bit
match). v2 still defaults to ``n`` because Phase J validation (24h
continuous, Tier 1/2/3 third-party libs, soak) is open; production
runtimes can opt in via ``backend=force=kvm-v2``. The archived v1
implementation lives at ``arch/um/backend/kvm-v1-archive/`` (not
built; ``CONFIG_UM_BACKEND_KVM_V1_ARCHIVE`` depends on ``BROKEN``).
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
Maximum speed                 Build with ``CONFIG_UM_BACKEND_KVM_V2=y`` and
                              boot ``backend=force=kvm-v2``. ~2× faster than
                              seccomp on minimal Python startup; full
                              substrate-gate parity at the 2026-04-30
                              milestone. Phase J validation still pending —
                              not yet the runtime default.
Smallest binary / minimum     Build with
TCB (sandbox profile)         ``CONFIG_UM_BACKEND_SECCOMP_ONLY=y``. The
                              dispatch macro inlines to direct calls.
A binary that runs            ``CONFIG_UM_BACKEND_DYNAMIC=y`` (default
anywhere                      when seccomp is available).
Force a specific backend      ``backend=force=seccomp`` /
(panic if unavailable)        ``backend=force=kvm-v2``. Panics during
                              early init if the chosen backend's host probe
                              fails.
============================  ==============================================

******************
Boot parameters
******************

``backend=<auto|seccomp|kvm-v2|force=seccomp|force=kvm-v2>``
    Pick the trap mechanism. ``auto`` (default) runs the host
    seccomp probe at boot and picks seccomp when the host supports
    it. ``force=`` makes the choice mandatory and panics if the
    requested backend isn't compiled in or fails its host probe.
    ``kvm-v2`` is at substrate parity with seccomp (memo 26 §H.1b,
    2026-04-30) and **3-4× faster on Python startup, 193-908×
    faster on per-syscall round-trip** with the Phase H LSTAR
    gadget enabled (default; see ``CONFIG_UM_BACKEND_KVM_V2_GADGET``
    and the cross-host bench at
    ``Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/bench-cross-host-2026-05-04-postgadget.md``).
    Opt in via ``backend=force=kvm-v2`` until Phase J validation
    flips it to default. The archived v1 implementation
    (``arch/um/backend/kvm-v1-archive/``, depends on ``BROKEN``) is
    not selectable.

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

kvm-v2 (workstream D)
=====================

.. code-block::

    UML host process               KVM guest
    ───────────────                ────────────────────────
                                    1. Guest userspace runs at CPL=3
                                    2. SYSCALL instruction → MSR_LSTAR
                                       trampoline (CPL=0, kernel CS) at
                                       PML4[508] kernel-half VA
                                    3. Trampoline: ``out %al,$0xf4``
                                       → KVM_EXIT_IO
    4. KVM_RUN returns to host;
       handle_io_trap → handle_syscall
       runs the syscall on the host
       UML kernel; marshals result back
       into kvm_run->s.regs via
       SYNC_REGS dirty bits
    5. KVM_RUN re-enters; trampoline
       SYSRETQ drops back to CPL=3
                                    6. Guest userspace continues

Exception delivery follows the same pattern via the IDT in
PML4[508] (``arch/um/backend/kvm-v2/exception.c``): #PF / #GP / #UD
/ #DE / #OF dispatch through per-vector handler stubs that ``out``
to the host then ``iretq`` back to the guest fault-resume
instruction.

Round-trip cost on the slow KVM_EXIT_IO path: ~150 ns measured,
dominated by the KVM_EXIT_IO ioctl pair.

Phase H gadget (default, ``CONFIG_UM_BACKEND_KVM_V2_GADGET=y``):
ten trivial syscalls (getpid/gettid/getppid/getuid/geteuid/getgid/
getegid/getcpu/time/clock_gettime CLOCK_MONOTONIC) handle entirely
in-guest via an in-LSTAR dispatch tree. The body reads per-task
state from a per-vCPU page (mapped at MSR_KERNEL_GS_BASE,
populated in ``load_user_sregs``) and returns via SYSRETQ — no
vmexit. Per-syscall round-trip drops from ~36 800 cyc to ~90 cyc
(see ``arch/um/backend/kvm-v2/lstar_gadget.S`` for the assembled
trampoline body and the cross-host bench at
``Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/bench-cross-host-2026-05-04-postgadget.md``
for measurements).

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
kvm-v2          Host with ``/dev/kvm`` accessible to the running user
                (typically /dev/kvm group membership or
                ``setfacl -m u:$(id -un):rw /dev/kvm``).  x86_64 host
                CPU with VT-x or SVM. Linux 5.x+ for the KVM API
                surface (KVM_CAP_SYNC_REGS, KVM_SET_USER_MEMORY_REGION).
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
