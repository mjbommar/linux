# SPDX-License-Identifier: GPL-2.0
#
# KVM mm-mutation smoke. Validates that post-syscall shadow refill is
# only needed for mm-mutating syscalls, and that those paths flow
# through callbacks that invalidate the shadow page tables. Covers
# mmap, munmap, mprotect, mremap, and brk.

TEST_PROGS := run-kvm-mm-smoke.sh
TEST_GEN_FILES := mm-smoke-loop
EXTRA_CLEAN := mm-smoke-loop

include ../../lib.mk

$(OUTPUT)/mm-smoke-loop: mm-smoke-loop.c
	$(CC) -Wall -Wextra -O2 -static -nostdlib -ffreestanding \
	      -fno-asynchronous-unwind-tables -fno-stack-protector \
	      -o $@ $<
