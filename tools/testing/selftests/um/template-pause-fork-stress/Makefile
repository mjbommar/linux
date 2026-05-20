# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-fork-stress kselftest — Memo 09 Phase 2a stress.
#
# Drives the master through N=100 fork iterations and gates on:
#   * master alive throughout (no panic, no exit)
#   * all child pids distinct (no SKAS aliasing regression)
#   * master RSS drift ≤ 5% from baseline (no per-iteration leak)
#   * zero orphan stub children after teardown
#   * median per-iteration wall time < 50 ms (perf regression detector)
#   * identity-blob round-trip: every iter's instance_name parsed
#     by the kernel.

TEST_PROGS := run-template-pause-fork-stress.sh

include ../../lib.mk
