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
BASELINE_DIR="$SRC/Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction"

# Per-host baseline file naming (task #94 resolution). A single
# committed baseline doesn't work across dev hosts — cycle counts
# aren't portable across Xeon generations (cross-host confusion
# documented in D47/D49). Slugify the CPU model string to derive
# a stable filename so each host owns its own baseline, and git
# blame records who captured what on which machine.
#
# Slug: lowercase, strip vendor noise, collapse non-alphanumeric
# to `-`, trim and truncate. Produces e.g.
#   "Intel(R) Xeon(R) W-2123 CPU @ 3.60GHz" → "xeon-w-2123"
#   "Intel(R) Xeon(R) CPU E3-1225 v6 @ 3.30GHz" → "xeon-e3-1225-v6"
baseline_slug_for_cpu() {
	local model=$1
	printf '%s\n' "$model" \
		| tr '[:upper:]' '[:lower:]' \
		| sed -E 's/\(r\)|\(tm\)//g' \
		| sed -E 's/@ [0-9.]+[gm]hz//g' \
		| sed -E 's/\b(intel|amd|cpu)\b//g' \
		| sed -E 's/[^a-z0-9]+/-/g' \
		| sed -E 's/^-+|-+$//g' \
		| cut -c1-40
}

# Refuse to bake a non-performance governor into the committed
# baseline. Without this gate, a future baseline capture on a
# laptop with `powersave` default would silently record ~3–4×
# inflated cycle counts and every subsequent comparison would
# either FAIL (correct binaries appear regressed) or mask real
# regressions (if baseline and current both happened to be
# powersave). The governor goes into the baseline JSON so
# uml-perf-compare.sh can validate current-vs-baseline match.
PERF_CPU=${UML_PERF_CPU:-0}
CPUFREQ_GOV=$(cat "/sys/devices/system/cpu/cpu$PERF_CPU/cpufreq/scaling_governor" \
	2>/dev/null || echo unknown)
if [ "$CPUFREQ_GOV" != "performance" ] && \
   [ "${UML_PERF_FORCE:-0}" != "1" ]; then
	echo >&2 "uml-perf-capture: cpu$PERF_CPU governor is '$CPUFREQ_GOV'."
	echo >&2 "  Committed baselines must be captured on 'performance'."
	echo >&2 "  Fix:   sudo cpupower -c $PERF_CPU frequency-set -g performance"
	echo >&2 "  Force: UML_PERF_FORCE=1 $0 (ONLY for testing; don't commit)"
	exit 2
fi

# Host context.
HOST_CPU=$(awk -F: '/model name/ {print $2; exit}' /proc/cpuinfo | sed 's/^ *//')
HOST_NCPU=$(nproc)
HOST_GCC=$(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion)
HOST_GLIBC=$(ldd --version 2>&1 | head -1 | awk '{print $NF}')
HOST_KERNEL=$(uname -r)
DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)
TREE_HEAD=$(cd "$SRC" && git rev-parse --short HEAD 2>/dev/null || echo unknown)

# Per-host filename — capture/overwrite the baseline for THIS host.
HOST_SLUG=$(baseline_slug_for_cpu "$HOST_CPU")
OUT="$BASELINE_DIR/perf-baseline-$HOST_SLUG.json"

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
	printf '    "kernel": "%s",\n' "$HOST_KERNEL"
	printf '    "perf_cpu": %s,\n' "$PERF_CPU"
	printf '    "cpufreq_governor": "%s"\n' "$CPUFREQ_GOV"
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
