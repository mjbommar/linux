# SPDX-License-Identifier: GPL-2.0
#
# um/kvm-record-clock-bench kselftest — memo 04 §"Acceptance criteria"
# integration gate (post-2026-05-19 sprint).  Closes HONEST-AUDIT §1's
# "integration test missing" caveat: KUnit covered the observe/consume
# surface, this bench covers the runtime wiring (static-key-gated
# um_on_clock_read → __um_record_event_clock → observe_time_travel +
# the symmetric replay-side consume path).
#
# Runner is a shell script; nothing to compile here.

TEST_PROGS := run-kvm-record-clock-bench.sh

include ../../lib.mk
