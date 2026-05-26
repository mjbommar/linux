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
BASELINE_DIR="$SRC/Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction"

WARN_PCT=${UML_PERF_WARN:-2}
FAIL_PCT=${UML_PERF_FAIL:-5}

# Per-host baseline file selection (task #94 resolution). Cycle
# counts aren't portable across Xeon generations; each host owns
# its own baseline at perf-baseline-<cpu-slug>.json. Slug the
# current host's CPU model to find the matching file. Keep this
# slug function in sync with uml-perf-capture.sh::baseline_slug_for_cpu.
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

HOST_CPU=$(awk -F: '/model name/ {print $2; exit}' /proc/cpuinfo | sed 's/^ *//')
HOST_SLUG=$(baseline_slug_for_cpu "$HOST_CPU")
BASELINE="$BASELINE_DIR/perf-baseline-$HOST_SLUG.json"

# Fallback: if no per-host baseline exists, check for the legacy
# un-slugged file (pre-task-#94) and warn. That file stays only
# for migration; once every host has its own, the legacy file
# should be deleted from tree.
if [ ! -f "$BASELINE" ] && [ -f "$BASELINE_DIR/perf-baseline.json" ]; then
	echo >&2 "uml-perf-compare: no per-host baseline for this CPU"
	echo >&2 "  ($HOST_CPU, slug: $HOST_SLUG) at"
	echo >&2 "    $BASELINE"
	echo >&2 "  Legacy un-slugged baseline exists at"
	echo >&2 "    $BASELINE_DIR/perf-baseline.json"
	echo >&2 "  but that's from a different host and comparing it is"
	echo >&2 "  meaningless (see D49). Capture a baseline for this host:"
	echo >&2 "    $SCRIPT_DIR/uml-perf-capture.sh"
	echo >&2 "    git add $BASELINE && git commit"
	exit 2
fi

if [ ! -f "$BASELINE" ]; then
	echo "No baseline for $HOST_CPU at $BASELINE."
	echo "Run $SCRIPT_DIR/uml-perf-capture.sh to create one."
	exit 2
fi

# Read what the baseline was actually captured on. Baselines
# pre-dating governor recording emit "unknown"; treat that as
# "we don't know, refuse to compare until a fresh gated capture
# replaces it." That's the right default — the previous
# hardcoded "assume performance" was an unverified assertion
# that cost a real investigation's worth of time.
PERF_CPU=${UML_PERF_CPU:-0}
BASELINE_GOV=$(sed -nE 's/.*"cpufreq_governor"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p' \
	"$BASELINE" | head -1)
BASELINE_CPU=$(sed -nE 's/.*"perf_cpu"[[:space:]]*:[[:space:]]*([0-9]+).*/\1/p' \
	"$BASELINE" | head -1)
: "${BASELINE_GOV:=unknown}"
: "${BASELINE_CPU:=0}"

CURRENT_GOV=$(cat "/sys/devices/system/cpu/cpu$PERF_CPU/cpufreq/scaling_governor" \
	2>/dev/null || echo unknown)

if [ "${UML_PERF_FORCE:-0}" != "1" ]; then
	if [ "$BASELINE_GOV" = "unknown" ]; then
		echo >&2 "uml-perf-compare: baseline at $BASELINE"
		echo >&2 "  has no cpufreq_governor field (pre-governor-capture baseline)."
		echo >&2 "  Without knowing what the baseline was captured on, any"
		echo >&2 "  comparison is meaningless. Capture a fresh baseline:"
		echo >&2 "    sudo cpupower -c $PERF_CPU frequency-set -g performance"
		echo >&2 "    $SCRIPT_DIR/uml-perf-capture.sh"
		echo >&2 "    git add $BASELINE && git commit"
		echo >&2 "  Or force comparison anyway (debug only):"
		echo >&2 "    UML_PERF_FORCE=1 $0"
		exit 2
	fi
	if [ "$CURRENT_GOV" != "$BASELINE_GOV" ]; then
		echo >&2 "uml-perf-compare: governor mismatch."
		echo >&2 "  baseline: captured on cpu$BASELINE_CPU with governor=$BASELINE_GOV"
		echo >&2 "  current:  cpu$PERF_CPU has governor=$CURRENT_GOV"
		echo >&2 "  Comparing across different governors produces"
		echo >&2 "  noise-dominated \"regressions\". Match the baseline:"
		echo >&2 "    sudo cpupower -c $PERF_CPU frequency-set -g $BASELINE_GOV"
		echo >&2 "  Or override (debug only):"
		echo >&2 "    UML_PERF_FORCE=1 $0"
		exit 2
	fi
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
echo "# Per-backend absolute p50 cycles (same-host-only signal):"
printf "%-15s %-15s %-15s %-10s %s\n" backend baseline current regression status

declare -A BASE_P50 CUR_P50

for backend in PTRACE_ONLY SECCOMP_ONLY DYN_ptrace DYN_seccomp; do
	base=$(extract_p50 "$BASELINE" "$backend")
	cur=$(extract_p50 "$TMP" "$backend")
	[ -z "$base" ] && base=0
	[ -z "$cur" ] && cur=0
	BASE_P50[$backend]=$base
	CUR_P50[$backend]=$cur

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

# Cross-host-stable signal: the RATIO between seccomp and ptrace
# backends. Per invariant I2 the interesting question is "does
# seccomp retain its advantage over ptrace", not "are the absolute
# cycle counts identical to some reference host." Ratios cancel
# out per-CPU IPC/cache/microcode differences that dominate
# absolute cycle counts across Xeon generations (the cross-host
# confusion documented in D47/D49).
echo
echo "# Backend ratios (cross-host-stable, matches invariant I2):"
printf "%-25s %-15s %-15s %-10s %s\n" "ratio" baseline current delta status

compare_ratio() {
	local label=$1 num_backend=$2 denom_backend=$3
	local bn=${BASE_P50[$num_backend]} bd=${BASE_P50[$denom_backend]}
	local cn=${CUR_P50[$num_backend]}  cd=${CUR_P50[$denom_backend]}

	if [ "$bn" -eq 0 ] || [ "$bd" -eq 0 ] || \
	   [ "$cn" -eq 0 ] || [ "$cd" -eq 0 ]; then
		printf "%-25s %-15s %-15s %-10s %s\n" \
			"$label" "n/a" "n/a" "n/a" "skip"
		return
	fi

	# baseline ratio, current ratio, and percent-change-of-ratio:
	local base_ratio cur_ratio ratio_delta_pct
	base_ratio=$(python3 -c "print(f'{$bn / $bd:.3f}')")
	cur_ratio=$(python3 -c "print(f'{$cn / $cd:.3f}')")
	ratio_delta_pct=$(python3 -c "print(f'{(($cn/$cd - $bn/$bd) * 100.0 / ($bn/$bd)):+.2f}')")

	# Ratio regression: seccomp-to-ptrace going UP means seccomp
	# got slower (relative to ptrace) — that's the I2 violation.
	# Going DOWN means seccomp kept or improved its advantage.
	local abs_int status="ok"
	abs_int=$(echo "$ratio_delta_pct" | tr -d '+-')
	abs_int=${abs_int%.*}
	if [[ "$ratio_delta_pct" == +* ]]; then
		if [ "$abs_int" -ge "$FAIL_PCT" ]; then
			status="FAIL (ratio regressed >${FAIL_PCT}%, I2)"
			FAIL=$((FAIL + 1))
		elif [ "$abs_int" -ge "$WARN_PCT" ]; then
			status="WARN (ratio regressed ${WARN_PCT}-${FAIL_PCT}%)"
			WARN=$((WARN + 1))
		fi
	elif [[ "$ratio_delta_pct" == -* ]] && \
	     [ "$abs_int" -ge "$FAIL_PCT" ]; then
		status="improvement (ratio tightened)"
	fi

	printf "%-25s %-15s %-15s %-10s %s\n" \
		"$label" "$base_ratio" "$cur_ratio" "$ratio_delta_pct%" "$status"
}

compare_ratio "SECCOMP / PTRACE"       SECCOMP_ONLY PTRACE_ONLY
compare_ratio "DYN_seccomp / DYN_ptrace" DYN_seccomp DYN_ptrace

echo
if [ "$FAIL" -gt 0 ]; then
	echo "FAIL: $FAIL metric(s) regressed >${FAIL_PCT}% (invariant I2)"
	exit 1
elif [ "$WARN" -gt 0 ]; then
	echo "WARN: $WARN metric(s) regressed ${WARN_PCT}-${FAIL_PCT}% (reviewer ack)"
	exit 0
else
	echo "OK: no regressions ≥${WARN_PCT}% across absolute p50 or backend ratios"
fi
