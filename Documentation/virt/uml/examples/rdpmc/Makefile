# SPDX-License-Identifier: GPL-2.0
CC      ?= cc
UML_BINARY ?= $(HOME)/src/uml-builds/uml-aperf-test/linux

CFLAGS  := -Wall -Wextra -O2 -static -nostdlib -ffreestanding \
           -fno-asynchronous-unwind-tables -fno-stack-protector

DEMO    := rdpmc-demo

UMLCMD   = $(UML_BINARY) backend=force=kvm-v2 init=$(CURDIR)/$(DEMO) \
           mem=128M con=null con0=fd:0,fd:1 \
           root=/dev/root rootfstype=hostfs rw panic=-1

.PHONY: all clean run

all: $(DEMO)

$(DEMO): $(DEMO).c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(DEMO) run-output.log

run: $(DEMO)
	@setsid -w bash -c 'timeout --kill-after=5 30 $(UMLCMD) </dev/null' 2>&1 | tee run-output.log | grep -E "RDPMC_DEMO|segfault" || true
