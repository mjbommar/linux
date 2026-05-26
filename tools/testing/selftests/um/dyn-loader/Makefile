# SPDX-License-Identifier: GPL-2.0
#
# Dynamic-loader kselftest (task #270). No binary to build —
# uses the host's /bin/dash via hostfs to exercise ld-linux
# in-guest. Repro for #238 STEP-2 (lazy-fault path doesn't
# service first-instruction-fetch on shared-library pages).

TEST_PROGS := run-dyn-loader.sh

include ../../lib.mk
