# SPDX-License-Identifier: GPL-2.0
TARGETS = cve-repro ftrace-smoke hooks-flip kmsan-smoke kprobes-stress launcher-smoke profiles snapshot-smoke userspace-smoke
include ../lib.mk

all:
	@for TARGET in $(TARGETS); do		\
		$(MAKE) -C $$TARGET;		\
	done;

clean:
	@for TARGET in $(TARGETS); do		\
		$(MAKE) -C $$TARGET clean;	\
	done;
