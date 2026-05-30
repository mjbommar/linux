# SPDX-License-Identifier: GPL-2.0
#
# Build the freestanding APERF/MPERF demo as a static-nostdlib init
# binary suitable for `init=` under UML with hostfs root.  Same flag
# set as tools/testing/selftests/um/kvm-mm-smoke/Makefile.
#
# Usage:
#     make                # build aperf-mperf-demo
#     make clean          # remove the binary
#     make run-on         # boot kvm-v2 UML with passthrough on
#     make run-off        # boot kvm-v2 UML with passthrough off
#     make capture        # rebuild + write both run-output-*.log files
#
# Environment:
#     CC          C compiler (default: cc)
#     UML_BINARY  Path to a UML kernel built with
#                 CONFIG_UM_BACKEND_KVM_V2=y +
#                 CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y +
#                 CONFIG_DEBUG_FS=y.
#                 (default: $(HOME)/src/uml-builds/uml-aperf-test/linux)

CC      ?= cc
UML_BINARY ?= $(HOME)/src/uml-builds/uml-aperf-test/linux

CFLAGS  := -Wall -Wextra -O2 -static -nostdlib -ffreestanding \
           -fno-asynchronous-unwind-tables -fno-stack-protector

DEMO    := aperf-mperf-demo
OUT_ON  := run-output-on.log
OUT_OFF := run-output-off.log

# Common UML cmdline: hostfs root, console redirected to host stdio,
# pid-1 panic auto-reboots (panic=-1) so the launcher exits cleanly.
UMLCMD   = $(UML_BINARY) backend=force=kvm-v2 init=$(CURDIR)/$(DEMO) \
           mem=128M con=null con0=fd:0,fd:1 \
           root=/dev/root rootfstype=hostfs rw panic=-1

.PHONY: all clean run-on run-off capture

all: $(DEMO)

$(DEMO): $(DEMO).c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(DEMO) $(OUT_ON) $(OUT_OFF)

run-on: $(DEMO)
	@timeout --kill-after=5 30 $(UMLCMD) kvm_v2_aperfmperf=on </dev/null

run-off: $(DEMO)
	@timeout --kill-after=5 30 $(UMLCMD) kvm_v2_aperfmperf=off </dev/null

capture: $(DEMO)
	@echo "Capturing run-output logs (kernel binary: $(UML_BINARY))"
	@timeout --kill-after=5 30 $(UMLCMD) kvm_v2_aperfmperf=on  </dev/null > $(OUT_ON)  2>&1 || true
	@timeout --kill-after=5 30 $(UMLCMD) kvm_v2_aperfmperf=off </dev/null > $(OUT_OFF) 2>&1 || true
	@echo "Wrote $(OUT_ON) and $(OUT_OFF)"
	@grep "APERF_MPERF_DEMO\|MSR passthrough" $(OUT_ON)  || true
	@grep "APERF_MPERF_DEMO\|MSR passthrough" $(OUT_OFF) || true
