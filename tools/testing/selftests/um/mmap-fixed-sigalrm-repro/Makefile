# SPDX-License-Identifier: GPL-2.0
TEST_GEN_PROGS := mmap-fixed-sigalrm-repro
include ../../lib.mk

$(OUTPUT)/mmap-fixed-sigalrm-repro: mmap-fixed-sigalrm-repro.c
	$(CC) $(CFLAGS) -O2 -Wall -o $@ $< -lrt
