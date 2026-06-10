#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side roundtrip for KVM v2 snapshot ELF export.
#
# The test boots UML with the snapshot ELF boot-export hook and validates the
# resulting ELF core with readelf and, when available, gdb plus the UML snapshot
# helper. The boot hook uses the same host-file exporter as mconsole and avoids
# depending on guest userspace before the snapshot file exists.

set -u

BINARY=${UML_BINARY:-./linux}
MEM=${UML_MEM:-256M}
GDB=${GDB:-gdb}
READELF=${READELF:-readelf}
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../../../../.." && pwd)
HELPER_PY="$REPO_ROOT/tools/uml/uml-gdb/uml-snapshot.py"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -e /dev/kvm ]; then
	echo "SKIP: /dev/kvm not present" >&2
	exit 4
fi
if ! command -v "$READELF" >/dev/null 2>&1; then
	echo "SKIP: readelf not installed" >&2
	exit 4
fi

ensure_kvm_readable() {
	local i

	for i in 1 2 3; do
		if [ -r /dev/kvm ]; then
			return 0
		fi
		sudo -n setfacl -m u:"$(id -un)":rw /dev/kvm 2>/dev/null || true
		[ -r /dev/kvm ] && return 0
		sleep 0.1
	done
	return 1
}

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable" >&2
	exit 4
fi

WORK=$(mktemp -d -t snapshot-elf-roundtrip.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
DUMP="$WORK/snapshot.elf"

OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	kvm_v2_snapshot_elf_export="$DUMP" \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm-v2" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm-v2)" >&2
	echo "$OUT" | grep -E 'backend = |SNAPSHOT_ELF_GUEST|snapshot|UML: fatal|panic' | head -20
	exit 4
fi

if ! echo "$OUT" | grep -q 'um: kvm-v2 snapshot elf: boot export complete'; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (boot export did not complete)"
	echo "$OUT" | grep -E 'backend = |snapshot elf|snapshot:|panic|failed' | head -30
	exit 1
fi

if [ ! -s "$DUMP" ]; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (host-visible dump missing or empty: $DUMP)"
	exit 1
fi

if ! "$READELF" -h "$DUMP" >"$WORK/readelf-h.out" 2>&1; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (readelf -h rejected dump)"
	cat "$WORK/readelf-h.out"
	exit 1
fi
if ! grep -q 'Type:.*CORE' "$WORK/readelf-h.out"; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (ELF type is not CORE)"
	grep 'Type:' "$WORK/readelf-h.out" || true
	exit 1
fi
if ! grep -q 'Class:.*ELF64' "$WORK/readelf-h.out"; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (ELF class is not ELF64)"
	grep 'Class:' "$WORK/readelf-h.out" || true
	exit 1
fi

if ! "$READELF" -l "$DUMP" >"$WORK/readelf-l.out" 2>&1; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (readelf -l rejected dump)"
	tail -20 "$WORK/readelf-l.out"
	exit 1
fi
PT_NOTE_COUNT=$(grep -c 'NOTE' "$WORK/readelf-l.out" || true)
if [ "${PT_NOTE_COUNT:-0}" -lt 1 ]; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (no PT_NOTE program header)"
	cat "$WORK/readelf-l.out"
	exit 1
fi

if ! "$READELF" -n "$DUMP" >"$WORK/readelf-n.out" 2>&1; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (readelf -n rejected notes)"
	tail -20 "$WORK/readelf-n.out"
	exit 1
fi
if ! grep -q 'NT_PRSTATUS' "$WORK/readelf-n.out"; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (NT_PRSTATUS note missing)"
	tail -20 "$WORK/readelf-n.out"
	exit 1
fi

GDB_LINE="skipped"
if command -v "$GDB" >/dev/null 2>&1; then
	GDB_CMD=("$GDB" -batch -nx)
	if [ -f "$HELPER_PY" ]; then
		GDB_CMD+=(-ex "source $HELPER_PY")
	fi
	GDB_CMD+=(-ex "info registers" -c "$DUMP")
	if ! "${GDB_CMD[@]}" >"$WORK/gdb.out" 2>&1; then
		echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (gdb rejected dump)"
		tail -20 "$WORK/gdb.out"
		exit 1
	fi
	if ! grep -q '^rip' "$WORK/gdb.out"; then
		echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL (gdb did not show rip)"
		head -20 "$WORK/gdb.out"
		exit 1
	fi
	GDB_LINE="ok"
fi

SIZE=$(wc -c < "$DUMP" 2>/dev/null)
echo "SNAPSHOT_ELF_ROUNDTRIP: PASS bytes=$SIZE pt_note=$PT_NOTE_COUNT readelf=ok gdb=$GDB_LINE"
exit 0
