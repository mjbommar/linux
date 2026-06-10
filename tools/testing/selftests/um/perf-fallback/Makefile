# SPDX-License-Identifier: GPL-2.0
#
# Fallback-path syscall round-trip bookend. Sibling
# to perf-getpid that uses __NR_getsid (Class A passthrough,
# always VMEXITs even under the gadget kernel) so the
# measurement reflects the real cost of the fallback path
# rather than the gadget fast path.

TEST_PROGS := run-perf-fallback.sh
TEST_GEN_FILES := fallback-loop
EXTRA_CLEAN := fallback-loop

include ../../lib.mk

$(OUTPUT)/fallback-loop: fallback-loop.c
	$(CC) -Wall -Wextra -O2 -static -nostdlib -ffreestanding \
	      -fno-asynchronous-unwind-tables -fno-stack-protector \
	      -o $@ $<
