# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-fork-stress kselftest.
#
# Drives the master through N=100 fork iterations and gates on:
#   * master alive throughout (no panic, no exit)
#   * all child pids distinct (no SKAS aliasing regression)
#   * master RSS drift <= 5% from baseline (no per-iteration leak)
#   * zero orphan stub children after teardown
#   * median per-iteration wall time < 50 ms (perf regression detector)
#   * identity-blob round-trip: every iter's instance_name parsed
#     by the kernel.

TEST_PROGS := run-template-pause-fork-stress.sh
# Meta-test: validates each strict gate fires when its synthetic
# failure mode is present.  Runs without a kernel - needed because
# production runs always pass and a buggy gate would silently mask
# kernel regressions.  Fast (<5 s), no privileges required.
TEST_PROGS += gates-self-test.py

include ../../lib.mk
