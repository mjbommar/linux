#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Guest-side init script for the snapshot-elf-roundtrip selftest (#181).
#
# Mounts /proc + /sys + debugfs, asks the kernel to dump an ELF64-core
# snapshot to a host-visible path, then prints recognisable markers
# the host wrapper greps for. The host runs the actual readelf / gdb
# validation since UML guests usually don't have those binaries.
#
# Emits one of:
#   SNAPSHOT_ELF_GUEST: PASS path=<path> bytes=<n>
#   SNAPSHOT_ELF_GUEST: SKIP <reason>
#   SNAPSHOT_ELF_GUEST: FAIL <reason>

set -u

echo "SNAPSHOT_ELF_GUEST: init running"

mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

TRIGGER=/sys/kernel/debug/um/kvm_v2_snapshot_elf_export_path

if [ ! -e "$TRIGGER" ]; then
	echo "SNAPSHOT_ELF_GUEST: SKIP $TRIGGER absent (CONFIG_DEBUG_FS off, or kvm-v2 init never ran)"
	halt -f 2>/dev/null
	exit 0
fi

# Pick a guest-visible destination. The selftest passes a host path
# via /umsnap-elf-dest if hostfs is mounted at /; otherwise we fall
# back to a guest tmpfs path (the test then runs in-guest validation
# only).
DEST=${UMSNAP_DEST:-/tmp/snap-dump.elf}
mkdir -p "$(dirname "$DEST")" 2>/dev/null || true

if ! echo "$DEST" > "$TRIGGER" 2>/dev/null; then
	echo "SNAPSHOT_ELF_GUEST: FAIL write to $TRIGGER failed (errno=$?)"
	halt -f 2>/dev/null
	exit 1
fi

if [ ! -s "$DEST" ]; then
	echo "SNAPSHOT_ELF_GUEST: FAIL $DEST is empty or missing"
	ls -l "$DEST" 2>&1 || true
	halt -f 2>/dev/null
	exit 1
fi

SIZE=$(wc -c < "$DEST" 2>/dev/null)

# Quick sanity check: ELF magic is "\x7fELF" so the first 4 bytes are
# 7f 45 4c 46.
HEAD=$(od -An -tx1 -N4 "$DEST" 2>/dev/null | tr -d ' \n')
case "$HEAD" in
	7f454c46)
		echo "SNAPSHOT_ELF_GUEST: PASS path=$DEST bytes=$SIZE magic=ELF"
		;;
	*)
		echo "SNAPSHOT_ELF_GUEST: FAIL ELF magic missing (got 0x$HEAD)"
		halt -f 2>/dev/null
		exit 1
		;;
esac

halt -f 2>/dev/null
exit 0
