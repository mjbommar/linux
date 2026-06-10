# SPDX-License-Identifier: GPL-2.0
#
# Host-only selftest for the atomic stack-pivot primitive used by
# UML pool-member work. Validates the compiler and host kernel before
# the primitive is wired into UML.

CFLAGS := -O0 -static -Wall -Wextra
TEST_GEN_PROGS := rt_sigreturn_test
TEST_PROGS := run-rt-sigreturn-isolation.sh

include ../../lib.mk

$(OUTPUT)/rt_sigreturn_test: rt_sigreturn_test.c
	$(CC) $(CFLAGS) -o $@ $<
