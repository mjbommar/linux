#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# uml-perf-capture.sh — A-07 baseline capture.
#
# Runs uml-perf.sh, captures the JSON output to
# Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/
#   perf-baseline.json
# along with host context (CPU, glibc, gcc, kernel, date) so future
# comparisons can detect cross-host noise.
#
# Per A-07 spec: "Baseline updates: explicit, batched, with sign-off."
# This script is the explicit step. Commit the resulting JSON.

set -eu

SRC=$(cd "$(dirname "$0")/../../../../.." && pwd)
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
OUT="$SRC/Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/perf-baseline.json"

# Host context.
HOST_CPU=$(awk -F: '/model name/ {print $2; exit}' /proc/cpuinfo | sed 's/^ *//')
HOST_NCPU=$(nproc)
HOST_GCC=$(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion)
HOST_GLIBC=$(ldd --version 2>&1 | head -1 | awk '{print $NF}')
HOST_KERNEL=$(uname -r)
DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)
TREE_HEAD=$(cd "$SRC" && git rev-parse --short HEAD 2>/dev/null || echo unknown)

# Run benchmarks; capture each backend line.
TMP=$(mktemp)
trap "rm -f $TMP" EXIT
"$SCRIPT_DIR/uml-perf.sh" > "$TMP"

# Build the wrapper JSON.
{
	echo '{'
	printf '  "schema_version": 1,\n'
	printf '  "captured_at": "%s",\n' "$DATE"
	printf '  "tree_head": "%s",\n' "$TREE_HEAD"
	printf '  "host": {\n'
	printf '    "cpu": "%s",\n' "$HOST_CPU"
	printf '    "ncpu": %s,\n' "$HOST_NCPU"
	printf '    "gcc": "%s",\n' "$HOST_GCC"
	printf '    "glibc": "%s",\n' "$HOST_GLIBC"
	printf '    "kernel": "%s"\n' "$HOST_KERNEL"
	printf '  },\n'
	printf '  "backends": [\n'
	# Indent each line by 4 spaces, comma-separate.
	mapfile -t lines < "$TMP"
	for i in "${!lines[@]}"; do
		printf '    %s' "${lines[$i]}"
		if [ "$i" -lt $((${#lines[@]} - 1)) ]; then
			printf ','
		fi
		printf '\n'
	done
	printf '  ]\n'
	echo '}'
} > "$OUT"

echo "Wrote $OUT"
echo
echo "Summary (cycles p50 per backend):"
mapfile -t lines < "$TMP"
for line in "${lines[@]}"; do
	backend=$(echo "$line" | sed -E 's/.*"backend":"([^"]*)".*/\1/')
	p50=$(echo "$line" | sed -E 's/.*"cycles":\{[^}]*"p50":([0-9]+).*/\1/')
	printf "  %-15s %s cycles\n" "$backend" "$p50"
done
echo
echo "Commit this baseline:"
echo "  git add $OUT"
echo "  git commit -m 'um: backend: capture A-07 perf baseline'"
