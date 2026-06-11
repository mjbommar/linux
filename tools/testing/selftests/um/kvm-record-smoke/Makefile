# SPDX-License-Identifier: GPL-2.0
TEST_PROGS := run-kvm-record-smoke.sh
TEST_FILES := kvm-record-smoke.sh
TEST_GEN_FILES := kvm-record-task
EXTRA_CLEAN := kvm-record-task

include ../../lib.mk

$(OUTPUT)/kvm-record-task: kvm-record-task.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<
