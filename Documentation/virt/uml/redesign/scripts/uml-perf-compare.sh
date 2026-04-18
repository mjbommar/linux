#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# uml-perf-compare.sh — A-07 regression checker.
#
# Runs uml-perf.sh, compares cycle counts (p50) against the
# committed baseline, applies the bot policy:
#
#   regression  <2%   pass silently
#   regression  2-5%  warn (CI: reviewer ack required)
#   regression  >5%   fail (CI: blocks merge)
#
# Improvements (negative regressions) always pass; >5% improvement
# also flagged as a candidate for baseline bump.
#
# Per A-07 invariant I2: prod-fast can't regress >5%.

set -u

SRC=$(cd "$(dirname "$0")/../../../../.." && pwd)
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BASELINE="$SRC/Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/perf-baseline.json"

WARN_PCT=${UML_PERF_WARN:-2}
FAIL_PCT=${UML_PERF_FAIL:-5}

if [ ! -f "$BASELINE" ]; then
	echo "No baseline at $BASELINE."
	echo "Run uml-perf-capture.sh and commit the result first."
	exit 2
fi

# Run the current measurement.
TMP=$(mktemp)
trap "rm -f $TMP" EXIT
"$SCRIPT_DIR/uml-perf.sh" > "$TMP"

# Extract per-backend cycle p50 from the baseline.
extract_p50() {
	local file=$1 backend=$2
	# Match the line for this backend, then pull the cycles.p50 number.
	grep -E "\"backend\":\"$backend\"" "$file" | head -1 | \
		sed -E 's/.*"cycles":\{[^}]*"p50":([0-9]+).*/\1/'
}

WORST_PCT=0
WORST_BACKEND="none"
FAIL=0
WARN=0

echo "===== uml-perf-compare ====="
printf "%-15s %-15s %-15s %-10s %s\n" backend baseline current regression status

for backend in PTRACE_ONLY SECCOMP_ONLY DYN_ptrace DYN_seccomp; do
	base=$(extract_p50 "$BASELINE" "$backend")
	cur=$(extract_p50 "$TMP" "$backend")
	[ -z "$base" ] && base=0
	[ -z "$cur" ] && cur=0

	if [ "$base" -eq 0 ] || [ "$cur" -eq 0 ]; then
		printf "%-15s %-15s %-15s %-10s %s\n" \
			"$backend" "$base" "$cur" "n/a" "skip"
		continue
	fi

	# Percent regression: (cur - base) * 100 / base
	# Use python for fractional math.
	pct=$(python3 -c "print(f'{(($cur - $base) * 100.0 / $base):+.2f}')")
	# Strip sign for absolute compare.
	abs_pct=$(echo "$pct" | tr -d '+-')
	abs_int=${abs_pct%.*}

	status="ok"
	if [[ "$pct" == -* ]]; then
		# Improvement
		if [ "$abs_int" -ge "$FAIL_PCT" ]; then
			status="improvement (consider baseline bump)"
		fi
	else
		if [ "$abs_int" -ge "$FAIL_PCT" ]; then
			status="FAIL (>${FAIL_PCT}% regression)"
			FAIL=$((FAIL + 1))
		elif [ "$abs_int" -ge "$WARN_PCT" ]; then
			status="WARN (${WARN_PCT}-${FAIL_PCT}% regression; ack required)"
			WARN=$((WARN + 1))
		fi
	fi

	printf "%-15s %-15s %-15s %-10s %s\n" \
		"$backend" "$base" "$cur" "$pct%" "$status"
done

echo
if [ "$FAIL" -gt 0 ]; then
	echo "FAIL: $FAIL backend(s) regressed >${FAIL_PCT}% (invariant I2)"
	exit 1
elif [ "$WARN" -gt 0 ]; then
	echo "WARN: $WARN backend(s) regressed ${WARN_PCT}-${FAIL_PCT}% (reviewer ack)"
	exit 0
else
	echo "OK: no regressions ≥${WARN_PCT}%"
fi
