# SPDX-License-Identifier: GPL-2.0
TEST_PROGS := run-kcov-smoke.sh
TEST_GEN_PROGS := kcov-smoke
TEST_FILES := kcov-smoke.sh
EXTRA_CLEAN := kcov-smoke

include ../../lib.mk

$(OUTPUT)/kcov-smoke: kcov-smoke.c
	$(CC) $(CFLAGS) -O2 -Wall -Wextra -o $@ $<
