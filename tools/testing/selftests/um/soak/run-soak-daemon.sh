#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# UML daemon-mode soak driver. Sibling to run-pilot.sh.
#
# Long-running, signal-driven, append-only. Cycles through workload
# templates within a wall-clock budget, writing per-iteration
# rows to scoreboard.jsonl + a rolling Wilson-CI summary table to
# summary.md, until budget elapses, an operator sends SIGTERM/SIGINT,
# or a per-(workload, backend) failure-rate threshold trips.
#
# Usage:
#   UML_KERNEL=/path/to/linux \
#     bash tools/testing/selftests/um/soak/run-soak-daemon.sh \
#          --budget-sec 86400 --workers 2 --iters-per-rotation 10
#
# Smoke (60s, 1 workload):
#   UML_KERNEL=... ./run-soak-daemon.sh --budget-sec 60 \
#       --workloads memcheck --iters-per-rotation 2
#
# Backend-isolated run:
#   UML_KERNEL=... ./run-soak-daemon.sh --backends seccomp \
#       --workloads tier3-django-v2,tier3-fastapi-v2
#
# Stop with SIGTERM/SIGINT; daemon finishes the in-flight workload
# phase before exiting (does NOT kill umlctl mid-iteration).
# Force a summary refresh: kill -USR1 $pid.

set -u

SOAK_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SOAK_DIR" && git rev-parse --show-toplevel 2>/dev/null || echo "")"
if [ -z "$REPO_ROOT" ]; then
	REPO_ROOT="$(cd "$SOAK_DIR/../../../../.." && pwd)"
fi
if [ -n "$REPO_ROOT" ] && [ -x "$REPO_ROOT/tools/uml/uml-launcher/target/debug/umlctl" ]; then
	UMLCTL_DEFAULT="$REPO_ROOT/tools/uml/uml-launcher/target/debug/umlctl"
else
	UMLCTL_DEFAULT="umlctl"
fi

# ----------------------------------------------------------------------
# Defaults (CLI flag may override; env var read as middle-priority).
# ----------------------------------------------------------------------
BUDGET_SEC="${SOAK_BUDGET_SEC:-86400}"
WORKERS="${SOAK_WORKERS:-2}"
ITERS="${SOAK_ITERS:-10}"
WORKLOADS_CSV="${SOAK_WORKLOADS:-memcheck,iocheck,stress-ng,cpython-soak,kbuild-tiny}"
BACKENDS_CSV="${SOAK_BACKENDS:-kvm-v2,seccomp}"
OUT="${SOAK_OUT:-}"
FAIL_THRESH_PCT="${SOAK_FAIL_THRESH_PCT:-5}"
FAIL_THRESH_WIN="${SOAK_FAIL_THRESH_WIN:-50}"
CONTINUE_ON_THRESH="${SOAK_CONTINUE_ON_THRESH:-}"
DRY_RUN=
COOLDOWN="${COOLDOWN:-30}"
THERMAL_PAUSE_C="${THERMAL_PAUSE_C:-88}"
THERMAL_RESUME_C="${THERMAL_RESUME_C:-75}"
UML_KERNEL="${UML_KERNEL:-}"
UMLCTL="${UMLCTL:-$UMLCTL_DEFAULT}"
TIER2_UV_PYTHON="${TIER2_UV_PYTHON:-$HOME/.cache/uml-soak-tier2-venv/bin/python}"

# ----------------------------------------------------------------------
# CLI parsing.
# ----------------------------------------------------------------------
while [ $# -gt 0 ]; do
	case "$1" in
		--budget-sec)              BUDGET_SEC=$2; shift 2 ;;
		--workloads)               WORKLOADS_CSV=$2; shift 2 ;;
		--backends)                BACKENDS_CSV=$2; shift 2 ;;
		--workers)                 WORKERS=$2; shift 2 ;;
		--iters-per-rotation)      ITERS=$2; shift 2 ;;
		--out)                     OUT=$2; shift 2 ;;
		--fail-threshold-pct)      FAIL_THRESH_PCT=$2; shift 2 ;;
		--fail-threshold-window)   FAIL_THRESH_WIN=$2; shift 2 ;;
		--continue-on-fail-threshold) CONTINUE_ON_THRESH=1; shift ;;
		--dry-run)                 DRY_RUN=1; shift ;;
		--help|-h)
			sed -n '4,30p' "$0" | sed 's/^# *//'
			exit 0
			;;
		*) echo "unknown flag: $1" >&2; exit 2 ;;
	esac
done

# Tier 3 v2 aliases reuse the base Django/FastAPI templates with a
# different `network.driver` substitution.
tier3_template_workload() {
	case "$1" in
		tier3-django-v2)  echo "tier3-django" ;;
		tier3-fastapi-v2) echo "tier3-fastapi" ;;
		*)                echo "$1" ;;
	esac
}

tier3_network_driver() {
	case "$1" in
		*-v2) echo "vector2" ;;
		*)   echo "vector" ;;
	esac
}

tier3_netdev_name() {
	case "$(tier3_network_driver "$1")" in
		vector2) echo "vec2.0" ;;
		*)       echo "vec0" ;;
	esac
}

tier3_host_mode() {
	case "$(tier3_network_driver "$1")" in
		vector2) echo "inproc" ;;
		*)       echo "legacy-inproc" ;;
	esac
}

tier3_metadata_extra() {
	local workload=$1 backend=$2 driver netdev host_mode
	driver=$(tier3_network_driver "$workload")
	netdev=$(tier3_netdev_name "$workload")
	host_mode=$(tier3_host_mode "$workload")
	printf ',"uml_network_driver":"%s","uml_netdev_name":"%s","uml_vmm_backend":"%s","uml_transport":"tap","uml_queue_count":1,"uml_host_mode":"%s"' \
		"$driver" "$netdev" "$backend" "$host_mode"
}

# ----------------------------------------------------------------------
# Validate inputs.
# ----------------------------------------------------------------------
if [ -z "$UML_KERNEL" ]; then
	echo "ERR: UML_KERNEL must be set (no fallback path)" >&2
	exit 2
fi
if [ ! -x "$UML_KERNEL" ]; then
	echo "ERR: UML_KERNEL=$UML_KERNEL not executable" >&2
	exit 2
fi
if [ ! -x "$UMLCTL" ] && [ -z "$DRY_RUN" ]; then
	echo "ERR: UMLCTL=$UMLCTL not executable" >&2
	exit 2
fi
if ! command -v python3 >/dev/null; then
	echo "ERR: python3 required for Wilson CI computation" >&2
	exit 2
fi

# Default OUT: uml-soak-<ISO> under $PWD if unset.
if [ -z "$OUT" ]; then
	OUT="$PWD/uml-soak-$(date -u +%Y-%m-%dT%H%M%SZ)"
fi
mkdir -p "$OUT/logs" "$OUT/logs/panics" || exit 2

# Resolve workloads CSV -> array; validate each template.
IFS=',' read -ra WORKLOADS <<< "$WORKLOADS_CSV"
IFS=',' read -ra BACKENDS <<< "$BACKENDS_CSV"
for w in "${WORKLOADS[@]}"; do
	tmpl_w=$(tier3_template_workload "$w")
	if [ ! -f "$SOAK_DIR/${tmpl_w}.toml.template" ]; then
		echo "ERR: missing template $SOAK_DIR/${tmpl_w}.toml.template" >&2
		exit 2
	fi
done
for backend in "${BACKENDS[@]}"; do
	case "$backend" in
		kvm-v2|seccomp) ;;
		*)
			echo "ERR: unsupported backend '$backend' (expected kvm-v2 or seccomp)" >&2
			exit 2
			;;
	esac
done

# Per-workload per-iter timeout. Mirrors run-pilot.sh's TIMEOUTS array.
declare -A TIMEOUT_FOR=(
	[memcheck]=90 [iocheck]=120 [stress-ng]=120
	[cpython-soak]=360 [kbuild-tiny]=600
	[tier1-pylibs]=90 [tier2-uv-pylibs]=120
	[django-loopback-none]=180
	[tier3-django]=180 [tier3-fastapi]=180
	[tier3-django-v2]=180 [tier3-fastapi-v2]=180
	[ltp-runner]=3600
)
DEFAULT_TIMEOUT=120

# ----------------------------------------------------------------------
# Run-state metadata.
# ----------------------------------------------------------------------
START_TS=$(date +%s)
SOAK_RUN_ID=$(basename "$OUT")
HOSTNAME_S=$(hostname)
GIT_DIR="$REPO_ROOT"
COMMIT=$(cd "$SOAK_DIR" && git rev-parse --short HEAD 2>/dev/null || echo "?")
BRANCH=$(cd "$SOAK_DIR" && git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")

STOP_REQUESTED=0
THROTTLE_PAUSED_TOTAL=0
THROTTLE_EVENTS=0

# Per-(workload|backend) counters: total iters, total passes.
declare -A WL_N WL_PASS

# Per-(workload|backend) rolling-window arrays (last N verdicts: 1=PASS, 0=FAIL).
# Stored as space-separated string in WL_WINDOW[$key] for portability.
declare -A WL_WINDOW

# ----------------------------------------------------------------------
# Signal handlers.
# ----------------------------------------------------------------------
on_term() {
	echo "[$(date -uIs)] received SIGTERM/SIGINT - will stop after current workload phase"
	STOP_REQUESTED=1
}
trap on_term TERM INT

on_usr1() {
	echo "[$(date -uIs)] received SIGUSR1 - refreshing summary"
	write_summary || true
}
trap on_usr1 USR1

# ----------------------------------------------------------------------
# Helpers (thermal lift from run-pilot.sh; same semantics).
# ----------------------------------------------------------------------
read_max_temp_c() {
	local maxv=0
	for nf in /sys/class/hwmon/hwmon*/name; do
		[ -r "$nf" ] || continue
		local n
		n=$(cat "$nf" 2>/dev/null)
		case "$n" in
			k10temp|coretemp|zenpower|cpu_thermal) ;;
			*) continue ;;
		esac
		local d
		d=$(dirname "$nf")
		for t in "$d"/temp*_input; do
			[ -r "$t" ] || continue
			local v
			v=$(cat "$t" 2>/dev/null || echo 0)
			v=$((v / 1000))
			[ "$v" -gt "$maxv" ] && maxv=$v
		done
	done
	if [ "$maxv" -eq 0 ]; then
		for f in /sys/class/thermal/thermal_zone*/temp; do
			[ -r "$f" ] || continue
			local v
			v=$(cat "$f" 2>/dev/null || echo 0)
			v=$((v / 1000))
			[ "$v" -gt "$maxv" ] && maxv=$v
		done
	fi
	echo "$maxv"
}

thermal_check() {
	local t
	t=$(read_max_temp_c)
	if [ "$t" -ge "$THERMAL_PAUSE_C" ]; then
		local pause_start
		pause_start=$(date +%s)
		echo "[$(date -uIs)] [thermal] $t C >= ${THERMAL_PAUSE_C}C - pausing"
		# Append a thermal scoreboard row.
		emit_scoreboard_row \
			"$SOAK_RUN_ID" "thermal" "thermal" "" 0 0 \
			"PAUSE" "" "false" "false" "false" "$t" "$t" 0 ""
		while [ "$(read_max_temp_c)" -gt "$THERMAL_RESUME_C" ]; do
			sleep 5
		done
		local pause_end
		pause_end=$(date +%s)
		local paused=$((pause_end - pause_start))
		THROTTLE_PAUSED_TOTAL=$((THROTTLE_PAUSED_TOTAL + paused))
		THROTTLE_EVENTS=$((THROTTLE_EVENTS + 1))
		echo "[$(date -uIs)] [thermal] resumed at $(read_max_temp_c)C (paused ${paused}s)"
	fi
}

# Wilson 95% CI - closed-form, no external dep beyond python3 stdlib.
# Echoes "rate_pct lower_pct upper_pct" (3 floats, 2-decimal).
wilson_ci() {
	local n=$1 k=$2
	python3 - "$n" "$k" <<-'PY'
		import math, sys
		n = int(sys.argv[1]); k = int(sys.argv[2])
		if n == 0:
		    print("0.00 0.00 0.00"); sys.exit(0)
		z = 1.959964
		p = k / n
		denom = 1 + z*z/n
		center = (p + z*z/(2*n)) / denom
		half = (z/denom) * math.sqrt(p*(1-p)/n + z*z/(4*n*n))
		print(f"{p*100:.2f} {max(0.0,(center-half)*100):.2f} {min(100.0,(center+half)*100):.2f}")
	PY
}

# JSON-escape a string (minimal - assumes 7-bit input from filenames/paths).
json_esc() { printf '%s' "$1" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'; }

# Append a row to scoreboard.jsonl. Field order matches section2.6 schema.
emit_scoreboard_row() {
	local soak_run_id=$1 gate=$2 workload=$3 backend=$4
	local rotation=$5 iter_within=$6 verdict=$7 init_log=$8
	local panic_b=$9 timeout_b=${10} host_error=${11}
	local t_pre=${12} t_post=${13} dur_ms=${14} extra=${15}
	local ts
	ts=$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)
	local rc=0
	[ "$verdict" = "PASS" ] || rc=1
	local sig=null
	local row
	row=$(printf '{"ts":"%s","gate":"%s","backend":%s,"commit":"%s","branch":"%s","kernel":%s,"host":"%s","soak_run_id":"%s","rotation_idx":%d,"iter_idx_within_workload":%d,"workload":"%s","workers":%d,"verdict":"%s","rc":%d,"signal":%s,"panic":%s,"timeout":%s,"host_error":%s,"max_temp_c_pre":%d,"max_temp_c_post":%d,"duration_ms":%d,"init_log_relpath":%s%s}' \
		"$ts" "$gate" \
		"$([ -n "$backend" ] && echo "\"$backend\"" || echo null)" \
		"$COMMIT" "$BRANCH" "$(json_esc "$UML_KERNEL")" "$HOSTNAME_S" \
		"$soak_run_id" "$rotation" "$iter_within" "$workload" "$WORKERS" \
		"$verdict" "$rc" "$sig" "$panic_b" "$timeout_b" "$host_error" \
		"$t_pre" "$t_post" "$dur_ms" \
		"$([ -n "$init_log" ] && json_esc "$init_log" || echo null)" \
		"$extra")
	echo "$row" >> "$OUT/scoreboard.jsonl"
}

# Update WL_WINDOW (rolling FAIL_THRESH_WIN verdicts) and check threshold.
# Returns 0 if under threshold, 1 if tripped.
update_window_and_check() {
	local key=$1 verdict=$2
	local v
	[ "$verdict" = "PASS" ] && v=1 || v=0
	local cur="${WL_WINDOW[$key]:-}"
	# shellcheck disable=SC2086
	set -- $cur $v
	# Trim to last FAIL_THRESH_WIN
	local n_keep=$FAIL_THRESH_WIN
	while [ $# -gt "$n_keep" ]; do shift; done
	WL_WINDOW[$key]="$*"
	# Compute fail rate over the window.
	local n_in=$# n_pass=0
	local x
	for x in "$@"; do
		[ "$x" = 1 ] && n_pass=$((n_pass + 1))
	done
	if [ "$n_in" -lt "$FAIL_THRESH_WIN" ]; then
		return 0  # window not full yet
	fi
	local n_fail=$((n_in - n_pass))
	local fail_x10000=$((n_fail * 10000 / n_in))
	local thresh_x10000=$((FAIL_THRESH_PCT * 100))
	if [ "$fail_x10000" -gt "$thresh_x10000" ]; then
		return 1
	fi
	return 0
}

write_summary() {
	local summary="$OUT/summary.md"
	local now elapsed pct_consumed
	now=$(date +%s)
	elapsed=$((now - START_TS))
	pct_consumed=$(awk -v e="$elapsed" -v b="$BUDGET_SEC" \
		'BEGIN { printf "%.1f", (e * 100.0) / b }')
	{
		echo "# UML soak summary - $SOAK_RUN_ID"
		echo
		echo "Start:    $(date -u -d "@$START_TS" +%Y-%m-%dT%H:%M:%SZ)"
		echo "Now:      $(date -u +%Y-%m-%dT%H:%M:%SZ)   (${elapsed}s elapsed)"
		echo "Budget:   ${BUDGET_SEC}s (${pct_consumed}% consumed)"
		echo "Stop:     $([ "$STOP_REQUESTED" = 1 ] && echo "requested" || echo "running")"
		echo "Kernel:   $UML_KERNEL"
		echo "Commit:   $COMMIT ($BRANCH)"
		echo
		echo "| workload | backend | n | pass | fail | rate | Wilson 95% CI |"
		echo "|----------|---------|---|------|------|------|---------------|"
		local key wl be
		for key in "${!WL_N[@]}"; do
			wl="${key%%|*}"; be="${key##*|}"
			local n="${WL_N[$key]}" k="${WL_PASS[$key]}"
			local fail=$((n - k))
			local ci_line
			ci_line=$(wilson_ci "$n" "$k")
			# shellcheck disable=SC2086
			set -- $ci_line
			printf "| %s | %s | %d | %d | %d | %s%% | [%s%%, %s%%] |\n" \
				"$wl" "$be" "$n" "$k" "$fail" "$1" "$2" "$3"
		done | sort
		echo
		echo "Throttle pauses: $THROTTLE_EVENTS events, ${THROTTLE_PAUSED_TOTAL}s total"
	} > "$summary.tmp" && mv "$summary.tmp" "$summary"
}

# Walk a workload-phase output dir; for each per-iter run-*.log, classify
# verdict, append scoreboard row, update counters + window.
process_phase_results() {
	local workload=$1 backend=$2 rotation=$3 phase_dir=$4 t_pre=$5 t_post=$6
	local key="${workload}|${backend}"
	local n="${WL_N[$key]:-0}"
	local k="${WL_PASS[$key]:-0}"
	local f log_rel verdict panic_b timeout_b host_error rc dur_ms iter_within
	for f in "$phase_dir"/p0_default/*/run-*.log; do
		[ -f "$f" ] || continue
		log_rel="${f#"$OUT/"}"
		iter_within=$((n + 1))
		verdict=FAIL; panic_b=false; timeout_b=false; host_error=false
		if grep -q "REPRO_DONE rc=0" "$f"; then
			verdict=PASS
		elif grep -q "Kernel panic" "$f"; then
			verdict=PANIC; panic_b=true
		elif grep -q -E "TIMEOUT|deadline exceeded" "$f"; then
			verdict=TIMEOUT; timeout_b=true
		fi
			# umlctl gate loop does not expose per-iteration timing.
			dur_ms=0
		emit_scoreboard_row "$SOAK_RUN_ID" "uml-soak-$workload" \
			"$workload" "$backend" "$rotation" "$iter_within" \
			"$verdict" "$log_rel" \
			"$panic_b" "$timeout_b" "$host_error" \
			"$t_pre" "$t_post" "$dur_ms" ""
		n=$((n + 1))
		[ "$verdict" = "PASS" ] && k=$((k + 1))
		# Update rolling window + check threshold.
		if ! update_window_and_check "$key" "$verdict"; then
			if [ -z "$CONTINUE_ON_THRESH" ]; then
				echo "[$(date -uIs)] THRESHOLD TRIPPED: $key (>$FAIL_THRESH_PCT% over rolling $FAIL_THRESH_WIN)" >&2
				touch "$OUT/THRESHOLD_TRIPPED"
				STOP_REQUESTED=1
			else
				echo "[$(date -uIs)] threshold tripped on $key; --continue-on-fail-threshold set, soaking on" >&2
			fi
		fi
		# Copy panic logs into logs/panics/ for post-mortem ease.
		if [ "$verdict" = "PANIC" ]; then
			local pdir="$OUT/logs/panics/r${rotation}-${workload}-${backend}"
			mkdir -p "$pdir"
			cp "$f" "$pdir/" 2>/dev/null || true
		fi
	done
	WL_N[$key]=$n
	WL_PASS[$key]=$k
}

# ----------------------------------------------------------------------
# Snapshot config.json at start.
# ----------------------------------------------------------------------
write_config() {
	local stress_ng_ver
	stress_ng_ver=$(stress-ng --version 2>/dev/null | head -1 | sed 's/.*version //; s/ .*//' || echo unknown)
	python3 - <<-PY > "$OUT/config.json"
	import json
	cfg = {
	  "soak_run_id": "$SOAK_RUN_ID",
	  "start_ts": "$(date -u -d "@$START_TS" +%Y-%m-%dT%H:%M:%SZ)",
	  "host": "$HOSTNAME_S",
	  "kernel": "$UML_KERNEL",
	  "commit": "$COMMIT",
	  "branch": "$BRANCH",
	  "umlctl": "$UMLCTL",
	  "stress_ng_version": "$stress_ng_ver",
	  "args": {
	    "budget_sec": $BUDGET_SEC,
	    "workers": $WORKERS,
	    "iters_per_rotation": $ITERS,
	    "workloads": $(python3 -c 'import json,sys; print(json.dumps("'$WORKLOADS_CSV'".split(",")))'),
	    "backends": "$BACKENDS_CSV".split(","),
	    "fail_threshold_pct": $FAIL_THRESH_PCT,
	    "fail_threshold_window": $FAIL_THRESH_WIN,
	    "continue_on_fail_threshold": $([ -n "$CONTINUE_ON_THRESH" ] && echo True || echo False),
	    "out": "$OUT"
	  },
	  "env": {
	    "THERMAL_PAUSE_C": $THERMAL_PAUSE_C,
	    "THERMAL_RESUME_C": $THERMAL_RESUME_C,
	    "COOLDOWN": $COOLDOWN
	  }
	}
	print(json.dumps(cfg, indent=2))
	PY
}

# Tier 3 workloads require per-worker IP carve-out from 192.168.42.0/24
# rather than the single shared TAP that `umlctl gate loop --workers N`
# produces.
is_tier3_workload() {
	case "$1" in
		tier3-*) return 0 ;;
		*)       return 1 ;;
	esac
}

# Expand placeholders into a per-worker TOML. Args:
#   template_path worker_idx backend output_path network_driver
# Placeholders set: {{KERNEL}}, {{BACKEND}}, {{WORKER_IDX}},
# {{HOST_IP}} (CIDR), {{GUEST_IP}} (CIDR),
# {{HOST_IP_PLAIN}} (no mask), {{GUEST_IP_PLAIN}} (no mask),
# {{TAP_NAME}}, {{NETWORK_DRIVER}}, {{SOAK_DIR}}, {{REPO_ROOT}}, and
# {{TIER2_UV_PYTHON}}.
emit_tier3_worker_toml() {
	local tmpl=$1 widx=$2 backend=$3 out=$4 network_driver=$5
	local hip="192.168.42.$((4 * widx + 1))"
	local gip="192.168.42.$((4 * widx + 2))"
	local tap="soak-tap${widx}"
	sed -e "s|{{KERNEL}}|$UML_KERNEL|g"           \
	    -e "s|{{BACKEND}}|$backend|g"              \
	    -e "s|{{WORKER_IDX}}|$widx|g"              \
	    -e "s|{{HOST_IP_PLAIN}}|${hip}|g"          \
	    -e "s|{{GUEST_IP_PLAIN}}|${gip}|g"         \
	    -e "s|{{HOST_IP}}|${hip}/30|g"             \
	    -e "s|{{GUEST_IP}}|${gip}/30|g"            \
	    -e "s|{{TAP_NAME}}|${tap}|g"               \
	    -e "s|{{NETWORK_DRIVER}}|${network_driver}|g" \
	    -e "s|{{SOAK_DIR}}|$SOAK_DIR|g"            \
	    -e "s|{{REPO_ROOT}}|$REPO_ROOT|g"          \
	    -e "s|{{TIER2_UV_PYTHON}}|$TIER2_UV_PYTHON|g" \
	    "$tmpl" > "$out"
}

# Walk a tier3 phase output dir (which has $phase_dir/w<N>/p0_default/...
# per-worker subdirs rather than a single p0_default/) and emit per-iter
# scoreboard rows for every worker.
process_tier3_phase_results() {
	local workload=$1 backend=$2 rotation=$3 phase_dir=$4 t_pre=$5 t_post=$6
	local key="${workload}|${backend}"
	local n="${WL_N[$key]:-0}"
	local k="${WL_PASS[$key]:-0}"
	local wdir f log_rel verdict panic_b timeout_b host_error dur_ms iter_within extra
	extra=$(tier3_metadata_extra "$workload" "$backend")
	for wdir in "$phase_dir"/w*; do
		[ -d "$wdir" ] || continue
		for f in "$wdir"/p0_default/*/run-*.log; do
			[ -f "$f" ] || continue
			log_rel="${f#"$OUT/"}"
			iter_within=$((n + 1))
			verdict=FAIL; panic_b=false; timeout_b=false; host_error=false
			if grep -q "REPRO_DONE rc=0" "$f"; then
				verdict=PASS
			elif grep -q "Kernel panic" "$f"; then
				verdict=PANIC; panic_b=true
			elif grep -q -E "TIMEOUT|deadline exceeded" "$f"; then
				verdict=TIMEOUT; timeout_b=true
			fi
			dur_ms=0
			emit_scoreboard_row "$SOAK_RUN_ID" "uml-soak-$workload" \
				"$workload" "$backend" "$rotation" "$iter_within" \
				"$verdict" "$log_rel" \
				"$panic_b" "$timeout_b" "$host_error" \
				"$t_pre" "$t_post" "$dur_ms" "$extra"
			n=$((n + 1))
			[ "$verdict" = "PASS" ] && k=$((k + 1))
			if ! update_window_and_check "$key" "$verdict"; then
				if [ -z "$CONTINUE_ON_THRESH" ]; then
					echo "[$(date -uIs)] THRESHOLD TRIPPED: $key (>$FAIL_THRESH_PCT% over rolling $FAIL_THRESH_WIN)" >&2
					touch "$OUT/THRESHOLD_TRIPPED"
					STOP_REQUESTED=1
				else
					echo "[$(date -uIs)] threshold tripped on $key; --continue-on-fail-threshold set, soaking on" >&2
				fi
			fi
			if [ "$verdict" = "PANIC" ]; then
				local pdir="$OUT/logs/panics/r${rotation}-${workload}-${backend}"
				mkdir -p "$pdir"
				cp "$f" "$pdir/" 2>/dev/null || true
				printf '{%s}\n' "${extra#,}" > "$pdir/metadata.json"
			fi
		done
	done
	WL_N[$key]=$n
	WL_PASS[$key]=$k
}

# Stale-tap cleanup. When a previous tier3 phase panicked the
# guest mid-run, umlctl's tear-down may not have completed and
# `soak-tap{N}` lingers. A subsequent phase's `ip tuntap add dev
# soak-tapN` then trips with `Device or resource busy`. This
# helper sweeps `soak-tap0..soak-tap{$WORKERS-1}` before each
# tier3 phase. Idempotent: deleting a non-existent tap is a no-op.
tier3_cleanup_stale_taps() {
	local w
	for w in $(seq 0 $((WORKERS - 1))); do
		if ip link show "soak-tap${w}" >/dev/null 2>&1; then
			echo "[$(date -uIs)] cleanup: removing stale soak-tap${w}"
			sudo -n ip link delete "soak-tap${w}" 2>/dev/null || true
		fi
	done
}

# Tier 3 phase runner. Spawns $WORKERS parallel single-worker
# `umlctl gate loop --workers 1` invocations, each with a per-worker
# /30 carve-out from 192.168.42.0/24. Each worker has a unique TAP
# (soak-tap0..soak-tap{N-1}) and a unique host_ip/guest_ip pair.
run_one_tier3_phase() {
	local workload=$1 backend=$2 rotation=$3
	local timeout_sec="${TIMEOUT_FOR[$workload]:-$DEFAULT_TIMEOUT}"
	local out_dir="$OUT/_loop/${workload}-${backend}-r${rotation}"
	mkdir -p "$out_dir"

	# Sweep any stale soak-tap{N} left behind by a prior phase
	# that crashed before umlctl's tear-down completed.
	tier3_cleanup_stale_taps

	thermal_check
	local t_pre t_post
	t_pre=$(read_max_temp_c)

	local phase_t0 phase_t1
	phase_t0=$(date +%s)

	echo "[$(date -uIs)] phase: workload=$workload backend=$backend r=$rotation W=$WORKERS (tier3 per-worker fanout) M=$ITERS timeout=${timeout_sec}s temp=${t_pre}C"

	local pids=() w toml w_out template_workload network_driver
	template_workload=$(tier3_template_workload "$workload")
	network_driver=$(tier3_network_driver "$workload")
	for w in $(seq 0 $((WORKERS - 1))); do
		toml="$OUT/_${workload}-${backend}-w${w}.toml"
		emit_tier3_worker_toml \
			"$SOAK_DIR/${template_workload}.toml.template" \
			"$w" "$backend" "$toml" "$network_driver"
		w_out="$out_dir/w${w}"
		mkdir -p "$w_out"
		if [ -n "$DRY_RUN" ]; then
			echo "  [dry-run] worker $w: $UMLCTL gate loop -f $toml -W 1 -M $ITERS --timeout $timeout_sec --out $w_out (host=192.168.42.$((4*w+1)) guest=192.168.42.$((4*w+2)) tap=soak-tap${w} driver=${network_driver})"
			mkdir -p "$w_out/p0_default/w0"
			echo "REPRO_DONE rc=0" > "$w_out/p0_default/w0/run-0.log"
		else
			"$UMLCTL" gate loop -f "$toml" -W 1 -M "$ITERS" \
				--timeout "$timeout_sec" --out "$w_out" \
				>"$w_out/_loop.log" 2>&1 &
			pids+=($!)
		fi
	done

	# Wait for all per-worker invocations. We do not propagate
	# child exit codes - verdicts come from per-iter log scraping.
	local pid
	for pid in "${pids[@]}"; do
		wait "$pid" 2>/dev/null || true
	done

	phase_t1=$(date +%s)
	t_post=$(read_max_temp_c)
	echo "  phase elapsed=$((phase_t1 - phase_t0))s temp_post=${t_post}C"

	process_tier3_phase_results \
		"$workload" "$backend" "$rotation" \
		"$out_dir" "$t_pre" "$t_post"
	sleep "$COOLDOWN"
}

run_one_phase() {
	local workload=$1 backend=$2 rotation=$3

	if is_tier3_workload "$workload"; then
		run_one_tier3_phase "$workload" "$backend" "$rotation"
		return
	fi

	local timeout_sec="${TIMEOUT_FOR[$workload]:-$DEFAULT_TIMEOUT}"
	local toml="$OUT/_${workload}-${backend}.toml"
	sed -e "s|{{KERNEL}}|$UML_KERNEL|g" -e "s|{{BACKEND}}|$backend|g" \
		-e "s|{{SOAK_DIR}}|$SOAK_DIR|g" \
		-e "s|{{REPO_ROOT}}|$REPO_ROOT|g" \
		-e "s|{{TIER2_UV_PYTHON}}|$TIER2_UV_PYTHON|g" \
		"$SOAK_DIR/${workload}.toml.template" > "$toml"
	local out_dir="$OUT/_loop/${workload}-${backend}-r${rotation}"
	mkdir -p "$out_dir"

	thermal_check
	local t_pre t_post
	t_pre=$(read_max_temp_c)

	local phase_t0 phase_t1
	phase_t0=$(date +%s)

	echo "[$(date -uIs)] phase: workload=$workload backend=$backend r=$rotation W=$WORKERS M=$ITERS timeout=${timeout_sec}s temp=${t_pre}C"

	if [ -n "$DRY_RUN" ]; then
		echo "  [dry-run] would run: $UMLCTL gate loop -f $toml -W $WORKERS -M $ITERS --timeout $timeout_sec --out $out_dir"
		# Synthesize a fake all-pass result for testing.
		mkdir -p "$out_dir/p0_default/w0"
		echo "REPRO_DONE rc=0" > "$out_dir/p0_default/w0/run-0.log"
	else
		"$UMLCTL" gate loop -f "$toml" -W "$WORKERS" -M "$ITERS" \
			--timeout "$timeout_sec" --out "$out_dir" \
			>"$out_dir/_loop.log" 2>&1 || true
	fi

	phase_t1=$(date +%s)
	t_post=$(read_max_temp_c)
	echo "  phase elapsed=$((phase_t1 - phase_t0))s temp_post=${t_post}C"

	process_phase_results "$workload" "$backend" "$rotation" "$out_dir" "$t_pre" "$t_post"
	sleep "$COOLDOWN"
}

# ----------------------------------------------------------------------
# Main loop.
# ----------------------------------------------------------------------
write_config
echo "[$(date -uIs)] uml-soak START run_id=$SOAK_RUN_ID budget=${BUDGET_SEC}s"
echo "[$(date -uIs)] workloads=${WORKLOADS_CSV} backends=${BACKENDS_CSV}"
echo "[$(date -uIs)] workers=$WORKERS iters=$ITERS"
echo "[$(date -uIs)] out_dir=$OUT"

ROTATION=0
LAST_SUMMARY_TS=$(date +%s)
SUMMARY_INTERVAL=3600  # rewrite summary every hour or every rotation, whichever first

while [ "$STOP_REQUESTED" = 0 ]; do
	NOW=$(date +%s)
	ELAPSED=$((NOW - START_TS))
	if [ "$ELAPSED" -ge "$BUDGET_SEC" ]; then
		echo "[$(date -uIs)] budget elapsed (${ELAPSED}s >= ${BUDGET_SEC}s); stopping"
		break
	fi

	for workload in "${WORKLOADS[@]}"; do
		[ "$STOP_REQUESTED" = 0 ] || break
		for backend in "${BACKENDS[@]}"; do
			[ "$STOP_REQUESTED" = 0 ] || break
			run_one_phase "$workload" "$backend" "$ROTATION"
		done
	done

	# Rotation summary write.
	write_summary || true
	NOW2=$(date +%s)
	if [ $((NOW2 - LAST_SUMMARY_TS)) -ge "$SUMMARY_INTERVAL" ]; then
		LAST_SUMMARY_TS=$NOW2
	fi
	ROTATION=$((ROTATION + 1))
done

# ----------------------------------------------------------------------
# Final summary + clean exit.
# ----------------------------------------------------------------------
write_summary
ELAPSED=$(($(date +%s) - START_TS))
echo "[$(date -uIs)] uml-soak STOP run_id=$SOAK_RUN_ID rotations=$ROTATION elapsed=${ELAPSED}s"
echo "[$(date -uIs)] artefacts at $OUT"
if [ -f "$OUT/THRESHOLD_TRIPPED" ]; then
	echo "[$(date -uIs)] failure-rate threshold tripped; see $OUT/scoreboard.jsonl" >&2
fi
exit 0
