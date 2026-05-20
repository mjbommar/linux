# SPDX-License-Identifier: GPL-2.0
#
# um/pool-spawn-smoke kselftest — Memo 09 Phase 1b.
#
# Verifies `umlctl pool spawn` drives the kernel template-pause
# primitive end-to-end: boots a UML, observes SIGSTOP, writes the
# identity blob, sends SIGCONT, returns JSON describing the running
# pool member.

TEST_PROGS := run-pool-spawn-smoke.sh

include ../../lib.mk
