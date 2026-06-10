# SPDX-License-Identifier: GPL-2.0
#
# um/pool-serve-smoke kselftest.
#
# Verifies `umlctl pool serve` boots a fork-mode master, accepts a
# `take` over its Unix socket, returns a live child pid, destroys it,
# and shuts down cleanly.  SKIPs when the fork-capable UML kernel is
# absent (no $UM_FORK_KERNEL).

TEST_PROGS := run-pool-serve-smoke.sh

include ../../lib.mk
