// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side raw-time payload smoke for strict KVM v2 replay.
 *
 * The helper records direct syscall and UML vDSO clock_gettime(2),
 * gettimeofday(2), and time(2) results, arms replay, and verifies that replay
 * returns the recorded time bytes instead of consulting host time again.
 */

#define _GNU_SOURCE

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CTL_PATH	"/sys/kernel/debug/um/kvm_v2_record_ctl"

#ifndef SYS_clock_gettime
#define SYS_clock_gettime	228
#endif
#ifndef SYS_gettimeofday
#define SYS_gettimeofday	96
#endif
#ifndef SYS_time
#define SYS_time		201
#endif

#ifndef AT_SYSINFO_EHDR
#define AT_SYSINFO_EHDR		33
#endif

struct time_sample {
	struct timespec ts;
	struct timeval tv;
	struct timezone tz;
	long time_value;
	long time_ret;
};

typedef int (*vdso_clock_gettime_t)(clockid_t, struct timespec *);
typedef int (*vdso_gettimeofday_t)(struct timeval *, struct timezone *);
typedef long (*vdso_time_t)(long *);

struct vdso_time_ops {
	vdso_clock_gettime_t clock_gettime;
	vdso_gettimeofday_t gettimeofday;
	vdso_time_t time;
};

struct local_elf_ehdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct local_elf_phdr {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

struct local_elf_dyn {
	int64_t d_tag;
	union {
		uint64_t d_val;
		uint64_t d_ptr;
	} d_un;
};

struct local_elf_sym {
	uint32_t st_name;
	unsigned char st_info;
	unsigned char st_other;
	uint16_t st_shndx;
	uint64_t st_value;
	uint64_t st_size;
};

static int ensure_dir(const char *path)
{
	if (!mkdir(path, 0755) || errno == EEXIST)
		return 0;
	return -1;
}

static int mount_if_needed(const char *type, const char *target)
{
	if (!mount("none", target, type, 0, "") || errno == EBUSY)
		return 0;
	return -1;
}

static void setup_mounts(void)
{
	(void)ensure_dir("/proc");
	(void)ensure_dir("/sys");
	(void)ensure_dir("/sys/kernel");
	(void)ensure_dir("/sys/kernel/debug");
	(void)mount_if_needed("proc", "/proc");
	(void)mount_if_needed("debugfs", "/sys/kernel/debug");
}

static int write_ctl_fd(int fd, const char *cmd)
{
	size_t len = strlen(cmd);
	ssize_t written = write(fd, cmd, len);

	return written == (ssize_t)len ? 0 : -1;
}

static int write_ctl(const char *cmd)
{
	int fd;
	int rc;

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	rc = write_ctl_fd(fd, cmd);
	if (close(fd) < 0)
		return -1;
	return rc;
}

static void *vdso_runtime_addr(uintptr_t load_bias, uint64_t addr)
{
	return (void *)(load_bias + (uintptr_t)addr);
}

static int vdso_validate_ehdr(const struct local_elf_ehdr *ehdr)
{
	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG))
		return -1;
	if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
		return -1;
	if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
		return -1;
	if (ehdr->e_ident[EI_VERSION] != EV_CURRENT)
		return -1;
	if (ehdr->e_phentsize != sizeof(struct local_elf_phdr))
		return -1;
	return 0;
}

static int vdso_time_ops_init(struct vdso_time_ops *ops)
{
	const struct local_elf_ehdr *ehdr;
	const struct local_elf_phdr *phdr;
	const struct local_elf_dyn *dyn = NULL;
	const struct local_elf_sym *symtab = NULL;
	const char *strtab = NULL;
	const uint32_t *hash = NULL;
	uintptr_t base = getauxval(AT_SYSINFO_EHDR);
	uintptr_t load_bias = 0;
	size_t dyn_count = 0;
	size_t syment = 0;
	uint32_t nchain;
	bool have_load = false;
	int i;

	memset(ops, 0, sizeof(*ops));
	if (!base)
		return -1;

	ehdr = (const struct local_elf_ehdr *)base;
	if (vdso_validate_ehdr(ehdr) < 0)
		return -1;

	phdr = (const struct local_elf_phdr *)(base + ehdr->e_phoff);
	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type != PT_LOAD)
			continue;
		load_bias = base - (uintptr_t)phdr[i].p_vaddr;
		have_load = true;
		break;
	}
	if (!have_load)
		return -1;

	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type != PT_DYNAMIC)
			continue;
		dyn = vdso_runtime_addr(load_bias, phdr[i].p_vaddr);
		dyn_count = phdr[i].p_memsz / sizeof(*dyn);
		break;
	}
	if (!dyn || !dyn_count)
		return -1;

	for (i = 0; i < (int)dyn_count && dyn[i].d_tag != DT_NULL; i++) {
		switch (dyn[i].d_tag) {
		case DT_HASH:
			hash = vdso_runtime_addr(load_bias, dyn[i].d_un.d_ptr);
			break;
		case DT_STRTAB:
			strtab = vdso_runtime_addr(load_bias, dyn[i].d_un.d_ptr);
			break;
		case DT_SYMTAB:
			symtab = vdso_runtime_addr(load_bias, dyn[i].d_un.d_ptr);
			break;
		case DT_SYMENT:
			syment = dyn[i].d_un.d_val;
			break;
		}
	}

	if (!hash || !strtab || !symtab)
		return -1;
	if (syment && syment != sizeof(*symtab))
		return -1;

	nchain = hash[1];
	for (i = 0; i < (int)nchain; i++) {
		const struct local_elf_sym *sym = &symtab[i];
		const char *name;
		void *addr;

		if (!sym->st_name || sym->st_shndx == SHN_UNDEF)
			continue;

		name = strtab + sym->st_name;
		addr = vdso_runtime_addr(load_bias, sym->st_value);
		if (!strcmp(name, "__vdso_clock_gettime"))
			ops->clock_gettime = (vdso_clock_gettime_t)addr;
		else if (!strcmp(name, "__vdso_gettimeofday"))
			ops->gettimeofday = (vdso_gettimeofday_t)addr;
		else if (!strcmp(name, "__vdso_time"))
			ops->time = (vdso_time_t)addr;
	}

	return ops->clock_gettime && ops->gettimeofday && ops->time ? 0 : -1;
}

static int sample_syscalls(struct time_sample *sample, const char *phase)
{
	long rc;

	memset(sample, 0, sizeof(*sample));

	rc = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &sample->ts);
	if (rc < 0) {
		printf("KVM_RECORD_TIME: FAIL %s syscall clock_gettime rc=%ld errno=%d\n",
		       phase, rc, errno);
		return -1;
	}

	rc = syscall(SYS_gettimeofday, &sample->tv, &sample->tz);
	if (rc < 0) {
		printf("KVM_RECORD_TIME: FAIL %s syscall gettimeofday rc=%ld errno=%d\n",
		       phase, rc, errno);
		return -1;
	}

	sample->time_ret = syscall(SYS_time, &sample->time_value);
	if (sample->time_ret < 0) {
		printf("KVM_RECORD_TIME: FAIL %s syscall time rc=%ld errno=%d\n",
		       phase, sample->time_ret, errno);
		return -1;
	}
	if (sample->time_ret != sample->time_value) {
		printf("KVM_RECORD_TIME: FAIL %s syscall time ret=%ld stored=%ld\n",
		       phase, sample->time_ret, sample->time_value);
		return -1;
	}

	return 0;
}

static int sample_vdso(const struct vdso_time_ops *ops,
		       struct time_sample *sample, const char *phase)
{
	long rc;

	memset(sample, 0, sizeof(*sample));

	rc = ops->clock_gettime(CLOCK_MONOTONIC, &sample->ts);
	if (rc < 0) {
		printf("KVM_RECORD_TIME: FAIL %s vdso clock_gettime rc=%ld\n",
		       phase, rc);
		return -1;
	}

	rc = ops->gettimeofday(&sample->tv, &sample->tz);
	if (rc < 0) {
		printf("KVM_RECORD_TIME: FAIL %s vdso gettimeofday rc=%ld\n",
		       phase, rc);
		return -1;
	}

	sample->time_ret = ops->time(&sample->time_value);
	if (sample->time_ret < 0) {
		printf("KVM_RECORD_TIME: FAIL %s vdso time rc=%ld\n",
		       phase, sample->time_ret);
		return -1;
	}
	if (sample->time_ret != sample->time_value) {
		printf("KVM_RECORD_TIME: FAIL %s vdso time ret=%ld stored=%ld\n",
		       phase, sample->time_ret, sample->time_value);
		return -1;
	}

	return 0;
}

static int compare_samples(const char *label, const struct time_sample *recorded,
			   const struct time_sample *replayed)
{
	if (recorded->ts.tv_sec != replayed->ts.tv_sec ||
	    recorded->ts.tv_nsec != replayed->ts.tv_nsec) {
		printf("KVM_RECORD_TIME: FAIL %s clock mismatch ", label);
		printf("recorded=%ld.%09ld replayed=%ld.%09ld\n",
		       (long)recorded->ts.tv_sec, recorded->ts.tv_nsec,
		       (long)replayed->ts.tv_sec, replayed->ts.tv_nsec);
		return -1;
	}

	if (recorded->tv.tv_sec != replayed->tv.tv_sec ||
	    recorded->tv.tv_usec != replayed->tv.tv_usec ||
	    recorded->tz.tz_minuteswest != replayed->tz.tz_minuteswest ||
	    recorded->tz.tz_dsttime != replayed->tz.tz_dsttime) {
		printf("KVM_RECORD_TIME: FAIL %s gettimeofday mismatch ", label);
		printf("recorded=%ld.%06ld/%d/%d ",
		       (long)recorded->tv.tv_sec, (long)recorded->tv.tv_usec,
		       recorded->tz.tz_minuteswest, recorded->tz.tz_dsttime);
		printf("replayed=%ld.%06ld/%d/%d\n",
		       (long)replayed->tv.tv_sec, (long)replayed->tv.tv_usec,
		       replayed->tz.tz_minuteswest, replayed->tz.tz_dsttime);
		return -1;
	}

	if (recorded->time_ret != replayed->time_ret ||
	    recorded->time_value != replayed->time_value) {
		printf("KVM_RECORD_TIME: FAIL %s time mismatch ", label);
		printf("recorded_ret=%ld recorded=%ld ",
		       recorded->time_ret, recorded->time_value);
		printf("replayed_ret=%ld replayed=%ld\n",
		       replayed->time_ret, replayed->time_value);
		return -1;
	}

	return 0;
}

int main(void)
{
	struct time_sample recorded_syscall;
	struct time_sample replayed_syscall;
	struct time_sample recorded_vdso;
	struct time_sample replayed_vdso;
	struct vdso_time_ops vdso;
	int fd;

	setup_mounts();

	if (vdso_time_ops_init(&vdso) < 0) {
		printf("KVM_RECORD_TIME: FAIL resolve UML vDSO time symbols\n");
		return 1;
	}

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_TIME: FAIL open ctl errno=%d\n", errno);
		return 1;
	}
	if (write_ctl_fd(fd, "start 4096\n") < 0) {
		printf("KVM_RECORD_TIME: FAIL start errno=%d\n", errno);
		(void)close(fd);
		return 1;
	}

	if (sample_syscalls(&recorded_syscall, "record") < 0 ||
	    sample_vdso(&vdso, &recorded_vdso, "record") < 0) {
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (write_ctl_fd(fd, "stop\n") < 0) {
		printf("KVM_RECORD_TIME: FAIL stop errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (close(fd) < 0) {
		printf("KVM_RECORD_TIME: FAIL close ctl errno=%d\n", errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	fd = open(CTL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("KVM_RECORD_TIME: FAIL open replay ctl errno=%d\n",
		       errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (write_ctl_fd(fd, "replay\n") < 0) {
		printf("KVM_RECORD_TIME: FAIL replay errno=%d\n", errno);
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (sample_syscalls(&replayed_syscall, "replay") < 0 ||
	    sample_vdso(&vdso, &replayed_vdso, "replay") < 0) {
		(void)close(fd);
		(void)write_ctl("destroy\n");
		return 1;
	}
	if (close(fd) < 0) {
		printf("KVM_RECORD_TIME: FAIL close replay ctl errno=%d\n",
		       errno);
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (compare_samples("syscall", &recorded_syscall, &replayed_syscall) < 0 ||
	    compare_samples("vdso", &recorded_vdso, &replayed_vdso) < 0) {
		(void)write_ctl("destroy\n");
		return 1;
	}

	if (write_ctl("destroy\n") < 0) {
		printf("KVM_RECORD_TIME: FAIL destroy errno=%d\n", errno);
		return 1;
	}

	printf("KVM_RECORD_TIME: PASS syscall_clock=%ld.%09ld ",
	       (long)replayed_syscall.ts.tv_sec, replayed_syscall.ts.tv_nsec);
	printf("vdso_clock=%ld.%09ld syscall_time=%ld vdso_time=%ld\n",
	       (long)replayed_vdso.ts.tv_sec, replayed_vdso.ts.tv_nsec,
	       replayed_syscall.time_value, replayed_vdso.time_value);
	return 0;
}
