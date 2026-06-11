# SPDX-License-Identifier: GPL-2.0
TEST_PROGS := run-kvm-record-smoke.sh
TEST_FILES := kvm-record-smoke.sh
TEST_GEN_FILES := kvm-record-task kvm-record-negative kvm-record-mismatch \
	kvm-record-time kvm-record-rdtsc
EXTRA_CLEAN := kvm-record-task kvm-record-negative kvm-record-mismatch \
	kvm-record-time kvm-record-rdtsc

include ../../lib.mk

$(OUTPUT)/kvm-record-task: kvm-record-task.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

$(OUTPUT)/kvm-record-negative: kvm-record-negative.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

$(OUTPUT)/kvm-record-mismatch: kvm-record-mismatch.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

$(OUTPUT)/kvm-record-time: kvm-record-time.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

$(OUTPUT)/kvm-record-rdtsc: kvm-record-rdtsc.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<
