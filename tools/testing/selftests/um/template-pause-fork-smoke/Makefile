# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-fork-smoke — Memo 09 Phase 2a Patch 5.
#
# Verifies the fork-on-resume primitive's *structural* progress
# (teardown happens, fork(2) returns the expected pid in both
# halves, parent writes child pid to memfd[260:264]).  Marked
# EXPERIMENTAL and exits SKIP — not PASS — on the documented
# secondary "UML_LONGJMP jmp_buf" v1-ceiling hazard.

TEST_PROGS := run-template-pause-fork-smoke.sh

include ../../lib.mk
