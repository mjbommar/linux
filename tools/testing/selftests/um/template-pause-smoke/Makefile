# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-smoke kselftest — Memo 09 Phase 1a + Phase 2.
#
# Verifies the in-kernel template-pause primitive end-to-end:
#   * case 1 — unarmed boot: /proc/um/template_pause must not exist
#   * case 2 — armed boot, no identity fd: SIGSTOP self → SIGCONT resume
#   * case 3 — armed boot, identity fd populated: blob parsed +
#              identity-apply path invoked (-ENODEV if no netdev)
#   * case 4 — armed boot WITH vec0 netdev: MAC + IPv4 actually
#              applied to vec0, verified via in-guest `ip addr show`

TEST_PROGS := run-template-pause-smoke.sh

include ../../lib.mk
