# SPDX-License-Identifier: GPL-2.0
#
# um/pool-bench kselftest.
#
# Drives `umlctl pool serve` through the pool performance gates:
#   * take.p50  <= 5 ms   (across 1000 sequential takes)
#   * take.p99  <= 50 ms
#   * rss.100forks <= 200 MiB  (supervisor + master + 100 children
#                               with a 128 MiB master)
#   * lifecycle.drift <= 5 %  (over 10 000 take/destroy cycles)
#   * throughput.60s   >= 2700 takes  (60 s @ 50/sec target = 90 %)
#
# SKIPs cleanly when /dev/kvm is absent, the fork-mode UML kernel
# is missing ($UM_FORK_KERNEL), or umlctl is not built.

TEST_PROGS := run-pool-bench.sh

include ../../lib.mk
