# SPDX-License-Identifier: GPL-2.0
#
# um/net-bench builds helpers for operator-run vector/vector2 throughput gates.
# The benchmark scripts require a UML kernel plus TAP setup, so they are
# installed but not run by the default kselftest harness.

TEST_GEN_PROGS := tcp-send
TEST_FILES := exec-uml-fd.py run-tcp-throughput.sh \
	      run-tcp-throughput-via-umlctl.sh tcp-throughput.toml.template
EXTRA_CLEAN := tcp-send

include ../../lib.mk

$(OUTPUT)/tcp-send: tcp-send.c
	$(Q)mkdir -p "$(OUTPUT)"
	$(CC) $(CFLAGS) -O2 -Wall -Wextra -o $@ $<
