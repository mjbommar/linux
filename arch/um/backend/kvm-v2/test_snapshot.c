// SPDX-License-Identifier: GPL-2.0
/*
 * KVM v2 snapshot KUnit coverage.
 *
 * These tests need a live KVM v2 VM and vCPU pool. When the suite is built
 * into a kernel that did not initialise KVM v2, each case is skipped rather
 * than reported as a false failure.
 */
#include <kunit/test.h>
#include <linux/elf.h>
#include <linux/elfcore.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/user.h>
#include <linux/vmalloc.h>

#include <os.h>

#include "kvm_v2_backend.h"

#define KVM_V2_SNAPSHOT_TEST_MARKER	0xa5a5a5a5cafebabeULL
#define KVM_V2_SNAPSHOT_TEST_GARBAGE	0x0badf00d0badf00dULL
#define KVM_V2_SNAPSHOT_TEST_SLOT_GPA	0x4000000000ULL
#define KVM_V2_SNAPSHOT_ELF_READ_SIZE	16384

static struct kvm_v2_vcpu *kvm_v2_snapshot_live_vcpu(struct kunit *test)
{
	struct kvm_v2_vcpu *first = NULL;
	struct kvm_v2_vcpu *vcpu;
	int cpu;
	int rc;

	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		vcpu = kvm_v2_vcpu_get(cpu);
		if (!vcpu || vcpu->vcpu_fd < 0)
			continue;

		if (!first)
			first = vcpu;

		rc = kvm_v2_vcpu_prime_for_kunit(vcpu);
		if (rc < 0) {
			KUNIT_FAIL(test,
				   "prime vCPU cpu=%d fd=%d failed: %d",
				   vcpu->cpu, vcpu->vcpu_fd, rc);
			return NULL;
		}
	}

	if (!first)
		kunit_skip(test, "KVM v2 vCPU pool is not available");

	return first;
}

static int kvm_v2_snapshot_count_loadable(const struct kvm_v2_snapshot *snap)
{
	int i;
	int count = 0;

	if (!snap || !snap->memslots)
		return 0;

	for (i = 0; i < snap->memslot_count; i++)
		if (snap->memslots[i].data && snap->memslots[i].data_size)
			count++;

	return count;
}

static bool kvm_v2_snapshot_has_slot_data(const struct kvm_v2_snapshot *snap,
					  u32 slot_id)
{
	int i;

	if (!snap || !snap->memslots)
		return false;

	for (i = 0; i < snap->memslot_count; i++) {
		const struct kvm_v2_memslot_snapshot *slot = &snap->memslots[i];

		if (slot->region.slot == slot_id && slot->data &&
		    slot->data_size)
			return true;
	}

	return false;
}

static void kvm_v2_snapshot_delete_test_slot(struct kvm_v2_vm *vm,
					     struct kvm_userspace_memory_region *region,
					     int slot_id)
{
	int rc;

	if (!vm || slot_id < 0)
		return;

	if (region) {
		region->memory_size = 0;
		rc = os_ioctl_generic(vm->vm_fd, KVM_SET_USER_MEMORY_REGION,
				      (unsigned long)region);
		if (rc < 0)
			pr_warn("um: kvm-v2 snapshot kunit: delete test memslot %d failed: %d\n",
				slot_id, rc);
	}

	kvm_v2_memslot_del(vm, (u32)slot_id);
}

static void test_kvm_v2_snapshot_regs_only(struct kunit *test)
{
	struct kvm_v2_snapshot *snap;
	int rc;

	if (!kvm_v2_snapshot_live_vcpu(test))
		return;

	snap = kvm_v2_snapshot_alloc();
	KUNIT_ASSERT_NOT_NULL(test, snap);

	rc = kvm_v2_snapshot_capture_regs_only(snap);
	KUNIT_EXPECT_EQ_MSG(test, rc, 0, "capture_regs_only rc=%d", rc);
	KUNIT_EXPECT_EQ(test, snap->msrs.nmsrs,
			(__u32)KVM_V2_SNAPSHOT_MSR_COUNT);
	KUNIT_EXPECT_GE(test, snap->xcrs.nr_xcrs, (__u32)1);

	kvm_v2_snapshot_destroy(snap);
}

static void test_kvm_v2_snapshot_full_memslot(struct kunit *test)
{
	struct kvm_v2_snapshot *snap = NULL;
	struct kvm_v2_vcpu *vcpu;
	struct kvm_v2_vm *vm;
	struct kvm_userspace_memory_region region = {0};
	struct kvm_regs regs;
	unsigned long scratch_page = 0;
	u64 *marker;
	int slot_id = -1;
	int rc;
	bool has_slot_data;

	vcpu = kvm_v2_snapshot_live_vcpu(test);
	if (!vcpu)
		return;

	vm = kvm_v2_vm_get();
	KUNIT_ASSERT_NOT_NULL(test, vm);

	scratch_page = __get_free_page(GFP_KERNEL | __GFP_ZERO);
	KUNIT_ASSERT_NE(test, scratch_page, 0UL);
	marker = (u64 *)scratch_page;

	slot_id = kvm_v2_memslot_add(vm, KVM_V2_SNAPSHOT_TEST_SLOT_GPA,
				     scratch_page, PAGE_SIZE, 0);
	if (slot_id < 0) {
		KUNIT_FAIL(test, "kvm_v2_memslot_add failed: %d", slot_id);
		goto out;
	}

	region = (struct kvm_userspace_memory_region){
		.slot = (u32)slot_id,
		.flags = 0,
		.guest_phys_addr = KVM_V2_SNAPSHOT_TEST_SLOT_GPA,
		.memory_size = PAGE_SIZE,
		.userspace_addr = scratch_page,
	};

	rc = os_ioctl_generic(vm->vm_fd, KVM_SET_USER_MEMORY_REGION,
			      (unsigned long)&region);
	if (rc < 0) {
		KUNIT_FAIL(test, "KVM_SET_USER_MEMORY_REGION failed: %d", rc);
		goto out;
	}

	*marker = KVM_V2_SNAPSHOT_TEST_MARKER;

	snap = kvm_v2_snapshot_alloc();
	if (!snap) {
		KUNIT_FAIL(test, "kvm_v2_snapshot_alloc failed");
		goto out;
	}

	rc = kvm_v2_snapshot_capture_full(snap, vcpu);
	if (rc < 0) {
		KUNIT_FAIL(test, "capture_full failed: %d", rc);
		goto out;
	}

	has_slot_data = kvm_v2_snapshot_has_slot_data(snap, (u32)slot_id);
	KUNIT_EXPECT_TRUE(test, has_slot_data);

	*marker = KVM_V2_SNAPSHOT_TEST_GARBAGE;

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&regs);
	if (rc < 0) {
		KUNIT_FAIL(test, "KVM_GET_REGS failed: %d", rc);
		goto out;
	}

	regs.rax = 0xdeadbeefdeadbeefULL;
	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_REGS,
			      (unsigned long)&regs);
	if (rc < 0) {
		KUNIT_FAIL(test, "KVM_SET_REGS failed: %d", rc);
		goto out;
	}

	rc = kvm_v2_snapshot_restore_full_vcpu(snap, vcpu);
	if (rc < 0) {
		KUNIT_FAIL(test, "restore_full_vcpu failed: %d", rc);
		goto out;
	}

	KUNIT_EXPECT_EQ(test, *marker, (u64)KVM_V2_SNAPSHOT_TEST_MARKER);

	rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&regs);
	if (rc < 0) {
		KUNIT_FAIL(test, "KVM_GET_REGS after restore failed: %d", rc);
		goto out;
	}
	KUNIT_EXPECT_EQ(test, regs.rax, snap->regs.rax);

out:
	kvm_v2_snapshot_destroy(snap);
	kvm_v2_snapshot_delete_test_slot(vm, &region, slot_id);
	if (scratch_page)
		free_page(scratch_page);
}

static void test_kvm_v2_snapshot_task_state(struct kunit *test)
{
	struct kvm_v2_snapshot *snap = NULL;
	struct kvm_v2_snapshot *empty = NULL;
	struct kvm_v2_vcpu *vcpu;
	struct kvm_xsave *saved_fpu;
	struct kvm_vcpu_events saved_events;
	bool saved_fpu_valid;
	bool saved_events_valid;
	const u8 marker_byte = 0xa5;
	const u8 garbage_byte = 0x5a;
	int rc;

	vcpu = kvm_v2_snapshot_live_vcpu(test);
	if (!vcpu)
		return;

	saved_fpu = kzalloc_obj(*saved_fpu, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, saved_fpu);

	*saved_fpu = current->thread.arch.kvm_v2.iotrap_fpu;
	saved_events = current->thread.arch.kvm_v2.iotrap_events;
	saved_fpu_valid = current->thread.arch.kvm_v2.iotrap_fpu_valid;
	saved_events_valid = current->thread.arch.kvm_v2.iotrap_events_valid;

	snap = kvm_v2_snapshot_alloc();
	if (!snap) {
		KUNIT_FAIL(test, "kvm_v2_snapshot_alloc failed");
		goto out_restore;
	}

	memset(&current->thread.arch.kvm_v2.iotrap_fpu, marker_byte,
	       sizeof(current->thread.arch.kvm_v2.iotrap_fpu));
	current->thread.arch.kvm_v2.iotrap_fpu_valid = true;
	memset(&current->thread.arch.kvm_v2.iotrap_events, marker_byte,
	       sizeof(current->thread.arch.kvm_v2.iotrap_events));
	current->thread.arch.kvm_v2.iotrap_events_valid = true;

	rc = kvm_v2_snapshot_capture_task(snap, vcpu);
	if (rc < 0) {
		KUNIT_FAIL(test, "capture_task failed: %d", rc);
		goto out_restore;
	}

	KUNIT_EXPECT_TRUE(test, snap->task_state_captured);
	KUNIT_EXPECT_TRUE(test, snap->task_iotrap_fpu_valid);
	KUNIT_EXPECT_TRUE(test, snap->task_iotrap_events_valid);
	KUNIT_EXPECT_EQ(test, snap->task_source_pid, current->pid);
	KUNIT_EXPECT_EQ(test, ((u8 *)&snap->task_iotrap_fpu)[0],
			marker_byte);

	memset(&current->thread.arch.kvm_v2.iotrap_fpu, garbage_byte,
	       sizeof(current->thread.arch.kvm_v2.iotrap_fpu));
	current->thread.arch.kvm_v2.iotrap_fpu_valid = false;
	memset(&current->thread.arch.kvm_v2.iotrap_events, garbage_byte,
	       sizeof(current->thread.arch.kvm_v2.iotrap_events));
	current->thread.arch.kvm_v2.iotrap_events_valid = false;

	rc = kvm_v2_snapshot_restore_task(snap, vcpu);
	if (rc < 0) {
		KUNIT_FAIL(test, "restore_task failed: %d", rc);
		goto out_restore;
	}

	KUNIT_EXPECT_TRUE(test, current->thread.arch.kvm_v2.iotrap_fpu_valid);
	KUNIT_EXPECT_TRUE(test,
			  current->thread.arch.kvm_v2.iotrap_events_valid);
	KUNIT_EXPECT_EQ(test,
			((u8 *)&current->thread.arch.kvm_v2.iotrap_fpu)[0],
			marker_byte);
	KUNIT_EXPECT_EQ(test,
			((u8 *)&current->thread.arch.kvm_v2.iotrap_events)[0],
			marker_byte);

	empty = kvm_v2_snapshot_alloc();
	if (!empty) {
		KUNIT_FAIL(test, "empty snapshot alloc failed");
		goto out_restore;
	}

	rc = kvm_v2_snapshot_restore_task(empty, vcpu);
	KUNIT_EXPECT_EQ(test, rc, -EINVAL);

out_restore:
	current->thread.arch.kvm_v2.iotrap_fpu = *saved_fpu;
	current->thread.arch.kvm_v2.iotrap_events = saved_events;
	current->thread.arch.kvm_v2.iotrap_fpu_valid = saved_fpu_valid;
	current->thread.arch.kvm_v2.iotrap_events_valid = saved_events_valid;
	kvm_v2_snapshot_destroy(empty);
	kvm_v2_snapshot_destroy(snap);
	kfree(saved_fpu);
}

static bool kvm_v2_note_prstatus_rax(const void *buf, size_t len,
				     const struct elf64_phdr *phdr,
				     u64 *rax)
{
	size_t off = phdr->p_offset;
	size_t end = phdr->p_offset + phdr->p_filesz;

	while (off + sizeof(struct elf64_note) <= end) {
		const struct elf64_note *nhdr = buf + off;
		size_t name_off = off + sizeof(*nhdr);
		size_t desc_off = name_off + ALIGN(nhdr->n_namesz, 4);
		size_t next = desc_off + ALIGN(nhdr->n_descsz, 4);
		const char *name = buf + name_off;
		const void *desc = buf + desc_off;
		const struct elf_prstatus *prstatus;
		const struct user_regs_struct *regs;

		if (name_off > len || desc_off > len || next > len ||
		    next > end)
			return false;

		if (nhdr->n_type == NT_PRSTATUS &&
		    nhdr->n_namesz == sizeof("CORE") &&
		    !memcmp(name, "CORE", sizeof("CORE"))) {
			if (nhdr->n_descsz < sizeof(*prstatus))
				return false;
			prstatus = desc;
			regs = (const struct user_regs_struct *)&prstatus->pr_reg;
			*rax = regs->ax;
			return true;
		}

		off = next;
	}

	return false;
}

static void test_kvm_v2_snapshot_elf_regs_only(struct kunit *test)
{
	struct kvm_v2_snapshot *snap = NULL;
	struct file *file = NULL;
	void *buf = NULL;
	struct elf64_hdr *ehdr;
	struct elf64_phdr *phdrs;
	ssize_t nread;
	loff_t pos = 0;
	u64 prstatus_rax = 0;
	bool saw_prstatus = false;
	int pt_note_count = 0;
	int pt_load_count = 0;
	int i;
	int rc;

	if (!kvm_v2_snapshot_live_vcpu(test))
		return;

	snap = kvm_v2_snapshot_alloc();
	KUNIT_ASSERT_NOT_NULL(test, snap);

	rc = kvm_v2_snapshot_capture_regs_only(snap);
	if (rc < 0) {
		KUNIT_FAIL(test, "capture_regs_only failed: %d", rc);
		goto out;
	}

	file = shmem_kernel_file_setup("kvm-v2-snapshot.elf", 0,
				       EMPTY_VMA_FLAGS);
	if (IS_ERR(file)) {
		KUNIT_FAIL(test, "shmem setup failed: %ld", PTR_ERR(file));
		file = NULL;
		goto out;
	}

	rc = kvm_v2_snapshot_elf_export_to_file(snap, file);
	if (rc < 0) {
		KUNIT_FAIL(test, "export_to_file failed: %d", rc);
		goto out;
	}

	buf = kvzalloc(KVM_V2_SNAPSHOT_ELF_READ_SIZE, GFP_KERNEL);
	if (!buf) {
		KUNIT_FAIL(test, "ELF read buffer allocation failed");
		goto out;
	}

	nread = kernel_read(file, buf, KVM_V2_SNAPSHOT_ELF_READ_SIZE, &pos);
	if (nread < (ssize_t)(sizeof(*ehdr) + sizeof(*phdrs))) {
		KUNIT_FAIL(test, "short ELF read: %zd", nread);
		goto out;
	}

	ehdr = buf;
	KUNIT_EXPECT_TRUE(test, !memcmp(ehdr->e_ident, ELFMAG, SELFMAG));
	KUNIT_EXPECT_EQ(test, ehdr->e_ident[EI_CLASS], (unsigned char)ELFCLASS64);
	KUNIT_EXPECT_EQ(test, ehdr->e_ident[EI_DATA], (unsigned char)ELFDATA2LSB);
	KUNIT_EXPECT_EQ(test, ehdr->e_type, (Elf64_Half)ET_CORE);
	KUNIT_EXPECT_EQ(test, ehdr->e_machine, (Elf64_Half)EM_X86_64);

	if (ehdr->e_phoff + (size_t)ehdr->e_phnum * sizeof(*phdrs) >
	    (size_t)nread) {
		KUNIT_FAIL(test, "program header table outside read buffer");
		goto out;
	}

	phdrs = buf + ehdr->e_phoff;
	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type == PT_NOTE) {
			pt_note_count++;
			saw_prstatus =
				kvm_v2_note_prstatus_rax(buf, (size_t)nread,
							 &phdrs[i],
							 &prstatus_rax);
		} else if (phdrs[i].p_type == PT_LOAD) {
			pt_load_count++;
		}
	}

	KUNIT_EXPECT_EQ(test, pt_note_count, 1);
	KUNIT_EXPECT_EQ(test, pt_load_count,
			kvm_v2_snapshot_count_loadable(snap));
	KUNIT_EXPECT_TRUE(test, saw_prstatus);
	KUNIT_EXPECT_EQ(test, prstatus_rax, snap->regs.rax);

out:
	kvfree(buf);
	if (file)
		fput(file);
	kvm_v2_snapshot_destroy(snap);
}

static struct kunit_case kvm_v2_snapshot_test_cases[] = {
	KUNIT_CASE(test_kvm_v2_snapshot_regs_only),
	KUNIT_CASE(test_kvm_v2_snapshot_full_memslot),
	KUNIT_CASE(test_kvm_v2_snapshot_task_state),
	KUNIT_CASE(test_kvm_v2_snapshot_elf_regs_only),
	{}
};

static struct kunit_suite kvm_v2_snapshot_suite = {
	.name = "um_kvm_v2_snapshot",
	.test_cases = kvm_v2_snapshot_test_cases,
};

kunit_test_suite(kvm_v2_snapshot_suite);
