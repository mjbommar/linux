# SPDX-License-Identifier: GPL-2.0
#
# um/pool-mconsole-path-probe kselftest.
#
# Probes the per-member mconsole socket path carried in the template-pause
# identity blob.  This is a diagnostic gate for daemon-routed exec: the socket
# must exist and answer existing mconsole commands before an exec primitive can
# be layered on top.

TEST_PROGS := run-pool-mconsole-path-probe.sh

include ../../lib.mk
