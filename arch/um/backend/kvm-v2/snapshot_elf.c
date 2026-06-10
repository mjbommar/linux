// SPDX-License-Identifier: GPL-2.0
/*
 * KVM v2 snapshot ELF64-core export.
 *
 * The writer emits an ET_CORE file that standard tools can open:
 *
 *   - one PT_NOTE segment containing NT_PRSTATUS, NT_FPREGSET,
 *     NT_X86_XSTATE, and a UML-private note for sregs, xcrs, vCPU events,
 *     MSRs, and memslot descriptors;
 *   - one PT_LOAD segment for each captured memslot with byte data.
 *
 * Memslots captured as metadata only are described in the UML-private note
 * and skipped as PT_LOAD segments.
 */

#include <linux/debugfs.h>
#include <linux/elf.h>
#include <linux/elfcore.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/user.h>		/* struct user_regs_struct */
#include <linux/vmalloc.h>

#ifdef CONFIG_MCONSOLE
#include "../../drivers/mconsole.h"
#endif

#include <os.h>

#include "kvm_v2_backend.h"

/*
 * UML-private PT_NOTE n_type. The numeric range starts at
 * 0x554d4c00 ('U'|'M'|'L'|0); ordinal 1 is the consolidated KVM v2
 * state payload.
 */
#define KVM_V2_NT_UML_STATE	0x554d4c01u
#define KVM_V2_NT_UML_OWNER	"UML"

/*
 * UML-private payload version. Bumped on any layout change to
 * uml_state_payload below. Readers refuse to parse a version they
 * don't know.
 */
#define KVM_V2_UML_STATE_VERSION	1u

/*
 * Compact UML-private state payload. Sized for stability - fixed-
 * width fields, no embedded pointers, no architecture-specific
 * unions. The header lets a python helper / crash extension verify it
 * loaded the right thing.
 *
 * sregs+events+xcrs are dumped verbatim (struct copies from
 * snap->{sregs,events,xcrs}). MSRs are dumped as fixed-length
 * (index,value) pairs sized by KVM_V2_SNAPSHOT_MSR_COUNT so the
 * reader can iterate without re-reading a variable-length embedded array.
 *
 * Memslot descriptors are dumped here too so the reader can pair
 * each PT_LOAD with its (slot_id, gpa, host_va, flags) without deriving
 * it from the phdr table.
 */
struct kvm_v2_uml_state_hdr {
	__u32 magic;		/* 'UMLE' (UML ELF state) - sanity tag.   */
	__u32 version;		/* KVM_V2_UML_STATE_VERSION.              */
	__u32 sregs_size;	/* sizeof(struct kvm_sregs).              */
	__u32 events_size;	/* sizeof(struct kvm_vcpu_events).         */
	__u32 xcrs_size;	/* sizeof(struct kvm_xcrs).                */
	__u32 msr_count;	/* KVM_V2_SNAPSHOT_MSR_COUNT.              */
	__u32 memslot_count;	/* number of slot_desc entries below.     */
	__u32 task_source_pid;	/* snap->task_source_pid (0 if unset).    */
	__u32 task_state_captured; /* bool, padded.                       */
	__u32 reserved;
};

struct kvm_v2_uml_state_msr {
	__u64 index;		/* MSR index in a fixed-width slot. */
	__u64 value;
};

struct kvm_v2_uml_state_slot {
	__u32 slot_id;
	__u32 flags;
	__u64 gpa;
	__u64 host_va;
	__u64 size;
};

/*
 * Total payload size for a given snapshot. The trailing memslot-desc
 * array is variable-length; everything else is fixed.
 */
static size_t kvm_v2_elf_uml_payload_size(const struct kvm_v2_snapshot *snap)
{
	return sizeof(struct kvm_v2_uml_state_hdr)
		+ sizeof(struct kvm_sregs)
		+ sizeof(struct kvm_vcpu_events)
		+ sizeof(struct kvm_xcrs)
		+ KVM_V2_SNAPSHOT_MSR_COUNT * sizeof(struct kvm_v2_uml_state_msr)
		+ snap->memslot_count * sizeof(struct kvm_v2_uml_state_slot);
}

/*
 * Build the UML-private payload into a caller-provided buffer.
 * Caller sizes the buffer via kvm_v2_elf_uml_payload_size().
 */
static void kvm_v2_elf_build_uml_payload(const struct kvm_v2_snapshot *snap,
					 void *buf)
{
	struct kvm_v2_uml_state_hdr *hdr = buf;
	u8 *p = buf;
	int i;

	hdr->magic		= 0x554d4c45u;	/* 'UMLE' little-endian. */
	hdr->version		= KVM_V2_UML_STATE_VERSION;
	hdr->sregs_size		= sizeof(struct kvm_sregs);
	hdr->events_size	= sizeof(struct kvm_vcpu_events);
	hdr->xcrs_size		= sizeof(struct kvm_xcrs);
	hdr->msr_count		= KVM_V2_SNAPSHOT_MSR_COUNT;
	hdr->memslot_count	= (u32)snap->memslot_count;
	hdr->task_source_pid	= (u32)snap->task_source_pid;
	hdr->task_state_captured = snap->task_state_captured ? 1u : 0u;
	hdr->reserved		= 0;

	p += sizeof(*hdr);
	memcpy(p, &snap->sregs, sizeof(snap->sregs));
	p += sizeof(snap->sregs);
	memcpy(p, &snap->events, sizeof(snap->events));
	p += sizeof(snap->events);
	memcpy(p, &snap->xcrs, sizeof(snap->xcrs));
	p += sizeof(snap->xcrs);

	for (i = 0; i < KVM_V2_SNAPSHOT_MSR_COUNT; i++) {
		struct kvm_v2_uml_state_msr m = {
			.index = snap->msrs.entries[i].index,
			.value = snap->msrs.entries[i].data,
		};
		memcpy(p, &m, sizeof(m));
		p += sizeof(m);
	}

	for (i = 0; i < snap->memslot_count; i++) {
		const struct kvm_userspace_memory_region *r =
			&snap->memslots[i].region;
		struct kvm_v2_uml_state_slot s = {
			.slot_id  = r->slot,
			.flags    = r->flags,
			.gpa      = r->guest_phys_addr,
			.host_va  = r->userspace_addr,
			.size     = r->memory_size,
		};
		memcpy(p, &s, sizeof(s));
		p += sizeof(s);
	}
}

/*
 * Translate snap->regs (KVM's kvm_regs layout) into the
 * gdb-compatible x86_64 user_regs_struct ordering. NT_PRSTATUS expects
 * elf_gregset_t which on x86_64 is exactly struct user_regs_struct.
 *
 * Segment selectors come from snap->sregs.{cs,ss,ds,es,fs,gs}.selector;
 * FS_BASE/GS_BASE come from the MSR list snap->msrs.entries[]. Those
 * fields are populated regardless of XSAVE state so gdb's "info
 * registers" lights up even on a regs-only capture.
 */
static void kvm_v2_elf_fill_user_regs(const struct kvm_v2_snapshot *snap,
				      struct user_regs_struct *ur)
{
	int i;

	memset(ur, 0, sizeof(*ur));
	ur->r15 = snap->regs.r15;
	ur->r14 = snap->regs.r14;
	ur->r13 = snap->regs.r13;
	ur->r12 = snap->regs.r12;
	ur->bp  = snap->regs.rbp;
	ur->bx  = snap->regs.rbx;
	ur->r11 = snap->regs.r11;
	ur->r10 = snap->regs.r10;
	ur->r9  = snap->regs.r9;
	ur->r8  = snap->regs.r8;
	ur->ax  = snap->regs.rax;
	ur->cx  = snap->regs.rcx;
	ur->dx  = snap->regs.rdx;
	ur->si  = snap->regs.rsi;
	ur->di  = snap->regs.rdi;
	ur->orig_ax = ~0UL;	/* no syscall in progress at capture. */
	ur->ip  = snap->regs.rip;
	ur->cs  = snap->sregs.cs.selector;
	ur->flags = snap->regs.rflags;
	ur->sp  = snap->regs.rsp;
	ur->ss  = snap->sregs.ss.selector;
	ur->ds  = snap->sregs.ds.selector;
	ur->es  = snap->sregs.es.selector;
	ur->fs  = snap->sregs.fs.selector;
	ur->gs  = snap->sregs.gs.selector;

	/*
	 * Pull FS_BASE / GS_BASE out of the MSR list. The indices are
	 * pinned in snapshot.c (kvm_v2_snapshot_msr_indices); search by
	 * MSR-index value rather than positional offset so the lookup
	 * survives the list being reordered.
	 */
	for (i = 0; i < KVM_V2_SNAPSHOT_MSR_COUNT; i++) {
		u32 idx = snap->msrs.entries[i].index;
		u64 val = snap->msrs.entries[i].data;

		if (idx == 0xC0000100u)		/* MSR_FS_BASE */
			ur->fs_base = val;
		else if (idx == 0xC0000101u)	/* MSR_GS_BASE */
			ur->gs_base = val;
	}
}

/*
 * Round @v up to the next multiple of @align. @align must be a power
 * of two. Used to align note payloads to 4-byte boundaries per ELF
 * Nhdr requirements.
 */
static inline size_t kvm_v2_elf_align_up(size_t v, size_t align)
{
	return (v + align - 1) & ~(align - 1);
}

struct kvm_v2_elf_writer {
	struct file *file;
	int host_fd;
};

/*
 * Write @len bytes from @buf at file offset @off. Wrapper around
 * kernel_write or the UML host-file helpers that retries on short
 * writes the way binfmt_elf's dump_emit does. Returns 0 on success,
 * -errno on hard failure.
 */
static int kvm_v2_elf_write(struct kvm_v2_elf_writer *writer, loff_t *pos,
			    const void *buf, size_t len)
{
	const u8 *p = buf;
	ssize_t n;
	int rc;

	while (len) {
		if (writer->file) {
			n = kernel_write(writer->file, p, len, pos);
		} else {
			size_t count = min_t(size_t, len, INT_MAX);

			rc = os_seek_file(writer->host_fd,
					  (unsigned long long)*pos);
			if (rc < 0)
				return rc;
			n = os_write_file(writer->host_fd, p, (int)count);
			if (n > 0)
				*pos += n;
		}
		if (n < 0)
			return (int)n;
		if (n == 0)
			return -EIO;
		p += n;
		len -= n;
	}
	return 0;
}

/*
 * Pad the file out to @target_off with zeroes from a small scratch
 * buffer. Used between note-section end and the first PT_LOAD payload
 * so the on-disk offsets match the phdr p_offset fields.
 *
 * The bounded scratch (PAGE_SIZE) keeps stack usage well under the
 * Wframe-larger-than= threshold even when PAGE_SIZE > 1024 - the
 * buffer is heap-allocated.
 */
static int kvm_v2_elf_pad_zero(struct kvm_v2_elf_writer *writer,
			       loff_t *pos, loff_t target_off)
{
	void *zero;
	size_t chunk;
	int rc = 0;

	if (*pos >= target_off)
		return 0;
	zero = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!zero)
		return -ENOMEM;
	while (*pos < target_off) {
		chunk = min_t(size_t, PAGE_SIZE, target_off - *pos);
		rc = kvm_v2_elf_write(writer, pos, zero, chunk);
		if (rc < 0)
			break;
	}
	kfree(zero);
	return rc;
}

/*
 * Emit a single ELF note: Nhdr + 4-byte-padded owner string + 4-byte-
 * padded payload. The owner string is copied including its trailing
 * NUL (per the standard).
 */
static int kvm_v2_elf_emit_note(struct kvm_v2_elf_writer *writer, loff_t *pos,
				const char *owner, u32 type,
				const void *payload, u32 paylen)
{
	struct elf64_note nhdr;
	u32 namesz = (u32)strlen(owner) + 1u;
	u8 pad[3] = {0, 0, 0};
	int rc;

	nhdr.n_namesz = namesz;
	nhdr.n_descsz = paylen;
	nhdr.n_type   = type;
	rc = kvm_v2_elf_write(writer, pos, &nhdr, sizeof(nhdr));
	if (rc < 0)
		return rc;
	rc = kvm_v2_elf_write(writer, pos, owner, namesz);
	if (rc < 0)
		return rc;
	if (namesz & 3) {
		rc = kvm_v2_elf_write(writer, pos, pad, 4 - (namesz & 3));
		if (rc < 0)
			return rc;
	}
	if (paylen) {
		rc = kvm_v2_elf_write(writer, pos, payload, paylen);
		if (rc < 0)
			return rc;
		if (paylen & 3) {
			rc = kvm_v2_elf_write(writer, pos, pad,
					      4 - (paylen & 3));
			if (rc < 0)
				return rc;
		}
	}
	return 0;
}

/*
 * Compute the on-disk size of one note (Nhdr + owner + payload, each
 * 4-byte-aligned). Mirrors what kvm_v2_elf_emit_note actually writes.
 */
static size_t kvm_v2_elf_note_size(const char *owner, u32 paylen)
{
	size_t namesz = strlen(owner) + 1u;

	return sizeof(struct elf64_note)
		+ kvm_v2_elf_align_up(namesz, 4)
		+ kvm_v2_elf_align_up(paylen, 4);
}

/*
 * Number of memslots with non-NULL data; each becomes a PT_LOAD.
 */
static int kvm_v2_elf_count_loadable(const struct kvm_v2_snapshot *snap)
{
	int i, n = 0;

	for (i = 0; i < snap->memslot_count; i++) {
		if (snap->memslots[i].data && snap->memslots[i].data_size > 0)
			n++;
	}
	return n;
}

/*
 * Emit the legacy NT_FPREGSET note. NT_FPREGSET expects a 512-byte
 * i387 FXSAVE-shape area (struct user_i387_struct on x86_64). The
 * captured XSAVE region's first 512 bytes ARE exactly that legacy
 * area - the AMD64 ABI defines XSAVE as "legacy area || extended
 * header || components." We emit those first 512 bytes verbatim.
 *
 * Returns the size that was emitted so the caller can sum into the
 * notes-segment total. paylen is always 512 (sizeof legacy area) so
 * just hard-code rather than threading a runtime size through.
 */
#define KVM_V2_ELF_FPREGSET_SIZE	512u

static int kvm_v2_elf_emit_fpregset(struct kvm_v2_elf_writer *writer,
				    loff_t *pos,
				    const struct kvm_v2_snapshot *snap)
{
	return kvm_v2_elf_emit_note(writer, pos, "CORE", NT_PRFPREG,
				    &snap->xsave,
				    KVM_V2_ELF_FPREGSET_SIZE);
}

/*
 * Size of the NT_X86_XSTATE payload. struct kvm_xsave is the static
 * 4 KB (1024 x __u32) shape KVM_GET_XSAVE returns; gdb reads the
 * leading XSAVE header (legacy area + XSTATE_BV) to know which
 * components are present.
 */
#define KVM_V2_ELF_X86_XSTATE_SIZE	sizeof(struct kvm_xsave)

/*
 * Build the NT_PRSTATUS payload in a stack-allocated buffer.
 * struct elf_prstatus is ~144 B; well under the frame-size cap.
 */
static void kvm_v2_elf_build_prstatus(const struct kvm_v2_snapshot *snap,
				      struct elf_prstatus *ps)
{
	memset(ps, 0, sizeof(*ps));
	ps->common.pr_pid = snap->task_source_pid ? snap->task_source_pid : 1;
	ps->pr_fpvalid    = 1;
	kvm_v2_elf_fill_user_regs(snap, (struct user_regs_struct *)&ps->pr_reg);
}

static int kvm_v2_snapshot_elf_export_to_writer(const struct kvm_v2_snapshot *snap,
						struct kvm_v2_elf_writer *writer)
{
	struct elf64_hdr ehdr;
	struct elf64_phdr *phdrs = NULL;
	struct elf_prstatus prstatus;
	void *uml_payload = NULL;
	size_t uml_payload_len;
	size_t notes_size;
	int n_load;
	int n_phdr;
	loff_t pos = 0;
	loff_t notes_off;
	loff_t loads_off;
	loff_t cur_load_off;
	int i;
	int rc;

	if (!snap || !writer || (!writer->file && writer->host_fd < 0))
		return -EINVAL;

	n_load  = kvm_v2_elf_count_loadable(snap);
	n_phdr  = 1 + n_load;	/* one PT_NOTE plus per-memslot PT_LOAD. */

	uml_payload_len = kvm_v2_elf_uml_payload_size(snap);
	uml_payload = kvzalloc(uml_payload_len, GFP_KERNEL);
	if (!uml_payload)
		return -ENOMEM;
	kvm_v2_elf_build_uml_payload(snap, uml_payload);

	notes_size = kvm_v2_elf_note_size("CORE", sizeof(struct elf_prstatus))
		   + kvm_v2_elf_note_size("CORE", KVM_V2_ELF_FPREGSET_SIZE)
		   + kvm_v2_elf_note_size("LINUX", KVM_V2_ELF_X86_XSTATE_SIZE)
		   + kvm_v2_elf_note_size(KVM_V2_NT_UML_OWNER,
					  (u32)uml_payload_len);

	notes_off = sizeof(ehdr) + (size_t)n_phdr * sizeof(struct elf64_phdr);
	loads_off = kvm_v2_elf_align_up(notes_off + notes_size, PAGE_SIZE);

	phdrs = kvcalloc((size_t)n_phdr, sizeof(*phdrs), GFP_KERNEL);
	if (!phdrs) {
		rc = -ENOMEM;
		goto out_free_uml;
	}

	/* PT_NOTE phdr. */
	phdrs[0].p_type   = PT_NOTE;
	phdrs[0].p_flags  = 0;
	phdrs[0].p_offset = notes_off;
	phdrs[0].p_vaddr  = 0;
	phdrs[0].p_paddr  = 0;
	phdrs[0].p_filesz = notes_size;
	phdrs[0].p_memsz  = 0;
	phdrs[0].p_align  = 4;

	cur_load_off = loads_off;
	{
		int ph = 1;

		for (i = 0; i < snap->memslot_count; i++) {
			const struct kvm_v2_memslot_snapshot *e =
				&snap->memslots[i];

			if (!e->data || e->data_size == 0)
				continue;
			phdrs[ph].p_type   = PT_LOAD;
			phdrs[ph].p_flags  = PF_R | PF_W;
			phdrs[ph].p_offset = cur_load_off;
			phdrs[ph].p_vaddr  = e->region.guest_phys_addr;
			phdrs[ph].p_paddr  = e->region.guest_phys_addr;
			phdrs[ph].p_filesz = e->data_size;
			phdrs[ph].p_memsz  = e->data_size;
			phdrs[ph].p_align  = PAGE_SIZE;
			cur_load_off += (loff_t)e->data_size;
			ph++;
		}
	}

	/* Elf64 ehdr. */
	memset(&ehdr, 0, sizeof(ehdr));
	memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
	ehdr.e_ident[EI_CLASS]   = ELFCLASS64;
	ehdr.e_ident[EI_DATA]    = ELFDATA2LSB;
	ehdr.e_ident[EI_VERSION] = EV_CURRENT;
	ehdr.e_ident[EI_OSABI]   = ELFOSABI_NONE;
	ehdr.e_type      = ET_CORE;
	ehdr.e_machine   = EM_X86_64;
	ehdr.e_version   = EV_CURRENT;
	ehdr.e_entry     = snap->regs.rip;
	ehdr.e_phoff     = sizeof(ehdr);
	ehdr.e_shoff     = 0;
	ehdr.e_flags     = 0;
	ehdr.e_ehsize    = sizeof(ehdr);
	ehdr.e_phentsize = sizeof(struct elf64_phdr);
	ehdr.e_phnum     = (u16)n_phdr;
	ehdr.e_shentsize = 0;
	ehdr.e_shnum     = 0;
	ehdr.e_shstrndx  = 0;

	rc = kvm_v2_elf_write(writer, &pos, &ehdr, sizeof(ehdr));
	if (rc < 0)
		goto out_free_phdrs;

	rc = kvm_v2_elf_write(writer, &pos, phdrs,
			      (size_t)n_phdr * sizeof(*phdrs));
	if (rc < 0)
		goto out_free_phdrs;

	/* Notes section. */
	kvm_v2_elf_build_prstatus(snap, &prstatus);
	rc = kvm_v2_elf_emit_note(writer, &pos, "CORE", NT_PRSTATUS,
				  &prstatus, sizeof(prstatus));
	if (rc < 0)
		goto out_free_phdrs;

	rc = kvm_v2_elf_emit_fpregset(writer, &pos, snap);
	if (rc < 0)
		goto out_free_phdrs;

	rc = kvm_v2_elf_emit_note(writer, &pos, "LINUX", NT_X86_XSTATE,
				  &snap->xsave,
				  KVM_V2_ELF_X86_XSTATE_SIZE);
	if (rc < 0)
		goto out_free_phdrs;

	rc = kvm_v2_elf_emit_note(writer, &pos, KVM_V2_NT_UML_OWNER,
				  KVM_V2_NT_UML_STATE,
				  uml_payload, (u32)uml_payload_len);
	if (rc < 0)
		goto out_free_phdrs;

	/* Pad to first PT_LOAD's p_offset. */
	if (n_load > 0) {
		rc = kvm_v2_elf_pad_zero(writer, &pos, loads_off);
		if (rc < 0)
			goto out_free_phdrs;
	}

	/* PT_LOAD payloads back-to-back. */
	for (i = 0; i < snap->memslot_count; i++) {
		const struct kvm_v2_memslot_snapshot *e = &snap->memslots[i];

		if (!e->data || e->data_size == 0)
			continue;
		rc = kvm_v2_elf_write(writer, &pos, e->data, e->data_size);
		if (rc < 0)
			goto out_free_phdrs;
	}

	pr_info("um: kvm-v2 snapshot elf: exported %d phdr (1 PT_NOTE + %d PT_LOAD), %zu bytes notes\n",
		n_phdr, n_load, notes_size);

	rc = 0;

out_free_phdrs:
	kvfree(phdrs);
out_free_uml:
	kvfree(uml_payload);
	return rc;
}

/**
 * kvm_v2_snapshot_elf_export_to_file - write an ELF64 core file
 *                                      from an in-memory snapshot.
 * @snap: previously-captured snapshot. Must not be NULL.
 * @file: open file for write. The export is sequential - caller
 *        should pass an empty/truncated file at position 0.
 *
 * Layout written:
 *
 *   [ Elf64_Ehdr                      ]
 *   [ Elf64_Phdr * (1 + N_load)       ]   PT_NOTE + N PT_LOAD
 *   [ note payloads (back-to-back)    ]
 *   [ PT_LOAD payloads                ]
 *
 * Returns 0 on success, -errno on the first failed write.
 *
 * Documented user-visible behaviours:
 *   - For regs-only snapshots (snap->memslots == NULL), N_load == 0
 *     and the file consists of header + one PT_NOTE phdr + notes.
 *     `readelf -n` and `gdb -c <file>` both accept this shape.
 *   - For full snapshots, each memslot with .data != NULL becomes one
 *     PT_LOAD with p_vaddr == guest_phys_addr.  gdb's `x` against a
 *     guest physical address shows the captured bytes.
 *   - The UML-private note's payload begins with a 'UMLE' magic +
 *     version so future readers can refuse mismatched layouts cleanly
 *     instead of mis-parsing.
 */
int kvm_v2_snapshot_elf_export_to_file(const struct kvm_v2_snapshot *snap,
				       struct file *file)
{
	struct kvm_v2_elf_writer writer = {
		.file = file,
		.host_fd = -1,
	};

	return kvm_v2_snapshot_elf_export_to_writer(snap, &writer);
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_elf_export_to_file);

static int kvm_v2_snapshot_elf_export_to_host_fd(const struct kvm_v2_snapshot *snap,
						 int fd)
{
	struct kvm_v2_elf_writer writer = {
		.file = NULL,
		.host_fd = fd,
	};

	return kvm_v2_snapshot_elf_export_to_writer(snap, &writer);
}

/**
 * kvm_v2_snapshot_elf_export_to_fd - convenience wrapper: take an fd,
 *                                    fget it, forward to _to_file.
 * @snap: snapshot to write.
 * @fd:   open writable fd (typically a memfd or an open(O_WRONLY)
 *        regular file).
 */
int kvm_v2_snapshot_elf_export_to_fd(const struct kvm_v2_snapshot *snap, int fd)
{
	struct file *f;
	int rc;

	f = fget(fd);
	if (!f)
		return -EBADF;
	if (!(f->f_mode & FMODE_WRITE)) {
		fput(f);
		return -EBADF;
	}
	rc = kvm_v2_snapshot_elf_export_to_file(snap, f);
	fput(f);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_v2_snapshot_elf_export_to_fd);

/*
 * Single shared lock for path-triggered exports. This avoids concurrent
 * debugfs or mconsole writes racing the kernel-side capture/restore
 * primitives.
 */
static DEFINE_MUTEX(kvm_v2_elf_export_lock);

static int kvm_v2_snapshot_capture_for_export(struct kvm_v2_snapshot *snap)
{
	int rc;

	rc = kvm_v2_snapshot_capture(snap);
	if (rc == -EOPNOTSUPP || rc == -ENOMEM) {
		/*
		 * Full capture not viable (e.g. tight vmalloc late in
		 * boot); fall back to regs-only so the operator still
		 * gets a usable ELF file with NT_PRSTATUS + XSAVE notes.
		 */
		pr_info("um: kvm-v2 snapshot elf: full capture rc=%d; falling back to regs-only\n",
			rc);
		rc = kvm_v2_snapshot_capture_regs_only(snap);
	}
	return rc;
}

static int __kvm_v2_snapshot_elf_export_path(const char *path)
{
	struct kvm_v2_snapshot *snap;
	struct file *f;
	int rc;

	if (!path || !*path)
		return -EINVAL;

	snap = kvm_v2_snapshot_alloc();
	if (!snap)
		return -ENOMEM;

	rc = kvm_v2_snapshot_capture_for_export(snap);
	if (rc < 0)
		goto out_destroy;

	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (IS_ERR(f)) {
		rc = PTR_ERR(f);
		pr_warn("um: kvm-v2 snapshot elf: filp_open(%s) rc=%d\n",
			path, rc);
		goto out_destroy;
	}

	rc = kvm_v2_snapshot_elf_export_to_file(snap, f);
	if (rc < 0)
		pr_warn("um: kvm-v2 snapshot elf: export rc=%d\n", rc);

	filp_close(f, NULL);

out_destroy:
	kvm_v2_snapshot_destroy(snap);
	return rc;
}

static int __kvm_v2_snapshot_elf_export_host_path(const char *path)
{
	struct kvm_v2_snapshot *snap;
	int fd;
	int rc;

	if (!path || !*path)
		return -EINVAL;

	snap = kvm_v2_snapshot_alloc();
	if (!snap)
		return -ENOMEM;

	rc = kvm_v2_snapshot_capture_for_export(snap);
	if (rc < 0)
		goto out_destroy;

	fd = os_open_file(path, of_trunc(of_create(of_write(OPENFLAGS()))),
			  0600);
	if (fd < 0) {
		rc = fd;
		pr_warn("um: kvm-v2 snapshot elf: host open(%s) rc=%d\n",
			path, rc);
		goto out_destroy;
	}

	rc = kvm_v2_snapshot_elf_export_to_host_fd(snap, fd);
	if (rc < 0)
		pr_warn("um: kvm-v2 snapshot elf: host export rc=%d\n", rc);

	os_close_file(fd);

out_destroy:
	kvm_v2_snapshot_destroy(snap);
	return rc;
}

static int kvm_v2_snapshot_elf_export_path(const char *path)
{
	int rc;

	mutex_lock(&kvm_v2_elf_export_lock);
	rc = __kvm_v2_snapshot_elf_export_path(path);
	mutex_unlock(&kvm_v2_elf_export_lock);

	return rc;
}

static int kvm_v2_snapshot_elf_export_host_path(const char *path)
{
	int rc;

	mutex_lock(&kvm_v2_elf_export_lock);
	rc = __kvm_v2_snapshot_elf_export_host_path(path);
	mutex_unlock(&kvm_v2_elf_export_lock);

	return rc;
}

#ifdef CONFIG_MCONSOLE
void mconsole_snapshot_export(struct mc_request *req)
{
	char *path = req->request.data;
	int rc;

	path += strlen("snapshot_export");
	path = skip_spaces(path);
	if (!*path) {
		mconsole_reply(req, "snapshot_export requires a path", 1, 0);
		return;
	}

	rc = kvm_v2_snapshot_elf_export_host_path(path);
	if (rc < 0) {
		char reply[128];

		snprintf(reply, sizeof(reply),
			 "snapshot_export failed: %d", rc);
		mconsole_reply(req, reply, 1, 0);
		return;
	}

	mconsole_reply(req, "snapshot_export complete", 0, 0);
}
#endif

/*
 * debugfs trigger: writing a path to
 *   /sys/kernel/debug/um/kvm_v2_snapshot_elf_export_path
 * causes the kernel to capture a fresh snapshot AND write it to the
 * named path as ET_CORE. The interface is stateless: every write captures
 * a fresh snapshot and exports it synchronously.
 */
#ifdef CONFIG_DEBUG_FS

/*
 * Maximum path length we'll accept on a single write. Bounded
 * deliberately: a debugfs write of "give us the whole filesystem"
 * isn't a meaningful operation, and the heap allocation should be
 * pageable on a tight UML config.
 */
#define KVM_V2_ELF_PATH_MAX	4095u

static ssize_t kvm_v2_elf_path_write(struct file *f,
				     const char __user *buf,
				     size_t count, loff_t *ppos)
{
	char *path;
	size_t copy_n;
	int rc;

	if (count == 0)
		return -EINVAL;
	copy_n = min_t(size_t, count, KVM_V2_ELF_PATH_MAX);
	path = kzalloc(copy_n + 1, GFP_KERNEL);
	if (!path)
		return -ENOMEM;
	if (copy_from_user(path, buf, copy_n)) {
		kfree(path);
		return -EFAULT;
	}
	path[copy_n] = '\0';
	if (copy_n > 0 && path[copy_n - 1] == '\n')
		path[copy_n - 1] = '\0';

	rc = kvm_v2_snapshot_elf_export_path(path);

	kfree(path);
	return rc < 0 ? rc : (ssize_t)count;
}

static const struct file_operations kvm_v2_elf_path_fops = {
	.write = kvm_v2_elf_path_write,
};

static int __init kvm_v2_snapshot_elf_debugfs_init(void)
{
	struct dentry *d;

	d = debugfs_lookup("um", NULL);
	if (!d) {
		d = debugfs_create_dir("um", NULL);
		if (IS_ERR(d))
			return PTR_ERR(d);
	}

	debugfs_create_file("kvm_v2_snapshot_elf_export_path", 0200, d,
			    NULL, &kvm_v2_elf_path_fops);
	return 0;
}
late_initcall_sync(kvm_v2_snapshot_elf_debugfs_init);

#endif /* CONFIG_DEBUG_FS */
