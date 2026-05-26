#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# uml-gate-bench.sh — run the B-05 Layer 2 gate microbenchmark and
# emit a JSON line with the results.
#
# Boots a UML kernel (default: /tmp/uml-matrix-dynamic/linux) with an
# init script that mounts debugfs, cats /sys/kernel/debug/um/bench,
# and halts. Parses the output into the bench-baseline.json shape.
#
# The compared-against reference is
# Documentation/virt/uml/redesign/02-workstreams/B-static-key-hot-paths/
# notes/bench-baseline.json. Use uml-gate-bench-compare.sh to diff.
#
# Requires: /tmp/uml-matrix-dynamic/linux built with CONFIG_DEBUG_FS=y
# (the DYNAMIC matrix config). Run uml-boot-matrix.sh first, then
# `scripts/config --file /tmp/uml-matrix-dynamic/.config --enable
# DEBUG_FS && make ARCH=um O=/tmp/uml-matrix-dynamic olddefconfig && ...`
# if you're starting from a fresh checkout.

set -u

BINARY=${UML_BENCH_BINARY:-/tmp/uml-matrix-dynamic/linux}
INIT_SCRIPT=$(mktemp /tmp/uml-bench-init.XXXXXX)
trap 'rm -f "$INIT_SCRIPT"' EXIT

if [ ! -x "$BINARY" ]; then
	echo "error: UML binary $BINARY not built. Run uml-boot-matrix.sh first." >&2
	exit 1
fi

cat > "$INIT_SCRIPT" <<'EOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo BENCH_BEGIN
cat /sys/kernel/debug/um/bench 2>&1
echo BENCH_END
halt -f
EOF
chmod +x "$INIT_SCRIPT"

OUT=$(timeout 30 "$BINARY" init="$INIT_SCRIPT" mem=128M \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw 2>&1 |
	awk '/^BENCH_BEGIN/{flag=1; next} /^BENCH_END/{flag=0} flag')

if [ -z "$OUT" ]; then
	echo "error: benchmark produced no output (kernel boot failed?)" >&2
	exit 1
fi

# Parse: lines starting with "# " are comments; data lines are
# "<name>   <off>   <on>".

echo "{"
echo '  "format": "b05-v1",'
echo "  \"date\": \"$(date -u +%Y-%m-%d)\","
echo '  "results_ns_per_call_x1000": {'

first=1
while read -r name off on rest; do
	case "$name" in
	'#'*|'') continue ;;
	esac
	[ -z "$on" ] && continue
	if [ "$first" -eq 0 ]; then
		echo ','
	fi
	printf '    "%s": { "off": %s, "on": %s }' "$name" "$off" "$on"
	first=0
done <<EOF2
$OUT
EOF2

echo ""
echo "  }"
echo "}"
