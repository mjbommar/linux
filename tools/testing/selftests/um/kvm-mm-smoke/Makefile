# SPDX-License-Identifier: GPL-2.0
#
# KVM mm-mutation smoke (review-01 P1 #6). Validates that
# experiment #1's invariant (post-syscall shadow refill is safe
# to drop because mm_map/mm_unmap callbacks invalidate the
# shadow PT for mm-mutating syscalls) holds across mmap, munmap,
# mprotect, mremap, brk.

TEST_PROGS := run-kvm-mm-smoke.sh
TEST_GEN_FILES := mm-smoke-loop
EXTRA_CLEAN := mm-smoke-loop

include ../../lib.mk

$(OUTPUT)/mm-smoke-loop: mm-smoke-loop.c
	$(CC) -Wall -Wextra -O2 -static -nostdlib -ffreestanding \
	      -fno-asynchronous-unwind-tables -fno-stack-protector \
	      -o $@ $<
