# SPDX-License-Identifier: GPL-2.0
#
# Direction-flag preservation selftest (audit round-4 F3
# follow-on, task #222 / decisions-log D87 cross-ref).
# Builds a freestanding 64-bit ELF that validates F2's claim
# that user RFLAGS (specifically DF) survive the SYSCALL +
# recoverable #PF round-trips through the KVM backend's
# kvm_build_sysret_r11 helper.

TEST_PROGS := run-df-preserve.sh
TEST_GEN_FILES := df-preserve-loop
EXTRA_CLEAN := df-preserve-loop

include ../../lib.mk

$(OUTPUT)/df-preserve-loop: df-preserve-loop.c
	$(CC) -Wall -Wextra -O2 -static -nostdlib -ffreestanding \
	      -fno-asynchronous-unwind-tables -fno-stack-protector \
	      -o $@ $<
