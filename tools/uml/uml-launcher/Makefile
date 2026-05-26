# SPDX-License-Identifier: GPL-2.0
#
# Makefile for tools/uml/uml-launcher/.
#
# Optional-tool pattern: if cargo isn't on the host, every target
# is a polite no-op with an explanatory message. Matches the
# tools/bpf/bpftool detection pattern for optional feature-gated
# userspace binaries.

CARGO ?= cargo
PREFIX ?= /usr/local
DESTDIR ?=

CARGO_PRESENT := $(shell command -v $(CARGO) 2>/dev/null)

.PHONY: all check test clean install help

all:
ifneq ($(CARGO_PRESENT),)
	$(CARGO) build --release
else
	@echo "uml-launcher: cargo not found in PATH; skipping build."
	@echo "Install a Rust toolchain (rustup or distro package) and re-run."
endif

check:
ifneq ($(CARGO_PRESENT),)
	$(CARGO) clippy --all-targets -- -D warnings
else
	@echo "uml-launcher: cargo not found; skipping clippy."
endif

test:
ifneq ($(CARGO_PRESENT),)
	$(CARGO) test
else
	@echo "uml-launcher: cargo not found; skipping tests."
endif

clean:
ifneq ($(CARGO_PRESENT),)
	$(CARGO) clean
else
	@echo "uml-launcher: cargo not found; nothing to clean."
endif

install: all
ifneq ($(CARGO_PRESENT),)
	install -D -m 0755 target/release/uml-launcher \
		$(DESTDIR)$(PREFIX)/bin/uml-launcher
	install -D -m 0755 target/release/umlctl \
		$(DESTDIR)$(PREFIX)/bin/umlctl
	install -D -m 0755 target/release/umlbuild \
		$(DESTDIR)$(PREFIX)/bin/umlbuild
	@echo "Installed uml-launcher, umlctl, umlbuild to $(DESTDIR)$(PREFIX)/bin/"
else
	@echo "uml-launcher: cargo not found; nothing to install."
endif

help:
	@echo "uml-launcher Makefile targets:"
	@echo "  all       build release binary (default)"
	@echo "  check     run cargo clippy -- -D warnings"
	@echo "  test      run cargo test"
	@echo "  clean     cargo clean"
	@echo "  install   install to \$$PREFIX/bin (default /usr/local/bin)"
	@echo ""
	@echo "Environment:"
	@echo "  CARGO=/path/to/cargo   override cargo binary"
	@echo "  PREFIX=/opt/uml        install prefix"
	@echo "  DESTDIR=/tmp/stage     stage install (packaging)"
