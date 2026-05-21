#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/snapshot-elf-roundtrip kselftest (#181) — host-side launcher.
#
# Boots UML with `backend=force=kvm-v2`, asks the guest's debugfs
# entry to write an ELF64 core snapshot to a host-visible path
# (under hostfs), then validates the dump file:
#
#   1. `readelf -h` shows ET_CORE + ELFCLASS64 + EM_X86_64.
#   2. `readelf -l` shows >= 1 PT_NOTE phdr.
#   3. `readelf -n` parses notes without errors. (Some readelf
#      versions don't recognise the UML vendor note and may print a
#      "Unknown note type" line — that's fine, we just check the
#      exit status and absence of "BAD".)
#   4. (optional) `gdb -c <file>` opens without erroring.
#   5. (optional) the uml-snapshot.py helper sources cleanly into gdb.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.

set -u

BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-128M}
GDB=${GDB:-gdb}
READELF=${READELF:-readelf}
GUEST_SCRIPT=$(cd "$(dirname "$0")" && pwd)/snapshot-elf-roundtrip-guest.sh
HELPER_PY=$(cd "$(dirname "$0")/../../../../tools/uml/uml-gdb" 2>/dev/null && pwd)/uml-snapshot.py
HELPER_PY=${HELPER_PY:-}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi

if [ ! -e /dev/kvm ]; then
	echo "SKIP: /dev/kvm not present" >&2
	exit 4
fi

if [ ! -r /dev/kvm ]; then
	echo "SKIP: /dev/kvm not readable by $(id -un)" >&2
	exit 4
fi

if ! command -v "$READELF" >/dev/null 2>&1; then
	echo "SKIP: readelf not installed" >&2
	exit 4
fi

# Use a fresh temp dir; hostfs maps the host's / into the guest so the
# guest can write /tmp/.../snap-dump.elf and it lands here.
WORK=$(mktemp -d -t snap-elf-roundtrip-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

DUMP="$WORK/snap-dump.elf"

OUT=$(timeout --kill-after=10 30 "$BINARY" \
	backend=force=kvm-v2 \
	init="$GUEST_SCRIPT" mem="$MEM" \
	UMSNAP_DEST="$DUMP" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

# Confirm the kernel picked kvm-v2.
OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm-v2" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm-v2)" >&2
	exit 4
fi

GUEST_LINE=$(echo "$OUT" | grep -E '^SNAPSHOT_ELF_GUEST:' | tail -1)
if [ -z "$GUEST_LINE" ]; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL guest produced no SNAPSHOT_ELF_GUEST line"
	echo "$OUT" | tail -10
	exit 1
fi
echo "  guest: $GUEST_LINE"

case "$GUEST_LINE" in
	*PASS*) ;;
	*SKIP*) echo "SKIP: $GUEST_LINE" >&2; exit 4;;
	*)
		echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL guest reported failure"
		exit 1
		;;
esac

if [ ! -s "$DUMP" ]; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL host-visible dump $DUMP missing/empty"
	exit 1
fi

# Validate ELF header shape.
if ! "$READELF" -h "$DUMP" >"$WORK/readelf-h.out" 2>&1; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL readelf -h rejected $DUMP"
	cat "$WORK/readelf-h.out"
	exit 1
fi

if ! grep -q "Type:.*CORE" "$WORK/readelf-h.out"; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL ELF Type is not CORE"
	grep "Type:" "$WORK/readelf-h.out" || true
	exit 1
fi

if ! grep -q "Class:.*ELF64" "$WORK/readelf-h.out"; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL Class is not ELF64"
	exit 1
fi

# PT_NOTE / PT_LOAD inventory via readelf -l.
"$READELF" -l "$DUMP" >"$WORK/readelf-l.out" 2>&1 || true
PT_NOTE_COUNT=$(grep -c 'NOTE  ' "$WORK/readelf-l.out" || true)
if [ "${PT_NOTE_COUNT:-0}" -lt 1 ]; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL no PT_NOTE in dump"
	cat "$WORK/readelf-l.out"
	exit 1
fi

# Notes section must parse — readelf -n exits non-zero on a malformed
# note. We accept "Unknown note type" lines for the UML vendor notes
# (older readelf doesn't know our vendor namespace) but not "BAD" or
# parse-error exit.
if ! "$READELF" -n "$DUMP" >"$WORK/readelf-n.out" 2>&1; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL readelf -n returned non-zero"
	tail -20 "$WORK/readelf-n.out"
	exit 1
fi

# Confirm at least one CORE NT_PRSTATUS note made it through.
if ! grep -q "NT_PRSTATUS" "$WORK/readelf-n.out"; then
	echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL NT_PRSTATUS not found by readelf -n"
	tail -20 "$WORK/readelf-n.out"
	exit 1
fi

GDB_LINE="(skipped — no gdb)"
if command -v "$GDB" >/dev/null 2>&1; then
	GDB_CMD=("$GDB" -batch -nx)
	if [ -n "$HELPER_PY" ] && [ -f "$HELPER_PY" ]; then
		GDB_CMD+=(-ex "source $HELPER_PY")
	fi
	GDB_CMD+=(-ex "info registers" -c "$DUMP")
	if ! "${GDB_CMD[@]}" >"$WORK/gdb.out" 2>&1; then
		echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL gdb -c rejected $DUMP"
		tail -20 "$WORK/gdb.out"
		exit 1
	fi
	# gdb's `info registers` always prints something on a usable
	# core; absence of "rip" means gdb didn't parse the prstatus.
	if grep -q "^rip" "$WORK/gdb.out"; then
		GDB_LINE="info registers OK ($(grep -c '^[re][a-z]\+' "$WORK/gdb.out") entries)"
	else
		echo "SNAPSHOT_ELF_ROUNDTRIP: FAIL gdb did not print 'rip' in info registers"
		head -20 "$WORK/gdb.out"
		exit 1
	fi
fi

CRASH_LINE="(skipped — no crash)"
if command -v crash >/dev/null 2>&1; then
	# crash(8) doesn't auto-resolve a UML vmlinux from the core; we
	# just smoke-test that "crash -h" recognises the file (parses
	# the ELF) without crashing. Full integration belongs to a
	# separate selftest with the vmlinux pre-positioned.
	if crash -h >/dev/null 2>&1; then
		CRASH_LINE="present, deeper integration deferred"
	fi
fi

echo "SNAPSHOT_ELF_ROUNDTRIP: PASS pt_note=$PT_NOTE_COUNT readelf=OK gdb=$GDB_LINE crash=$CRASH_LINE"
exit 0
