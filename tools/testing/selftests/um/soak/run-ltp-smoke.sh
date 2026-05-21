#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Phase J LTP smoke driver — single-suite probe to confirm that LTP +
# kirk + the skip-list + the host's UML kernel all wire together
# before paying the 45-minute per-cycle cost of the daemon-driven
# full curated run.
#
# Spec: phase-J-ltp-curation-2026-05-14.md §5.3 ("What happens on
# first run") + §6.2 (kirk JSON schema validation).
#
# Pre-flight (see ltp-preflight-checklist memo for the full list):
#   - /opt/ltp populated by `make install` from upstream LTP.
#   - /opt/kirk populated by `git clone linux-test-project/kirk`.
#   - UML kernel binary at $UML_KERNEL (default: post-Phase-A build).
#   - Optional: CONFIG_UML_NET_VECTOR=y (only needed if SUITE pulls
#     in the few syscalls tests that probe AF_NETLINK rt-netlink;
#     defaults below pick suites that don't).
#
# Usage:
#   ./run-ltp-smoke.sh            # default: smoketest suite, host SUT
#   ./run-ltp-smoke.sh syscalls   # single named suite
#   ./run-ltp-smoke.sh smoketest math nptl  # multiple suites
#
# Environment overrides:
#   LTP_ROOT     /opt/ltp                       LTP install prefix
#   KIRK_DIR     /opt/kirk                      kirk checkout
#   SKIP_FILE    <this dir>/ltp-skip.txt        per-test deny-list
#   EXEC_TIMEOUT 120                            per-test wall-clock (s)
#   SUITE_TIMEOUT 600                           per-suite wall-clock (s)
#   OUT_DIR      /tmp/ltp-smoke-<unix-ts>       artefact directory
#
# Exit codes:
#   0   all selected suites PASS (kirk fail count == 0)
#   1   at least one suite FAIL (kirk fail count > 0 or kirk crash)
#   2   pre-flight broke (missing /opt/ltp, missing /opt/kirk, etc.)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

LTP_ROOT="${LTP_ROOT:-/opt/ltp}"
KIRK_DIR="${KIRK_DIR:-/opt/kirk}"
SKIP_FILE="${SKIP_FILE:-$SCRIPT_DIR/ltp-skip.txt}"
EXEC_TIMEOUT="${EXEC_TIMEOUT:-120}"
SUITE_TIMEOUT="${SUITE_TIMEOUT:-600}"
OUT_DIR="${OUT_DIR:-/tmp/ltp-smoke-$(date +%s)}"

# Default suite = smoketest (15 entries, ~30 s on a clean UML).
# Override by passing suite names on the command line.
SUITES=("$@")
[ "${#SUITES[@]}" -eq 0 ] && SUITES=("smoketest")

# --- pre-flight -------------------------------------------------------
err() { echo "run-ltp-smoke: $*" >&2; }

preflight_ok=1
if [ ! -d "$LTP_ROOT" ] || [ ! -d "$LTP_ROOT/runtest" ]; then
    err "LTP_ROOT=$LTP_ROOT missing or lacks runtest/ — clone+install upstream LTP."
    preflight_ok=0
fi
if [ ! -d "$KIRK_DIR" ] || [ ! -f "$KIRK_DIR/kirk" -a ! -f "$KIRK_DIR/libkirk/main.py" ]; then
    err "KIRK_DIR=$KIRK_DIR missing or lacks kirk entry-point — clone upstream kirk."
    preflight_ok=0
fi
if [ ! -f "$SKIP_FILE" ]; then
    err "SKIP_FILE=$SKIP_FILE missing."
    preflight_ok=0
fi
command -v python3 >/dev/null 2>&1 || {
    err "python3 not on PATH; kirk requires python3."
    preflight_ok=0
}
[ "$preflight_ok" -eq 1 ] || exit 2

mkdir -p "$OUT_DIR"

# Pick the kirk invocation that exists. Upstream kirk's entry-point
# is either a top-level `kirk` script or `python3 -m libkirk.main`.
if [ -x "$KIRK_DIR/kirk" ]; then
    KIRK_CMD=("$KIRK_DIR/kirk")
else
    KIRK_CMD=(python3 -m libkirk.main)
fi

# --- run --------------------------------------------------------------
total_pass=0
total_fail=0
total_skip=0
total_warn=0
suite_failures=0

for suite in "${SUITES[@]}"; do
    suite_log="$OUT_DIR/$suite.log"
    suite_json="$OUT_DIR/$suite.json"
    echo "[run-ltp-smoke] suite=$suite -> $suite_log"
    (
        cd "$KIRK_DIR" || exit 1
        LTPROOT="$LTP_ROOT" \
        PATH="$LTP_ROOT/testcases/bin:$PATH" \
            "${KIRK_CMD[@]}" \
                --sut host \
                --run-suite "$suite" \
                --skip-file "$SKIP_FILE" \
                --json-report "$suite_json" \
                --exec-timeout "$EXEC_TIMEOUT" \
                --suite-timeout "$SUITE_TIMEOUT" \
                --workers 1 \
                > "$suite_log" 2>&1
    )
    kirk_rc=$?

    # Parse the JSON; tolerate kirk crash producing no JSON.
    if [ -s "$suite_json" ]; then
        # kirk's JSON schema (libkirk/results.py): top-level either
        # "results" (newer) or "tests" (older) array of dicts each
        # with a "status" string. Accept both schemas.
        counters=$(python3 - "$suite_json" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    d = json.load(f)
arr = d.get("results") or d.get("tests") or []
c = {"pass":0,"fail":0,"broken":0,"skipped":0,"warning":0,"conf":0}
for r in arr:
    s = str(r.get("status","")).lower()
    c[s] = c.get(s, 0) + 1
total = sum(c.values())
fail  = c["fail"] + c["broken"]
skip  = c["skipped"] + c["conf"]
print(f"{total} {c['pass']} {fail} {skip} {c['warning']}")
PY
)
        read s_total s_pass s_fail s_skip s_warn <<<"$counters"
    else
        s_total=0; s_pass=0; s_fail=0; s_skip=0; s_warn=0
    fi

    echo "  total=$s_total pass=$s_pass fail=$s_fail skip=$s_skip warn=$s_warn (kirk_rc=$kirk_rc)"
    total_pass=$((total_pass + s_pass))
    total_fail=$((total_fail + s_fail))
    total_skip=$((total_skip + s_skip))
    total_warn=$((total_warn + s_warn))
    if [ "$s_total" -eq 0 ] || [ "$s_fail" -gt 0 ] || [ "$kirk_rc" -ne 0 ]; then
        suite_failures=$((suite_failures + 1))
    fi
done

echo
echo "[run-ltp-smoke] SUMMARY suites=${#SUITES[@]} pass=$total_pass fail=$total_fail skip=$total_skip warn=$total_warn"
echo "[run-ltp-smoke] artefacts: $OUT_DIR"

if [ "$suite_failures" -gt 0 ]; then
    echo "[run-ltp-smoke] VERDICT=FAIL suites_with_failure=$suite_failures"
    exit 1
fi
echo "[run-ltp-smoke] VERDICT=PASS"
exit 0
