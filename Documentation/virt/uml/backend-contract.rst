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
``UM_BACKEND_CONTRACT_VERSION = 2``.

Version history
===============

- **v2** (current) - SMP-era signature changes: ``ipi_send`` and
  ``set_timer`` take a per-CPU ``cpu`` argument, and ``set_timer``
  takes an ``enum um_timer_mode``. The version is bumped because these
  are *signature* changes, not additions. The optional
  ``tlb_kick_others`` op (for backends with per-vCPU guest TLBs) was
  added in the same era; pure additions do not by themselves bump the
  version.
- **v1** - initial single-CPU contract (6.16-era ``seccomp`` backend).

The bump rule: adding a new op at the end of ``struct um_backend_ops``
is source-compatible and does **not** bump the version; changing an
existing op's signature does. Every op that is dispatched
unconditionally through ``um_backend_dispatch()`` is validated non-NULL
at boot by ``validate_required_ops()`` in ``arch/um/kernel/backend.c``
(it panics naming the missing op), so a backend that omits a required
op fails cleanly at init rather than NULL-dereferencing on first
dispatch. Genuinely optional ops (``probe``/``init``/``shutdown``,
``mm_region_protected``, ``tlb_kick_others``) may be NULL and are
NULL-checked at their call sites.

For end-user documentation (which backend to pick, the boot
parameters, the trap-path diagrams), see
:doc:`UML Backends <backends>`.

************
Overview
************

A UML backend is a host-side trap mechanism that lets a User-Mode
Linux instance intercept its guest's syscalls, page faults, signals,
and context switches. Two backends are in tree:

- ``seccomp`` - merged in 6.16. Trades a small in-guest BPF filter
  for ~3-4x faster traps via SIGSYS + futex. ~300-500 ns per syscall.
- ``kvm-v2`` - accelerated backend using KVM exits and in-guest
  fast paths for selected syscalls. It is opt-in while validation
  continues.

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
  is a plain function-pointer call with no NULL check; it cannot
  synthesize ``-ENOSYS`` for a missing ``void`` or ``u64`` op.
  Backends that don't yet implement a cold op should provide a thin
  wrapper that returns ``-EOPNOTSUPP`` (for ops returning ``int``)
  rather than leave the field NULL. See the seccomp
  backends' ``read_guest_regs`` / ``write_guest_regs`` for examples
  pending KGDB integration.
- Errno conventions follow standard kernel style: 0 on success,
  negative ``errno`` on failure.

Per-op semantics
================

Lifecycle and trap (4 ops)
--------------------------

All three cold lifecycle ops dispatch through the ops table from
``arch/um/kernel/backend.c``: ``init_backend()`` calls
``probe()`` and ``init()`` in sequence right after HOT-ops
validation, and ``uml_cleanup()`` on the reboot/halt path calls
``shutdown()``. Call sites panic on any non-zero return from
probe/init; shutdown returns void. The in-tree seccomp backend
ships implementations that return 0; host probing remains in
``arch/um/os-Linux/start_up.c``. The dispatch path itself is
authoritative, so the v2 KVM backend's
``kvm_open()`` / per-mm-worker spawn work has a wired call site.

``probe(void)``
    Cold. Returns 0 if this backend can run on the current host
    (hardware capability + kernel feature checks), or a negative
    errno otherwise. Must be cheap (<=1 ms typical) and side-effect
    free beyond opening probe FDs that are immediately closed.
    Dispatched from ``init_backend()`` after HOT-ops validation.

``init(const struct um_backend_args *args)``
    Cold. Called once after this backend wins the arbiter. Sets up
    backend-private state (per-backend singleton). May fail with a
    negative errno; on failure the kernel panics. Dispatched from
    ``init_backend()`` immediately after ``probe()``.

``shutdown(void)``
    Cold. Called from the reboot/halt path. Releases backend
    resources. Must not sleep; called with interrupts off in some
    paths. Dispatched from ``uml_cleanup()`` in
    ``arch/um/kernel/reboot.c`` before any other teardown so the
    backend can free things while kmalloc is still usable.

``vcpu_run(struct uml_pt_regs *regs)``
    **HOT.** The trap loop. Resumes guest userspace with the state
    in ``*regs``, runs until the next trap (syscall, page fault,
    signal, IRQ injection), then returns with ``regs`` updated and
    ``regs->faultinfo`` populated for fault-bearing exits.

    The kernel-side trap handlers (``handle_syscall``, ``segv``,
    ``relay_signal``) are invoked **by** the backend from inside
    ``vcpu_run`` once the trap reason is known. Backends should
    not return to the caller without invoking the appropriate
    handler.

Memory (5 ops)
--------------

All per-mm and per-region ops take ``struct mm_struct *`` and
``const struct um_memory_region *``. Backends manage their own
per-mm storage layout: seccomp uses ``mm->context.id`` and kvm-v2
keys off the mm pointer differently.

``mm_create(struct mm_struct *mm)``
    Cold. Called once per ``init_new_context()``. Sets up per-mm
    backend state (stub child for seccomp; per-mm worker process
    + KVM context for v2). Returns 0 or negative errno.

``mm_destroy(struct mm_struct *mm)``
    Cold. Called from ``destroy_context()`` and the reboot path.
    Tears down everything ``mm_create`` created.

``mm_region_added(struct mm_struct *mm, const struct um_memory_region *region)``
    **HOT.** Map a contiguous host-backing region into the guest
    mm at ``region->va``. ``region->phys_fd`` / ``region->offset``
    identify the host-side backing; ``region->prot`` is a
    ``PROT_*`` mask. Returns 0 or negative errno.

    Backends may queue and amortize multiple ``mm_region_added``
    calls into one stub round-trip; they must flush implicitly
    before the next ``vcpu_run()`` returns to the guest.

    Backends may stash per-region state in ``region->backend_data``
    (e.g., v2 stores the memslot ID).

``mm_region_removed(struct mm_struct *mm, const struct um_memory_region *region)``
    **HOT.** Symmetric. Same flush semantics as ``mm_region_added``.

``mm_region_protected(struct mm_struct *mm, const struct um_memory_region *region)``
    Cold. Optional (backend may set NULL). Notification that an
    existing region's ``prot`` changed. The mprotect path goes
    through ``um_tlb_sync`` which emits a ``mm_region_removed`` +
    ``mm_region_added`` pair; backends that want a direct path
    (v2's memslot-flag-update) implement this op. mm-arbiter
    consults the field and falls back to the remove+add sequence
    when NULL.

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

    - ``UM_TIMER_DISABLE`` - disable; ``deadline_ns`` ignored.
    - ``UM_TIMER_ONE_SHOT`` - fire once after ``deadline_ns``
      nanoseconds (relative).
    - ``UM_TIMER_PERIODIC`` - fire every ``deadline_ns`` nanoseconds.

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
    ``*regs``. Reserved for future KGDB integration and debugger
    tooling.

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

``struct mm_id`` (declared in
``arch/um/include/shared/skas/mm_id.h``) is **seccomp-internal**:
it lives in
``mm->context.id`` and seccomp's ``mm_create`` /
``mm_region_added`` impls look it up via ``&mm->context.id``.
Other backends are free to ignore ``struct mm_id`` entirely and
key their per-mm state off the ``struct mm_struct *`` pointer
(v2's per-mm worker process model does this).

The fields seccomp uses internally:

==================== =========== ==================================
Field                Use
==================== =========== ==================================
``stack``            all backends  per-mm scratch page (stub_data
                                   for seccomp; per-vCPU
                                   payload for v2)
``pid``              seccomp       host child PID
``syscall_data_len`` seccomp       length of pending stub syscalls
``sock``             seccomp       SCM_RIGHTS socket
``syscall_fd_num``   seccomp       FDs queued for next stub batch
``syscall_fd_map[]`` seccomp       FD slot table
==================== =========== ==================================

Backend-owned per-mm state lives outside ``struct mm_id``.

For v2's per-region state (memslot ID, mmap'd buffer, mmu_notifier
handle), use ``struct um_memory_region::backend_data`` instead of
extending ``struct mm_id``.

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
- The dispatch macro is a plain function-pointer call; there is no
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

- ``CONFIG_UM_BACKEND_SECCOMP_ONLY`` - only seccomp; direct calls.
- ``CONFIG_UM_BACKEND_DYNAMIC`` - multi-backend; indirect calls
  through ``um_backend``.

Single-backend builds inline the chosen backend's symbols directly,
producing zero-overhead dispatch and a smaller binary. Used by
sandbox and embedded profiles. Multi-backend builds let the boot
arbiter pick.

Boot-time (multi-backend builds)
================================

In ``CONFIG_UM_BACKEND_DYNAMIC`` builds the active backend is chosen
at boot from the kernel command line:

- ``backend=auto`` - **Default.** Runs the host seccomp probe at
  boot and picks seccomp when the host supports it. Matches
  ``os_early_checks()`` behavior in
  ``arch/um/os-Linux/start_up.c`` (which runs the probe
  unconditionally) and the selector in
  ``arch/um/kernel/backend.c::pick_dynamic_backend()``.
- ``backend=seccomp`` - preference for seccomp; falls through
  similarly.
- ``backend=kvm-v2`` - preference for kvm-v2. Falls through to
  seccomp on probe failure.
- ``backend=force=seccomp`` - require seccomp; the probe runs
  (no need to also set ``seccomp=on``); **panic** if the probe
  fails or the seccomp backend isn't compiled in.
- ``backend=force=kvm-v2`` - require kvm-v2; **panic** if
  ``/dev/kvm`` isn't accessible or the backend isn't compiled in.

In ``*_ONLY`` builds the boot param is honored only as a sanity
check: ``backend=force=<other>`` against an ``*_ONLY`` build that
doesn't include that backend panics; otherwise the Kconfig choice
wins regardless of the boot param.

The ``seccomp=on/auto/off`` compatibility alias is consumed by
``os_early_checks`` to set ``using_seccomp`` (which ``auto`` reads).
Mapping:

- ``seccomp=on``    -> ``backend=force=seccomp``
- ``seccomp=auto``  -> ``backend=auto``
- ``seccomp=off``   -> ``backend=auto``

***********************
Conformance
***********************

Backends are validated by the conformance suite under
``arch/um/backend/contract/``. The suite exercises:

- Per-op unit tests (one kunit test per op).
- Cross-backend equivalence (LTP syscall subset on every available
  backend; identical observable behavior required).
- Performance regression (5 HOT ops, cycle-level CI gates;
  ``getpid()`` < 100 ns on the KVM bookend).

A backend that does not pass the conformance suite cannot land in
mainline.

The authoritative API is ``arch/um/include/shared/backend.h``.
