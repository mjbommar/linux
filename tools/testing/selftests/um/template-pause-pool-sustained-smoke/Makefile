# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-pool-sustained-smoke — Memo 09 §2 N-member.
#
# Drives master through N=3 fork iterations with distinct identity
# blobs and verifies each forked pool member reaches MEMBER_DONE.
#
# Currently exits XFAIL (4 = skip) — iter 1 PASSes, iter 2+ crashes
# due to inherited stub_pid aliasing.  The fix requires a new SKAS
# API for clone-without-CLONE_VM-aliasing.  See
# Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
# state-audit/32-pool-member-entry-wip.md §4 step 19e.

TEST_PROGS := run-template-pause-pool-sustained-smoke.sh

include ../../lib.mk
