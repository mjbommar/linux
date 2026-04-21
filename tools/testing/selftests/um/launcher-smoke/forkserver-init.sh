#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# launcher-smoke Part C guest init: mount /proc, /sys, debugfs,
# then trigger the C-09 forkserver by writing to
# /sys/kernel/debug/um/snapshot_ready. um_snapshot_ready() reads
# from fd 198, writes to fd 199, forks a worker, reaps it, and
# returns once the driver closes the ctl pipe.
#
# Only meaningful on a kernel built with
# CONFIG_UM_SNAPSHOT_FORKSERVER=y (fuzz / fuzz-deep profiles).

mount -t proc     none /proc             2>/dev/null
mount -t sysfs    none /sys              2>/dev/null
mount -t debugfs  none /sys/kernel/debug 2>/dev/null

TRIG=/sys/kernel/debug/um/snapshot_ready
if [ ! -e "$TRIG" ]; then
	# No snapshot debugfs node → kernel lacks the Kconfig.
	# Exit; the driver's read on fd 199 will see EOF and
	# report FAIL handshake.
	halt -f
	exit 1
fi

echo "launcher-smoke-part-c" > "$TRIG"

halt -f
