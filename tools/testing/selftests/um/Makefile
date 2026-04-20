# SPDX-License-Identifier: GPL-2.0
TARGETS = ftrace-smoke hooks-flip kprobes-stress profiles snapshot-smoke
include ../lib.mk

all:
	@for TARGET in $(TARGETS); do		\
		$(MAKE) -C $$TARGET;		\
	done;

clean:
	@for TARGET in $(TARGETS); do		\
		$(MAKE) -C $$TARGET clean;	\
	done;
