# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-pool-sustained-smoke kselftest.
#
# Drives master through N=3 fork iterations with distinct identity
# blobs and verifies each forked pool member reaches MEMBER_DONE.
#
# Currently exits XFAIL (4 = skip): iter 1 passes, iter 2+ crashes
# due to inherited stub_pid aliasing.  The fix requires a new SKAS
# API for clone-without-CLONE_VM-aliasing.

TEST_PROGS := run-template-pause-pool-sustained-smoke.sh

include ../../lib.mk
