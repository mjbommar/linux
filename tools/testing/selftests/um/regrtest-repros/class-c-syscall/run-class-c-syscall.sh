#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Per-class runner for Class C (real syscall/ioctl gap) reproducers.
# Invoked from the top-level run-regrtest-repros.sh inside the UML guest.
DIR="$(dirname "$0")"

BINS="
ioctl_fionread
ioctl_tiocgwinsz_socketpair
ioctl_blkgetsize_loop
socket_options_dgram
socket_unix_abstract
socket_udplite
os_waitid_edges
os_sched_getcpu
os_setblocking
sanity_struct_unicode
cross_process_futex
scm_rights_fdpass
"

for b in $BINS; do
	if [ -x "$DIR/$b" ]; then
		"$DIR/$b" 2>&1
	else
		echo "REPRO: $b EXPECTED_FAIL not_built"
	fi
done

exit 0
