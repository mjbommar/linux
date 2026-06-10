# SPDX-License-Identifier: GPL-2.0
#
# um/pool-exec-smoke kselftest.
#
# Verifies `umlctl exec` returns a clean error envelope from the
# daemon when the in-guest exec primitive (uml_mconsole(1) + mconsole
# socket) is unavailable.  This is the contract syzkaller's vm/uml
# shim relies on: ok=false with a diagnostic, not a hang.
#
# SKIPs when the fork-capable UML kernel or umlctl binary is absent.

TEST_PROGS := run-pool-exec-smoke.sh

include ../../lib.mk
