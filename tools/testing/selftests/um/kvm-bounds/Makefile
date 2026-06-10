# SPDX-License-Identifier: GPL-2.0
#
# KVM gadget user-pointer bounds-check selftest. Validates that the
# gadget's TASK_SIZE_CAP check returns -EFAULT for canonical kernel VAs
# and non-canonical pointers across clock_gettime / time / getcpu.

TEST_PROGS := run-kvm-bounds.sh
TEST_GEN_FILES := kvm-bounds-loop
EXTRA_CLEAN := kvm-bounds-loop

include ../../lib.mk

$(OUTPUT)/kvm-bounds-loop: kvm-bounds-loop.c
	$(CC) -Wall -Wextra -O2 -static -nostdlib -ffreestanding \
	      -fno-asynchronous-unwind-tables -fno-stack-protector \
	      -o $@ $<
