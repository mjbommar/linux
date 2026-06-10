.. SPDX-License-Identifier: GPL-2.0

================================================
UML kvm-v2 snapshot ELF64-core on-disk format
================================================

:Author: UML kvm-v2 maintainers
:Status: v1, version-tagged for forward evolution; current ``next``
         validation is pending.

This document specifies the on-disk layout of the ELF64 core file
produced by ``kvm_v2_snapshot_elf_export_to_file()``.  The format is
deliberately a strict superset of an x86_64 ET_CORE that gdb and
crash(8) understand, with one UML-private ``PT_NOTE`` entry carrying
state those tools do not natively interpret.

Why ELF
=======

UML is itself an ELF64 Linux process; ``fs/binfmt_elf.c`` and
``fs/proc/vmcore.c`` are the in-tree precedent for producing
ET_CORE files from kernel context.  We mirror their shape so an
operator who already knows how to inspect a kernel coredump can
walk a UML snapshot with the same toolchain.

File layout
===========

::

    +-------------------------------+
    | Elf64_Ehdr  (e_type = ET_CORE)|
    +-------------------------------+
    | Elf64_Phdr * (1 + N_load)     |
    |   - phdrs[0]  = PT_NOTE        |
    |   - phdrs[1..N] = PT_LOAD per   |
    |                  memslot         |
    +-------------------------------+
    | PT_NOTE payload                 |
    |   NT_PRSTATUS  (CORE)          |
    |   NT_FPREGSET  (CORE)          |
    |   NT_X86_XSTATE (LINUX)        |
    |   UML private state (UML)      |
    +-------------------------------+
    | (zero-pad to PAGE_SIZE)       |
    +-------------------------------+
    | PT_LOAD payloads (back-to-back)|
    +-------------------------------+

ELF header
----------

The ``Elf64_Ehdr`` follows the standard x86_64 ET_CORE shape with the
following constants fixed:

* ``e_ident``: ``\x7fELF`` ``ELFCLASS64`` ``ELFDATA2LSB`` ``EV_CURRENT``
* ``e_type    = ET_CORE``
* ``e_machine = EM_X86_64``
* ``e_entry   = snap->regs.rip`` (advisory; gdb uses ``NT_PRSTATUS``
  for register state)
* ``e_phoff   = sizeof(Elf64_Ehdr)``
* ``e_shoff   = 0`` (no section headers)
* ``e_phnum   = 1 + memslot_load_count``

PT_NOTE
-------

A single ``PT_NOTE`` phdr located immediately after the phdr table.
Contains four notes in the following order:

1. ``NT_PRSTATUS`` / owner ``"CORE"`` — gdb-compatible
   ``struct elf_prstatus``.  ``pr_reg`` is filled from ``snap->regs``
   in x86_64 ``struct user_regs_struct`` ordering; segment selectors
   from ``snap->sregs``; ``FS_BASE`` / ``GS_BASE`` pulled from the
   captured MSR list.

2. ``NT_FPREGSET`` / owner ``"CORE"`` — 512-byte legacy i387 FXSAVE
   area, dumped verbatim from the first 512 bytes of
   ``snap->xsave`` (the AMD64 ABI defines the XSAVE layout as
   "legacy header ‖ XSTATE_BV ‖ components", so the first 512 bytes
   ARE exactly the legacy area gdb expects here).

3. ``NT_X86_XSTATE`` / owner ``"LINUX"`` — full ``struct kvm_xsave``
   (4 KB, x86_64 ``KVM_GET_XSAVE`` shape).  Carries the AVX YMM upper
   halves the legacy area cannot represent.

4. UML private note — owner ``"UML"``, ``n_type = 0x554d4c01``
   (``'U' | 'M' | 'L' | 0`` + ordinal 1).  Layout described below.

UML private note payload
========================

The UML note carries scalar state gdb cannot inspect natively: the
captured sregs / events / xcrs / MSR list, plus a compact descriptor
table for each memslot.  Schema is fixed-width and little-endian on
every supported host.

The payload starts with a 40-byte header::

    struct kvm_v2_uml_state_hdr {
        __u32 magic;            /* 0x554d4c45 ('UMLE') */
        __u32 version;          /* see "Version" below */
        __u32 sregs_size;       /* sizeof(struct kvm_sregs) */
        __u32 events_size;      /* sizeof(struct kvm_vcpu_events) */
        __u32 xcrs_size;        /* sizeof(struct kvm_xcrs) */
        __u32 msr_count;        /* always 7 in v1 */
        __u32 memslot_count;    /* may be zero */
        __u32 task_source_pid;  /* 0 if not a _task capture */
        __u32 task_state_captured; /* 0 / 1 */
        __u32 reserved;         /* 0 */
    };

Followed by, in order:

* ``sregs_size`` bytes of ``struct kvm_sregs`` (KVM's
  ``KVM_GET_SREGS`` shape).
* ``events_size`` bytes of ``struct kvm_vcpu_events`` (KVM's
  ``KVM_GET_VCPU_EVENTS`` shape).
* ``xcrs_size`` bytes of ``struct kvm_xcrs``.
* ``msr_count`` × ``struct { __u64 index; __u64 value; }`` MSR pairs,
  in the capture order pinned by
  ``arch/um/backend/kvm-v2/snapshot.c::kvm_v2_snapshot_msr_indices``
  (LSTAR / STAR / SYSCALL_MASK / KERNEL_GS_BASE / FS_BASE / GS_BASE
  / EFER).  Readers SHOULD key on ``index`` (not array position) for
  forward compatibility.
* ``memslot_count`` × ``struct { __u32 slot_id; __u32 flags; __u64
  gpa; __u64 host_va; __u64 size; }``.

PT_LOAD
=======

Each captured memslot with non-NULL data appears as a ``PT_LOAD``
phdr with:

* ``p_type    = PT_LOAD``
* ``p_flags   = PF_R | PF_W``
* ``p_offset  = (file offset; computed after the notes section)``
* ``p_vaddr   = memslot.guest_phys_addr``
* ``p_paddr   = memslot.guest_phys_addr``
* ``p_filesz  = memslot.memory_size``
* ``p_memsz   = memslot.memory_size``
* ``p_align   = PAGE_SIZE``

Memslots whose data buffer failed to allocate during capture are recorded in
the UML private note's memslot descriptor array with ``size`` populated but no
matching ``PT_LOAD`` segment.  Readers should iterate the UML note's slot list
rather than assuming a 1:1 correspondence with ``PT_LOAD`` phdrs.

Version
=======

The UML note header carries an explicit ``version`` field.  v1
(documented above) is the shipping format.  Readers MUST verify the
magic + version before parsing.

Schema evolution rules:

* Adding a fixed-position field at the end of an existing struct is
  NOT a breaking change as long as the corresponding ``_size`` field
  in ``kvm_v2_uml_state_hdr`` is bumped (a v1 reader will simply
  ignore the trailing bytes).  This is how ``struct kvm_sregs``
  evolves upstream and we want to follow the same rule.
* Adding a new note type number under the ``UML`` vendor is also
  forward-compatible; existing readers ignore unknown ``n_type``.
* Renaming or repurposing a field in an existing struct REQUIRES a
  version bump.  Old readers refuse the new file, new readers may
  optionally fall back to v1 parsing for backwards compatibility.

Reading the file
================

readelf
-------

``readelf -h``, ``readelf -l``, and ``readelf -n`` all parse a
v2 snapshot.  Older ``readelf`` releases may print
"Unknown note type" for the UML vendor note; that's expected — the
file is still well-formed.

gdb
---

``gdb -c dump.elf`` opens without errors and ``info registers``
shows the captured RIP / RSP / RFLAGS / GPR set.  Source the bundled
``tools/uml/uml-gdb/uml-snapshot.py`` helper to add the
``uml-snap-info``, ``uml-snap-sregs``, ``uml-snap-msrs``, and
``uml-snap-memslots`` commands for inspecting the UML private state.

crash(8)
--------

``crash dump.elf <vmlinux>`` recognises the ET_CORE shape and walks
``PT_LOAD`` ranges.  Older ``crash`` releases lack a UML-aware
extension; the gdb python helper above is the recommended path for
operator-level inspection until that exists.

Producing the file
==================

From the operator side:

* ``umlctl snapshot export <instance> --output dump.elf`` — the
  blessed host-side path.  It resolves the running instance's mconsole
  socket and sends ``snapshot_export <path>``; the kernel writes the ELF
  through UML host-file helpers.

* Direct debugfs write (for inside-the-guest scripts)::

    echo /path/to/dump.elf > /sys/kernel/debug/um/kvm_v2_snapshot_elf_export_path

* In-kernel callers use ``kvm_v2_snapshot_elf_export_to_file()`` or
  ``kvm_v2_snapshot_elf_export_to_fd()`` (declared in
  ``arch/um/backend/kvm-v2/kvm_v2_backend.h``).

Validation requirements
=======================

Before this format is marked complete on ``next``, validate a freshly exported
dump with:

* ``readelf -h dump.elf``
* ``readelf -l dump.elf``
* ``readelf -n dump.elf``
* ``gdb -c dump.elf``
* ``gdb -ex 'source tools/uml/uml-gdb/uml-snapshot.py' -c dump.elf``
