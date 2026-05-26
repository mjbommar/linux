# SPDX-License-Identifier: GPL-2.0
TEST_PROGS := run-regrtest-repros.sh

SUBDIRS := class-a-env class-b-process class-c-syscall class-d-structural

all:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d || exit 1; done

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean || true; done

include ../../lib.mk
