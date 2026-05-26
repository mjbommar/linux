# SPDX-License-Identifier: GPL-2.0
#
# Path A isolation test — see Documentation/virt/uml/redesign/
# 06-sequencing/post-2026-05-21-three-test-paths.md §1.
#
# Host-only selftest.  Validates that the atomic stack-pivot
# primitive (alternative to rt_sigreturn) works on this host's
# compiler + kernel before any UML porting.

CFLAGS := -O0 -static -Wall -Wextra
TEST_GEN_PROGS := rt_sigreturn_test
TEST_PROGS := run-rt-sigreturn-isolation.sh

include ../../lib.mk

$(OUTPUT)/rt_sigreturn_test: rt_sigreturn_test.c
	$(CC) $(CFLAGS) -o $@ $<
