# SPDX-License-Identifier: GPL-2.0
#
# tools/uml/Makefile — aggregate subdirectory targets for the
# UML host-side tool tree.
#
# Today the only sub-tool is uml-launcher (workstream C-10 v1).
# As v2 lands (per-device vhost-user helpers, RPC control
# surface, etc.) new sub-tools drop into SUBDIRS and pick up
# the all / check / test / clean / install / help fan-out for
# free.
#
# Usage:
#   make -C tools/uml             # build everything that has a Makefile
#   make -C tools/uml check       # clippy etc.
#   make -C tools/uml test        # cargo test etc.
#   make -C tools/uml install     # install each tool's binary
#
# Per-sub-tool Makefiles are free to no-op if their build
# toolchain (e.g. cargo) isn't present; matches the optional-
# tool pattern used across tools/.

SUBDIRS := uml-launcher

.PHONY: all check test clean install help $(SUBDIRS)

all: $(addsuffix .all, $(SUBDIRS))
check: $(addsuffix .check, $(SUBDIRS))
test: $(addsuffix .test, $(SUBDIRS))
clean: $(addsuffix .clean, $(SUBDIRS))
install: $(addsuffix .install, $(SUBDIRS))

%.all:
	$(MAKE) -C $(basename $@) all

%.check:
	$(MAKE) -C $(basename $@) check

%.test:
	$(MAKE) -C $(basename $@) test

%.clean:
	$(MAKE) -C $(basename $@) clean

%.install:
	$(MAKE) -C $(basename $@) install

help:
	@echo "tools/uml/ targets:"
	@echo "  all        build every sub-tool"
	@echo "  check      run per-sub-tool linters (clippy, etc.)"
	@echo "  test       run per-sub-tool tests"
	@echo "  clean      remove build artefacts"
	@echo "  install    install each sub-tool's binary"
	@echo ""
	@echo "Sub-tools: $(SUBDIRS)"
	@echo ""
	@echo "Per-tool help:"
	@for d in $(SUBDIRS); do \
		echo "  make -C tools/uml/$$d help"; \
	done
