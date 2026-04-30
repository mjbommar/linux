# SPDX-License-Identifier: GPL-2.0
TEST_GEN_PROGS_EXTENDED := fork_tree_3level
include ../../lib.mk
override CFLAGS := -O0 -static
$(OUTPUT)/fork_tree_3level: fork_tree_3level.c
	$(CC) $(CFLAGS) -o $@ $<
