# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-smoke kselftest — Memo 09 Phase 1a.
#
# Verifies the in-kernel template-pause primitive end-to-end:
#   * unarmed boot: /proc/um/template_pause must not exist
#   * armed boot, no identity fd: SIGSTOP self → SIGCONT resume
#   * armed boot, identity fd populated: blob parsed, fields logged

TEST_PROGS := run-template-pause-smoke.sh

include ../../lib.mk
