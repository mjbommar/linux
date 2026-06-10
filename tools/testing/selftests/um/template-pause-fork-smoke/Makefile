# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-fork-smoke kselftest.
#
# Verifies the fork-on-resume primitive's structural progress
# (teardown happens, fork(2) returns the expected pid in both
# halves, parent writes child pid to memfd[260:264]).  Marked
# EXPERIMENTAL and exits SKIP, not PASS, on the documented
# secondary UML_LONGJMP jmp_buf hazard.

TEST_PROGS := run-template-pause-fork-smoke.sh

include ../../lib.mk
