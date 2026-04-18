.. SPDX-License-Identifier: GPL-2.0

.. _backend-contract:

##########################
UML Backend Ops Contract
##########################

.. contents:: :local:

This document specifies the interface every User-Mode Linux backend
must implement: the function-pointer table ``struct um_backend_ops``
declared in ``arch/um/include/shared/backend.h``.

The contract version covered by this document is
``UM_BACKEND_CONTRACT_VERSION = 1``.

For end-user documentation (which backend to pick, the boot
parameters, the trap-path diagrams), see
:doc:`UML Backends <backends>`.

************
Overview
************

A UML backend is a host-side trap mechanism that lets a user-mode
Linux instance intercept its guest's syscalls, page faults, signals,
and context switches. Two backends are currently in tree:

- ``ptrace`` — the original SKAS mechanism. Works everywhere Linux
  ptrace works. ~1–5 µs per syscall round-trip on bare metal.
- ``seccomp`` — merged in 6.16. Trades a small in-guest BPF filter
  for ~3–4× faster traps via SIGSYS + futex. ~300–500 ns per syscall.

A KVM backend (~100 ns syscall via gVisor-style ring-0 entry) is
in design.

Each backend is a single immutable instance of ``struct
um_backend_ops``. Only one backend is active per UML kernel instance,
selected once at boot by ``init_backend()``.

***********************
The ops table
***********************

The full struct definition lives in
``arch/um/include/shared/backend.h``. The struct is declared in
the shared header (rather than in ``arch/um/include/asm/``) because
USER-side translation units in ``arch/um/`` need to deref
``um_backend->op`` in dynamic builds. The kernel-only
``init_backend()`` API lives in ``arch/um/include/asm/backend.h``.

This document specifies the **semantics** of each op.

Conventions
===========

- All ops are **synchronous**. There is no completion handle and no
  polling.
- Ops marked **HOT** are inlinable in single-backend builds via the
  ``um_backend_dispatch()`` macro. They MUST be non-NULL on every
  in-tree backend; ``init_backend()`` validates this at boot and
  panics otherwise.
- **All other (cold) ops MUST also be non-NULL.** The dispatch macro
  is a plain function-pointer call with no NULL check — it cannot
  synthesize ``-ENOSYS`` for a missing ``void`` or ``u64`` op.
  Backends that don't yet implement a cold op should provide a thin
  wrapper that returns ``-EOPNOTSUPP`` (for ops returning ``int``)
  rather than leave the field NULL. See the ptrace and seccomp
  backends' ``read_guest_regs`` / ``write_guest_regs`` for examples
  pending KGDB integration (workstream C-11).
- Errno conventions follow standard kernel style: 0 on success,
  negative ``errno`` on failure.

Per-op semantics
================

Lifecycle and trap (4 ops)
--------------------------

``probe(void)``
    Cold. Returns 0 if this backend can run on the current host
    (hardware capability + kernel feature checks), or a negative
    errno otherwise. Must be cheap (≤1 ms typical) and side-effect
    free beyond opening probe FDs that are immediately closed.

``init(const struct um_backend_args *args)``
    Cold. Called once after this backend wins the arbiter. Sets up
    backend-private state (per-backend singleton). May fail with a
    negative errno; on failure the kernel panics.

``shutdown(void)``
    Cold. Called from the reboot/halt path. Releases backend
    resources. Must not sleep; called with interrupts off in some
    paths.

``run_userspace(struct uml_pt_regs *regs)``
    **HOT.** The trap loop. Resumes guest userspace with the state
    in ``*regs``, runs until the next trap (syscall, page fault,
    signal, IRQ injection), then returns with ``regs`` updated and
    ``regs->faultinfo`` populated for fault-bearing exits.

    The kernel-side trap handlers (``handle_syscall``, ``segv``,
    ``relay_signal``) are invoked **by** the backend from inside
    ``run_userspace`` once the trap reason is known. Backends should
    not return to the caller without invoking the appropriate
    handler.

Memory (4 ops)
--------------

``mm_attach(struct mm_id *id)``
    Cold. Called once per ``init_new_context()`` after the kernel
    has allocated ``mm->context.id``. Sets up per-mm backend state
    (stub child for ptrace/seccomp; KVM memory slot + guest pgd for
    KVM). The backend records its private fields in ``*id``; see
    `mm_id field ownership`_ below.

``mm_detach(struct mm_id *id)``
    Cold. Called from ``destroy_context()`` and the reboot path.
    Tears down everything ``mm_attach`` created.

``mm_map(id, va, len, prot, phys_fd, offset)``
    **HOT.** Map a contiguous host-backing region into the guest mm
    at virtual address ``va``. ``phys_fd``/``offset`` identify the
    host-side backing; ``prot`` is a ``PROT_*`` mask. Returns 0 or
    negative errno.

    Backends may queue and amortize multiple ``mm_map`` calls into
    one stub round-trip; they must flush implicitly before the next
    ``run_userspace()`` returns to the guest.

``mm_unmap(id, va, len)``
    **HOT.** Symmetric. Same flush semantics as ``mm_map``.

Scheduling (4 ops)
------------------

``thread_create(p, stack, handler)``
    Cold. Per-fork. Sets up ``p->thread`` with whatever per-thread
    state the backend needs (jmp_buf for ptrace/seccomp; vCPU
    descriptor for KVM). ``handler`` is the entry function the
    new thread will run.

``thread_start_idle(stack, t)``
    Cold. Boot-only. Brings up the boot CPU's idle thread. After
    return, the kernel's normal scheduling takes over.

``context_switch(prev, next)``
    **HOT.** Switch from ``prev`` to ``next``. Backends extract their
    per-thread state from the task pointers; the abstraction is
    deliberately at task-level, not at jmp_buf or vCPU level.

``ipi_send(cpu, vector)``
    Cold. Send a UML IPI vector to a target CPU. For ``cpu ==
    smp_processor_id()`` and ``vector == UML_IPI_PREEMPT``, this
    is a "kick self" used by the timer interrupt path.

Time (3 ops)
------------

``read_clock_ns(void)``
    **HOT.** Returns the host monotonic clock in nanoseconds. Backs
    the UML clocksource.

``set_timer(cpu, deadline_ns, mode)``
    Cold. Configure the host timer for ``cpu``:

    - ``UM_TIMER_DISABLE`` — disable; ``deadline_ns`` ignored.
    - ``UM_TIMER_ONE_SHOT`` — fire once after ``deadline_ns``
      nanoseconds (relative).
    - ``UM_TIMER_PERIODIC`` — fire every ``deadline_ns`` nanoseconds.

``read_persistent_clock_ns(void)``
    Cold. Returns the host wall clock in nanoseconds. Used for boot
    time and persistent-clock emulation.

Debug / introspection (3 ops)
-----------------------------

``init_thread_regs(gp, fp)``
    Cold. Populate a fresh thread's general-purpose and FP register
    arrays with backend-safe defaults. (For ptrace/seccomp, these
    are derived from the boot probe; for KVM, from the vCPU initial
    state.)

``read_guest_regs(t, regs)``
    Cold. Read the live guest register state for task ``t`` into
    ``*regs``. Used by KGDB and debugger tooling.

``write_guest_regs(t, regs)``
    Cold. Write ``*regs`` into the live guest register state for
    task ``t``.

***********************
Backend-private state
***********************

Backends hold state in three places:

1. **Module-private static state** in ``arch/um/backend/<kind>/``.
   Holds the singleton things like the KVM ``vm_fd``, the seccomp
   filter program, the ptrace exec-template registers.
2. **Per-mm state** in ``struct mm_id``. See ownership table below.
3. **Per-thread state** in ``task->thread``. The backend defines
   what fields it needs; ``struct thread_struct`` carries a union
   of the per-backend layouts.

There is **no** ``void *backend_private`` slot. Allocator round-
trips and ownership ambiguity outweigh the flexibility.

mm_id field ownership
=====================

``struct mm_id`` (declared in ``arch/um/include/shared/skas/mm_id.h``)
carries fields used by different backends. **All fields are present
in every build**, including builds that don't compile in the backend
that uses them (the cost is ≤24 bytes per mm).

==================== ============= ==================================
Field                Owner         Use
==================== ============= ==================================
``stack``            all           per-mm scratch page (stub_data
                                   for ptrace/seccomp; memslot base
                                   for KVM)
``pid``              ptrace, secc. host child PID (-1 for KVM)
``syscall_data_len`` seccomp       length of pending stub syscalls
``sock``             seccomp       SCM_RIGHTS socket
``syscall_fd_num``   seccomp       FDs queued for next stub batch
``syscall_fd_map[]`` seccomp       FD slot table
*(planned)*
``kvm_pgd``          kvm           guest CR3 cookie
``memslot_id``       kvm           KVM memory slot index
==================== ============= ==================================

The fields are not unioned because they are inspected at runtime
from cross-backend code paths (``destroy_context`` checks
``id.sock``, etc.) and a union would require ``um_backend->kind``
checks at every access site. The byte cost is negligible.

***********************
Versioning policy
***********************

``UM_BACKEND_CONTRACT_VERSION`` (``u32`` in
``arch/um/include/shared/backend.h``) and the matching
``contract_version`` field of ``struct um_backend_ops`` follow this
policy:

- **Bump** on any change to the struct: adding ops, removing ops,
  changing op signatures. All in-tree backends migrate in lockstep
  with the bump.
- The dispatch macro is a plain function-pointer call — there is no
  NULL-op fallback. Adding an op without populating it on every
  in-tree backend is a build/runtime regression; init_backend()'s
  HOT-op validator catches the HOT subset at boot, but cold ops
  must be populated by convention.

The ABI is **internal**: it is not exposed to user space and not
guaranteed across kernel versions. Out-of-tree backends are not a
supported use case; in-tree backends migrate in lockstep with the
contract.

***********************
Selection mechanism
***********************

Build-time
==========

One of these is set:

- ``CONFIG_UM_BACKEND_PTRACE_ONLY`` — only ptrace; direct calls.
- ``CONFIG_UM_BACKEND_SECCOMP_ONLY`` — only seccomp; direct calls.
- ``CONFIG_UM_BACKEND_KVM_ONLY`` — only KVM; direct calls.
- ``CONFIG_UM_BACKEND_DYNAMIC`` — multi-backend; indirect calls
  through ``um_backend``.

Single-backend builds inline the chosen backend's symbols directly,
producing zero-overhead dispatch and a smaller binary. Used by
sandbox and embedded profiles. Multi-backend builds let the boot
arbiter pick.

Boot-time (multi-backend builds)
================================

In ``CONFIG_UM_BACKEND_DYNAMIC`` builds the active backend is chosen
at boot from the kernel command line:

- ``backend=auto`` — **Default.** Defers to the legacy ``seccomp=``
  alias: if ``seccomp=on`` or ``seccomp=auto`` was passed AND its
  probe succeeded, picks seccomp; otherwise ptrace. (In a future
  release ``auto`` may be changed to *unconditionally* probe
  seccomp first, matching the gVisor pattern; for now it preserves
  the historical "ptrace unless asked otherwise" behavior.)
- ``backend=ptrace`` — pick ptrace; the seccomp probe is skipped
  unless legacy ``seccomp=`` requested it independently.
- ``backend=seccomp`` — pick seccomp; the seccomp probe is run even
  if ``seccomp=`` is unset. Falls back to ptrace if the probe fails
  on this host.
- ``backend=force=ptrace`` — require ptrace; **panic** if the
  ptrace backend isn't compiled in.
- ``backend=force=seccomp`` — require seccomp; the probe runs (no
  need to also set ``seccomp=on``); **panic** if the probe fails or
  the seccomp backend isn't compiled in.

In ``*_ONLY`` builds the boot param is honored only as a sanity
check: ``backend=force=<other>`` against an ``*_ONLY`` build that
doesn't include that backend panics; otherwise the Kconfig choice
wins regardless of the boot param.

Legacy ``seccomp=on/auto/off`` is preserved one release as a
transitional alias and is consumed by ``os_early_checks`` to set
``using_seccomp`` (which ``auto`` reads). Migration:

- ``seccomp=on``    → ``backend=force=seccomp``
- ``seccomp=auto``  → ``backend=auto``
- ``seccomp=off``   → ``backend=ptrace``

***********************
Conformance
***********************

Backends are validated by the conformance suite under
``arch/um/backend/contract/`` (workstream A-05). The suite exercises:

- Per-op unit tests (one kunit test per op).
- Cross-backend equivalence (LTP syscall subset on every available
  backend; identical observable behavior required).
- Performance regression (5 HOT ops, cycle-level CI gates;
  ``getpid()`` < 100 ns on the KVM bookend).

A backend that does not pass the conformance suite cannot land in
mainline.

***********************
References
***********************

- Architecture: ``Documentation/virt/uml/redesign/01-architecture/three-layers.md``
- Workstream A: ``Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/``
- Decisions log: ``Documentation/virt/uml/redesign/04-risks/decisions-log.md``
- Header: ``arch/um/include/asm/backend.h``
