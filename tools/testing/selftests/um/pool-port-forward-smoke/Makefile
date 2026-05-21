# SPDX-License-Identifier: GPL-2.0
#
# um/pool-port-forward-smoke kselftest — Memo 09 Phase 4.
#
# Verifies `umlctl port-forward --json` returns the well-typed
# tap-direct envelope for a daemon-taken pool member.  The verb is
# pure metadata lookup; the in-guest service binding is NOT exercised
# (host kernel cannot reach guest userspace until the kernel-side tap
# fd swap lands).

TEST_PROGS := run-pool-port-forward-smoke.sh

include ../../lib.mk
