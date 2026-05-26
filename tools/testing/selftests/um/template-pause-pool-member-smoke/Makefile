# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-pool-member-smoke — Memo 09 §2 end-to-end.
#
# Verifies the pool-member entry path: M-fork child runs
# child_entry_pool_member on the Path A private stack, drops into
# userspace(), and init.sh continues past the /proc write trigger,
# performs multiple sleep+echo cycles, and reaches MEMBER_DONE.

TEST_PROGS := run-template-pause-pool-member-smoke.sh

include ../../lib.mk
