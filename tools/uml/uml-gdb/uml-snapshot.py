# SPDX-License-Identifier: GPL-2.0
#
# gdb python helper for UML kvm-v2 snapshot ELF64-core files (#181).
#
# Sourced into gdb via:
#
#   gdb -ex 'source tools/uml/uml-gdb/uml-snapshot.py' -c dump.elf
#
# Provides three new commands:
#
#   uml-snap-info     — header dump of the UML private PT_NOTE
#   uml-snap-sregs    — pretty-print captured sregs (CS/SS/IDT/GDT/CR3)
#   uml-snap-msrs     — list captured MSRs (LSTAR/STAR/EFER/FS_BASE/...)
#   uml-snap-memslots — table of memslot descriptors
#
# All commands are no-ops on a non-UML core file (the helper detects
# the UML private note by 'UMLE' magic + version 1 and silently skips
# when absent). This lets the helper live alongside other gdb scripts
# without interfering with native Linux coredump inspection.
#
# Implementation note: gdb's `gdb.python` API does NOT expose the raw
# note bytes from the core file in a convenient way (the closest is
# the unhelpful Frame / Inferior triple). We work around this by
# `subprocess.check_output(['readelf', '-x', '0xXX', core])` — readelf
# can dump arbitrary note sections by file offset, and we drive it
# from the python side after grepping `readelf -n` for the right
# UML note offset. This keeps the helper independent of bleeding-edge
# gdb python APIs.

import os
import re
import struct
import subprocess
import sys

import gdb

# UML private PT_NOTE n_type — must match
# arch/um/backend/kvm-v2/snapshot_elf.c::KVM_V2_NT_UML_STATE.
UML_NT_STATE = 0x554D4C01

# 'UMLE' little-endian — must match
# arch/um/backend/kvm-v2/snapshot_elf.c::kvm_v2_uml_state_hdr.magic
# (the value written is 0x554d4c45 — 'E','L','M','U' on disk).
UML_MAGIC = 0x554D4C45

UML_VERSION = 1

# 7 MSRs, fixed list (matches arch/um/backend/kvm-v2/snapshot.c
# kvm_v2_snapshot_msr_indices). Order is intentionally separate from
# the captured payload — the payload carries the indices.
MSR_NAMES = {
    0xC0000080: "MSR_EFER",
    0xC0000081: "MSR_STAR",
    0xC0000082: "MSR_LSTAR",
    0xC0000084: "MSR_SYSCALL_MASK",
    0xC0000100: "MSR_FS_BASE",
    0xC0000101: "MSR_GS_BASE",
    0xC0000102: "MSR_KERNEL_GS_BASE",
}


def _current_core_file():
    """Return the absolute path to the core file gdb is currently
    examining, or None if the inferior has no core file loaded.
    """
    try:
        info = gdb.execute("info files", to_string=True)
    except gdb.error:
        return None
    m = re.search(r"^Local core dump file:\s*\n?\s*`([^']+)'", info, re.M)
    if not m:
        return None
    return m.group(1)


def _read_uml_note_bytes(core_path):
    """Extract the UML-private PT_NOTE payload bytes from @core_path.

    Returns the raw payload bytes (without the Nhdr / owner-string
    prefix), or None if no UML note is found. The probe is done via
    readelf -n + readelf -x; we don't try to parse the ELF ourselves.
    """
    try:
        notes = subprocess.check_output(
            ["readelf", "-n", core_path],
            stderr=subprocess.DEVNULL,
        ).decode("utf-8", "replace")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None

    # readelf prints lines like:
    #   UML                  0x000003b8	Unknown note type: (0x554d4c01)
    # The 'Unknown note type' wording varies; the n_type number is
    # what we key on. We then need the offset / size to seek. readelf
    # doesn't print the file offset of each note inline; the simpler
    # path is to fall back to parsing the ELF header + iterating PT_NOTE
    # phdrs ourselves.
    with open(core_path, "rb") as f:
        ehdr_bytes = f.read(64)
        if len(ehdr_bytes) < 64 or ehdr_bytes[:4] != b"\x7fELF":
            return None
        # Elf64_Ehdr layout:
        #  16  e_ident
        #  H   e_type
        #  H   e_machine
        #  I   e_version
        #  Q   e_entry
        #  Q   e_phoff
        #  Q   e_shoff
        #  I   e_flags
        #  H   e_ehsize
        #  H   e_phentsize
        #  H   e_phnum
        (
            _ident_bytes,
            _e_type,
            _e_machine,
            _e_version,
            _e_entry,
            e_phoff,
            _e_shoff,
            _e_flags,
            _e_ehsize,
            e_phentsize,
            e_phnum,
        ) = struct.unpack("<16sHHIQQQIHHH", ehdr_bytes[:58])

        for i in range(e_phnum):
            f.seek(e_phoff + i * e_phentsize)
            phdr = f.read(e_phentsize)
            if len(phdr) < 56:
                continue
            # Elf64_Phdr: I p_type, I p_flags, Q p_offset, Q p_vaddr,
            # Q p_paddr, Q p_filesz, Q p_memsz, Q p_align.
            (p_type, _p_flags, p_offset, _p_vaddr,
             _p_paddr, p_filesz, _p_memsz, _p_align) = struct.unpack(
                "<IIQQQQQQ", phdr[:56])
            if p_type != 4:  # PT_NOTE
                continue
            f.seek(p_offset)
            note_blob = f.read(p_filesz)

            # Walk the Nhdr stream inside this PT_NOTE.
            cur = 0
            while cur + 12 <= len(note_blob):
                n_namesz, n_descsz, n_type = struct.unpack(
                    "<III", note_blob[cur:cur + 12])
                cur += 12
                name_end = cur + ((n_namesz + 3) & ~3)
                desc_end = name_end + ((n_descsz + 3) & ~3)
                if desc_end > len(note_blob):
                    break
                if n_type == UML_NT_STATE:
                    return note_blob[name_end:name_end + n_descsz]
                cur = desc_end
    return None


def _parse_uml_payload(payload):
    """Parse the UML PT_NOTE payload into a dict. Returns None on
    magic/version mismatch.
    """
    if len(payload) < 40:
        return None
    hdr = struct.unpack("<IIIIIIIIII", payload[:40])
    (magic, version, sregs_size, events_size, xcrs_size,
     msr_count, memslot_count, task_source_pid,
     task_state_captured, _reserved) = hdr
    if magic != UML_MAGIC or version != UML_VERSION:
        return None
    p = 40
    sregs = payload[p:p + sregs_size]
    p += sregs_size
    events = payload[p:p + events_size]
    p += events_size
    xcrs = payload[p:p + xcrs_size]
    p += xcrs_size

    msrs = []
    for _ in range(msr_count):
        if p + 16 > len(payload):
            break
        idx, val = struct.unpack("<QQ", payload[p:p + 16])
        msrs.append((idx, val))
        p += 16

    slots = []
    for _ in range(memslot_count):
        if p + 32 > len(payload):
            break
        slot_id, flags, gpa, host_va, size = struct.unpack(
            "<IIQQQ", payload[p:p + 32])
        slots.append((slot_id, flags, gpa, host_va, size))
        p += 32

    return {
        "magic": magic,
        "version": version,
        "sregs_bytes": sregs,
        "events_bytes": events,
        "xcrs_bytes": xcrs,
        "msrs": msrs,
        "memslots": slots,
        "task_source_pid": task_source_pid,
        "task_state_captured": bool(task_state_captured),
    }


def _load_uml_note():
    """Return the parsed UML PT_NOTE payload for the current core, or
    None if none is present / parseable.
    """
    core = _current_core_file()
    if not core:
        return None
    raw = _read_uml_note_bytes(core)
    if not raw:
        return None
    return _parse_uml_payload(raw)


class UmlSnapInfo(gdb.Command):
    """uml-snap-info — summarise the UML private PT_NOTE.

    Prints the magic / version / counts so the operator can confirm
    they're looking at a UML kvm-v2 snapshot (rather than a native
    Linux task coredump).
    """
    def __init__(self):
        super().__init__("uml-snap-info", gdb.COMMAND_USER)

    def invoke(self, _arg, _from_tty):
        note = _load_uml_note()
        if note is None:
            print("uml-snap-info: no UML PT_NOTE in current core "
                  "(not a kvm-v2 snapshot?)")
            return
        print("UML kvm-v2 snapshot:")
        print(f"  magic               : {note['magic']:#010x} ('UMLE')")
        print(f"  version             : {note['version']}")
        print(f"  sregs payload       : {len(note['sregs_bytes'])} bytes")
        print(f"  events payload      : {len(note['events_bytes'])} bytes")
        print(f"  xcrs payload        : {len(note['xcrs_bytes'])} bytes")
        print(f"  msr count           : {len(note['msrs'])}")
        print(f"  memslot count       : {len(note['memslots'])}")
        print(f"  task source pid     : {note['task_source_pid']}")
        print(f"  task state captured : {note['task_state_captured']}")


class UmlSnapMsrs(gdb.Command):
    """uml-snap-msrs — list the captured MSR (index, value) pairs."""
    def __init__(self):
        super().__init__("uml-snap-msrs", gdb.COMMAND_USER)

    def invoke(self, _arg, _from_tty):
        note = _load_uml_note()
        if note is None:
            print("uml-snap-msrs: no UML PT_NOTE in current core")
            return
        print(f"  {'index':<12} {'name':<22} {'value'}")
        for idx, val in note["msrs"]:
            name = MSR_NAMES.get(idx, "(unknown)")
            print(f"  {idx:#010x}   {name:<22} {val:#018x}")


class UmlSnapMemslots(gdb.Command):
    """uml-snap-memslots — table of captured memslot descriptors."""
    def __init__(self):
        super().__init__("uml-snap-memslots", gdb.COMMAND_USER)

    def invoke(self, _arg, _from_tty):
        note = _load_uml_note()
        if note is None:
            print("uml-snap-memslots: no UML PT_NOTE in current core")
            return
        if not note["memslots"]:
            print("(no memslots captured — regs-only snapshot?)")
            return
        print(f"  {'id':<4} {'flags':<10} {'gpa':<18} "
              f"{'host_va':<18} {'size':<18}")
        for slot_id, flags, gpa, host_va, size in note["memslots"]:
            print(f"  {slot_id:<4} {flags:#010x} {gpa:#018x} "
                  f"{host_va:#018x} {size:#018x}")


class UmlSnapSregs(gdb.Command):
    """uml-snap-sregs — pretty-print captured x86_64 sregs (cs/ss/idt/gdt
    cr0/cr2/cr3/cr4/efer + segment selectors).
    """
    def __init__(self):
        super().__init__("uml-snap-sregs", gdb.COMMAND_USER)

    def invoke(self, _arg, _from_tty):
        note = _load_uml_note()
        if note is None:
            print("uml-snap-sregs: no UML PT_NOTE in current core")
            return
        b = note["sregs_bytes"]
        # struct kvm_segment is 24 bytes: u64 base, u32 limit, u16
        # selector, 7 u8 fields, 1 u8 unusable, 4 u8 padding.
        # struct kvm_sregs layout (arch/x86/include/uapi/asm/kvm.h):
        #  kvm_segment cs, ds, es, fs, gs, ss, tr, ldt;   (8 × 24 = 192)
        #  kvm_dtable gdt, idt;                            (2 × 16 = 32)
        #  __u64 cr0, cr2, cr3, cr4, cr8, efer;
        #  __u64 apic_base;
        #  __u64 interrupt_bitmap[(KVM_NR_INTERRUPTS+63)/64]; (4 × 8)
        if len(b) < 192 + 32 + 8 * 7:
            print(f"uml-snap-sregs: payload too short ({len(b)})")
            return

        # Selectors are at offset 12 within each kvm_segment.
        seg_names = ["cs", "ds", "es", "fs", "gs", "ss", "tr", "ldt"]
        print("Segment selectors:")
        for i, n in enumerate(seg_names):
            off = i * 24
            base, limit, selector = struct.unpack("<QIH", b[off:off + 14])
            print(f"  {n:<3}  selector={selector:#06x}  "
                  f"base={base:#018x}  limit={limit:#010x}")

        gdt_idt_off = 192
        gdt_base, gdt_limit, _, _, _ = struct.unpack(
            "<QHHHH", b[gdt_idt_off:gdt_idt_off + 16])
        idt_base, idt_limit, _, _, _ = struct.unpack(
            "<QHHHH", b[gdt_idt_off + 16:gdt_idt_off + 32])
        print("\nDescriptor tables:")
        print(f"  gdt  base={gdt_base:#018x}  limit={gdt_limit:#06x}")
        print(f"  idt  base={idt_base:#018x}  limit={idt_limit:#06x}")

        cr_off = gdt_idt_off + 32
        (cr0, cr2, cr3, cr4, cr8, efer) = struct.unpack(
            "<QQQQQQ", b[cr_off:cr_off + 48])
        print("\nControl registers:")
        print(f"  cr0  = {cr0:#018x}")
        print(f"  cr2  = {cr2:#018x}")
        print(f"  cr3  = {cr3:#018x}")
        print(f"  cr4  = {cr4:#018x}")
        print(f"  cr8  = {cr8:#018x}")
        print(f"  efer = {efer:#018x}")


# Register the commands. Each constructor calls super().__init__ which
# registers with gdb.
UmlSnapInfo()
UmlSnapMsrs()
UmlSnapMemslots()
UmlSnapSregs()

# Quiet on load: if the helper is sourced from a script that loads
# many things, a banner is noise. Operators who want confirmation
# can run `uml-snap-info` immediately after sourcing.
try:
    note = _load_uml_note()
    if note is not None:
        gdb.write(
            f"uml-snapshot.py: UML kvm-v2 snapshot detected "
            f"(version={note['version']}, "
            f"memslots={len(note['memslots'])}, "
            f"msrs={len(note['msrs'])}); "
            "commands: uml-snap-info, uml-snap-sregs, "
            "uml-snap-msrs, uml-snap-memslots\n"
        )
except Exception:
    # gdb python is environment-dependent; never let helper noise
    # interfere with core debugging.
    pass
