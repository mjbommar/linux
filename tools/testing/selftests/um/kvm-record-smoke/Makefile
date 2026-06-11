# SPDX-License-Identifier: GPL-2.0
TEST_PROGS := run-kvm-record-smoke.sh
TEST_FILES := kvm-record-smoke.sh
TEST_GEN_FILES := kvm-record-task kvm-record-negative \
	kvm-record-negative-openat kvm-record-negative-read \
	kvm-record-negative-write kvm-record-negative-ioctl \
	kvm-record-mismatch kvm-record-time kvm-record-rdtsc \
	kvm-record-rdtscp kvm-record-signal
EXTRA_CLEAN := kvm-record-task kvm-record-negative \
	kvm-record-negative-openat kvm-record-negative-read \
	kvm-record-negative-write kvm-record-negative-ioctl \
	kvm-record-mismatch kvm-record-time kvm-record-rdtsc \
	kvm-record-rdtscp kvm-record-signal

include ../../lib.mk

$(OUTPUT)/kvm-record-task: kvm-record-task.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

$(OUTPUT)/kvm-record-negative: kvm-record-negative.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

$(OUTPUT)/kvm-record-negative-openat: kvm-record-negative.c
	$(CC) -Wall -Wextra -O2 -static -DKVM_RECORD_NEGATIVE_OPENAT -o $@ $<

$(OUTPUT)/kvm-record-negative-read: kvm-record-negative.c
	$(CC) -Wall -Wextra -O2 -static -DKVM_RECORD_NEGATIVE_READ -o $@ $<

$(OUTPUT)/kvm-record-negative-write: kvm-record-negative.c
	$(CC) -Wall -Wextra -O2 -static -DKVM_RECORD_NEGATIVE_WRITE -o $@ $<

$(OUTPUT)/kvm-record-negative-ioctl: kvm-record-negative.c
	$(CC) -Wall -Wextra -O2 -static -DKVM_RECORD_NEGATIVE_IOCTL -o $@ $<

$(OUTPUT)/kvm-record-mismatch: kvm-record-mismatch.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

$(OUTPUT)/kvm-record-time: kvm-record-time.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

$(OUTPUT)/kvm-record-rdtsc: kvm-record-timestamp.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<

$(OUTPUT)/kvm-record-rdtscp: kvm-record-timestamp.c
	$(CC) -Wall -Wextra -O2 -static -DKVM_RECORD_TIMESTAMP_RDTSCP -o $@ $<

$(OUTPUT)/kvm-record-signal: kvm-record-signal.c
	$(CC) -Wall -Wextra -O2 -static -o $@ $<
