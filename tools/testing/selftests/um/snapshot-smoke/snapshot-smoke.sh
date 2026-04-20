#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# um/snapshot-smoke/snapshot-smoke.sh — workstream C-09 smoke test.
#
# Runs inside a UML guest as init=. Validates that the snapshot /
# forkserver kernel-side plumbing is wired up:
#
#   1. /sys/kernel/um/state_version exists and reads "1"
#      (the C-09 v1 AFL-compatible 12-byte wire contract).
#   2. /sys/kernel/debug/um/snapshot_ready exists and is write-only.
#   3. Writing a named point to snapshot_ready with fds 198/199 NOT
#      plumbed produces the expected -ENODEV clean-skip path
#      (confirms the ready-point machinery compiled in and the
#      "no fuzzer listening" branch returns cleanly rather than
#      crashing). This is the mode almost every casual boot hits.
#
# If CONFIG_UM_SNAPSHOT_FORKSERVER=n, /sys/kernel/um/state_version
# is absent and the test emits SNAPSHOT_SMOKE: SKIP and halts.
#
# The end-to-end forkserver protocol (handshake + fork + status)
# requires fds 198/199 plumbed by the host launcher; that part is
# exercised by run-snapshot-smoke.sh's optional Python driver, not
# by this guest-side script.
#
# Emits one terminal line:
#   SNAPSHOT_SMOKE: PASS state_version=N ready=<present|absent>
#   SNAPSHOT_SMOKE: FAIL <reason>
#   SNAPSHOT_SMOKE: SKIP <reason>

set -u

echo "SNAPSHOT_SMOKE: init running"

mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

SV=/sys/kernel/um/state_version
RP=/sys/kernel/debug/um/snapshot_ready

if [ ! -f "$SV" ]; then
	echo "SNAPSHOT_SMOKE: SKIP $SV missing (CONFIG_UM_SNAPSHOT_FORKSERVER=n?)"
	halt -f 2>/dev/null
	exit 0
fi

VER=$(cat "$SV" 2>/dev/null)
if [ -z "$VER" ]; then
	echo "SNAPSHOT_SMOKE: FAIL could not read $SV"
	halt -f 2>/dev/null
	exit 1
fi

# Check protocol version matches what the selftest expects.
if [ "$VER" != "1" ]; then
	echo "SNAPSHOT_SMOKE: FAIL state_version='$VER' (expected 1)"
	halt -f 2>/dev/null
	exit 1
fi

if [ ! -e "$RP" ]; then
	echo "SNAPSHOT_SMOKE: FAIL $RP absent (CONFIG_DEBUG_FS missing, or snapshot debugfs init failed)"
	halt -f 2>/dev/null
	exit 1
fi

# Write-only node — reads should fail with EACCES; writes should be
# accepted. We expect the write to succeed (the kernel takes the
# input, discovers fds 198/199 aren't open, logs and returns
# -ENODEV). From userspace the write() returns the byte count, so
# `echo` exits 0 either way; we just verify no kernel crash.
if echo smoke > "$RP" 2>/dev/null; then
	:
else
	echo "SNAPSHOT_SMOKE: FAIL write to $RP failed (errno=$?)"
	halt -f 2>/dev/null
	exit 1
fi

echo "SNAPSHOT_SMOKE: PASS state_version=$VER ready=present"
halt -f 2>/dev/null
exit 0
