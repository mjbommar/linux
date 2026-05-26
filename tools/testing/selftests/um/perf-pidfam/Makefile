# SPDX-License-Identifier: GPL-2.0
#
# Mixed pid-family gadget perf loop (review-01 P2 #12).

TEST_PROGS := run-perf-pidfam.sh
TEST_GEN_FILES := pidfam-loop
EXTRA_CLEAN := pidfam-loop

include ../../lib.mk

$(OUTPUT)/pidfam-loop: pidfam-loop.c
	$(CC) -Wall -Wextra -O2 -static -nostdlib -ffreestanding \
	      -fno-asynchronous-unwind-tables -fno-stack-protector \
	      -o $@ $<
