# SPDX-License-Identifier: GPL-2.0
#
# D-06 getpid() bookend (memo 08 sub-commit #6). Builds a
# freestanding 64-bit ELF binary that measures the cost of a
# single getpid() round-trip from guest userspace; the host
# runner boots it as init= under each backend and compares
# the cycle counts.

TEST_PROGS := run-perf-getpid.sh
TEST_GEN_FILES := getpid-loop
EXTRA_CLEAN := getpid-loop

include ../../lib.mk

$(OUTPUT)/getpid-loop: getpid-loop.c
	$(CC) -Wall -Wextra -O2 -static -nostdlib -ffreestanding \
	      -fno-asynchronous-unwind-tables -fno-stack-protector \
	      -o $@ $<
