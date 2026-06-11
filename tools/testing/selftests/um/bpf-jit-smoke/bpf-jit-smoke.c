// SPDX-License-Identifier: GPL-2.0
/*
 * Guest-side UML BPF JIT smoke test.
 *
 * Loads a minimal eBPF socket-filter program and checks that
 * BPF_OBJ_GET_INFO_BY_FD reports JITed code. Intended to run inside
 * a UML guest whose research profile enables CONFIG_BPF_JIT_ALWAYS_ON.
 */

#include <errno.h>
#include <linux/bpf.h>
#include <linux/bpf_common.h>
#include <linux/unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __NR_bpf
# error __NR_bpf is not defined for this architecture
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int sys_bpf(enum bpf_cmd cmd, union bpf_attr *attr, unsigned int size)
{
	return syscall(__NR_bpf, cmd, attr, size);
}

static int read_jit_sysctl(void)
{
	FILE *f;
	int value;

	f = fopen("/proc/sys/net/core/bpf_jit_enable", "r");
	if (!f)
		return -errno;
	if (fscanf(f, "%d", &value) != 1) {
		fclose(f);
		return -EINVAL;
	}
	fclose(f);
	return value;
}

static void raise_memlock_limit(void)
{
	struct rlimit rlim = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};

	(void)setrlimit(RLIMIT_MEMLOCK, &rlim);
}

int main(void)
{
	struct bpf_insn insns[] = {
		{
			.code = BPF_ALU64 | BPF_MOV | BPF_K,
			.dst_reg = BPF_REG_0,
			.src_reg = 0,
			.off = 0,
			.imm = 0,
		},
		{
			.code = BPF_JMP | BPF_EXIT,
			.dst_reg = 0,
			.src_reg = 0,
			.off = 0,
			.imm = 0,
		},
	};
	char license[] = "GPL";
	char log_buf[16384];
	union bpf_attr attr;
	struct bpf_prog_info info;
	union bpf_attr info_attr;
	int sysctl;
	int fd;

	sysctl = read_jit_sysctl();
	if (sysctl < 0) {
		if (sysctl == -ENOENT) {
			printf("BPF_JIT_SMOKE: SKIP bpf_jit_enable sysctl missing\n");
			return 4;
		}
		printf("BPF_JIT_SMOKE: FAIL read bpf_jit_enable err=%d\n", -sysctl);
		return 1;
	}
	if (sysctl != 1) {
		printf("BPF_JIT_SMOKE: FAIL bpf_jit_enable=%d expected=1\n",
		       sysctl);
		return 1;
	}

	raise_memlock_limit();

	memset(log_buf, 0, sizeof(log_buf));
	memset(&attr, 0, sizeof(attr));
	attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
	attr.insn_cnt = ARRAY_SIZE(insns);
	attr.insns = (uint64_t)(uintptr_t)insns;
	attr.license = (uint64_t)(uintptr_t)license;
	attr.log_buf = (uint64_t)(uintptr_t)log_buf;
	attr.log_size = sizeof(log_buf);
	attr.log_level = 1;

	fd = sys_bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
	if (fd < 0) {
		if (errno == EOPNOTSUPP) {
			printf("BPF_JIT_SMOKE: SKIP bpf syscall unavailable errno=%d\n",
			       errno);
			return 4;
		}
		printf("BPF_JIT_SMOKE: FAIL BPF_PROG_LOAD errno=%d log=%s\n",
		       errno, log_buf);
		return 1;
	}

	memset(&info, 0, sizeof(info));
	memset(&info_attr, 0, sizeof(info_attr));
	info_attr.info.bpf_fd = fd;
	info_attr.info.info_len = sizeof(info);
	info_attr.info.info = (uint64_t)(uintptr_t)&info;

	if (sys_bpf(BPF_OBJ_GET_INFO_BY_FD, &info_attr, sizeof(info_attr)) < 0) {
		printf("BPF_JIT_SMOKE: FAIL BPF_OBJ_GET_INFO_BY_FD errno=%d\n",
		       errno);
		close(fd);
		return 1;
	}

	if (!info.xlated_prog_len || !info.jited_prog_len) {
		printf("BPF_JIT_SMOKE: FAIL id=%u xlated_len=%u jited_len=%u\n",
		       info.id, info.xlated_prog_len, info.jited_prog_len);
		close(fd);
		return 1;
	}

	printf("BPF_JIT_SMOKE: PASS bpf_jit_enable=%d id=%u xlated_len=%u jited_len=%u\n",
	       sysctl, info.id, info.xlated_prog_len, info.jited_prog_len);
	close(fd);
	return 0;
}
