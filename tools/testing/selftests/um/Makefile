# SPDX-License-Identifier: GPL-2.0
TARGETS = cve-repro df-preserve ftrace-smoke hooks-flip kmsan-smoke kprobes-stress kvm-bounds kvm-smoke launcher-smoke perf-fallback perf-getpid profiles snapshot-smoke umlctl-smoke userspace-smoke
include ../lib.mk

all:
	@for TARGET in $(TARGETS); do		\
		$(MAKE) -C $$TARGET;		\
	done;

clean:
	@for TARGET in $(TARGETS); do		\
		$(MAKE) -C $$TARGET clean;	\
	done;
