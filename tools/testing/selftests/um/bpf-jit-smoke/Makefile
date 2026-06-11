# SPDX-License-Identifier: GPL-2.0
TEST_PROGS := run-bpf-jit-smoke.sh
TEST_GEN_PROGS := bpf-jit-smoke
TEST_FILES := bpf-jit-smoke.sh
EXTRA_CLEAN := bpf-jit-smoke

include ../../lib.mk

$(OUTPUT)/bpf-jit-smoke: bpf-jit-smoke.c
	$(CC) $(CFLAGS) -O2 -Wall -Wextra -o $@ $<
