# SPDX-License-Identifier: GPL-2.0
TARGETS = cpython-parity cpython-tier0 cve-repro df-preserve dyn-loader ftrace-smoke hooks-flip kmsan-smoke kprobes-stress kvm-bounds kvm-mm-smoke kvm-record-smoke kvm-smoke kvm-snapshot-bench launcher-smoke perf-fallback perf-getpid perf-pidfam pool-spawn-smoke profiles regrtest-repros snapshot-kvm-smoke snapshot-smoke template-pause-smoke umlctl-smoke userspace-smoke vector2-sandbox-audit
include ../lib.mk

all:
	@for TARGET in $(TARGETS); do		\
		$(MAKE) -C $$TARGET;		\
	done;

clean:
	@for TARGET in $(TARGETS); do		\
		$(MAKE) -C $$TARGET clean;	\
	done;
