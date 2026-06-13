#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/cpython-full - full CPython stdlib regrtest under UML.
#
# Runs `python3 -m test` with no module list, then compares the failure
# set against an explicit allowlist of known-failing modules. PASS iff
# exactly the documented set fails; any new failure is a regression.
#
# Exits 0 PASS / 1 FAIL / 4 SKIP (kselftest convention).

set -u

KERNEL=${UML_KERNEL:-$HOME/src/uml-builds/uml-smp-t41fix/linux}
BACKEND=${UML_BACKEND:-seccomp}
INSTANCE=cpython-full-test-$$

if [ ! -x "$KERNEL" ]; then
    echo "SKIP: UML binary $KERNEL not found (set UML_KERNEL)"
    exit 4
fi
if ! command -v umlctl >/dev/null 2>&1; then
    echo "SKIP: umlctl not in PATH (run: make -C tools/uml/uml-launcher install)"
    exit 4
fi

SRC=$(cd "$(dirname "$0")" && cd ../../../../.. && pwd)
TOML=$SRC/tools/uml/uml-launcher/examples/cpython-test-full.toml
ALLOW=$(cd "$(dirname "$0")" && pwd)/expected_failures.txt

if [ ! -f "$TOML" ]; then
    echo "SKIP: $TOML missing"
    exit 4
fi
if [ ! -f "$ALLOW" ]; then
    echo "SKIP: $ALLOW missing"
    exit 4
fi

WORK=$(mktemp -d -t cpython-full.XXXXXX)
trap 'umlctl rm "$INSTANCE" 2>/dev/null; rm -rf "$WORK"' EXIT

# Substitute the instance name so concurrent runs don't collide.
sed "s/^name = \"cpython-test-full\"$/name = \"$INSTANCE\"/" "$TOML" \
    > "$WORK/Umlfile.toml"

echo "== launch UML guest, run python3 -m test (full suite) =="
export UML_KERNEL="$KERNEL"
if ! timeout 30 umlctl up -f "$WORK/Umlfile.toml" > "$WORK/up.log" 2>&1; then
    echo "FAIL: umlctl up failed; see $WORK/up.log"
    cat "$WORK/up.log"
    exit 1
fi
echo "  started $(grep 'started' "$WORK/up.log")"

# Find the run dir umlctl created.
RUN_ID=$(grep -oE "run_id=[A-Z0-9]+" "$WORK/up.log" | cut -d= -f2)
LOG="$HOME/.local/state/uml/runs/$RUN_ID/init.log"

echo "== wait for suite completion (marker) or 90 min timeout =="
# We wait for regrtest's own completion line ("Total tests:") rather than
# for the instance to exit. umlctl's generated init shuts down with
# `echo b > /proc/sysrq-trigger` (sysrq-b == reboot), and because the
# full suite exits non-zero whenever any test fails, the init phase
# chain aborts and reboots *before* a clean power-off. The guest then
# re-runs the whole suite on the next boot, so "wait until the instance
# disappears" never returns and the run burns the full 90 min running
# the suite ~4 times. The first pass is authoritative, so stop as soon
# as it finishes.
DEADLINE=$(($(date +%s) + 5400))
while ! grep -aq "Total tests:" "$LOG" 2>/dev/null; do
    if [ "$(date +%s)" -gt "$DEADLINE" ]; then
        echo "FAIL: timed out at 90 min waiting for suite completion"
        umlctl stop "$INSTANCE" 2>/dev/null
        umlctl rm "$INSTANCE" 2>/dev/null
        tail -50 "$LOG"
        exit 1
    fi
    if ! umlctl ps 2>/dev/null | grep -q "$INSTANCE"; then
        # Instance gone. Fine only if the suite already completed.
        grep -aq "Total tests:" "$LOG" 2>/dev/null && break
        echo "FAIL: instance exited before the suite completed"
        tail -50 "$LOG"
        exit 1
    fi
    sleep 30
done
# Suite finished its first pass; stop the (rebooting) guest now. Parsing
# below runs against $LOG before the EXIT trap removes the run dir.
echo "== suite completed; stopping instance =="
umlctl stop "$INSTANCE" 2>/dev/null

# Extract the failed-test list from regrtest's authoritative summary
# block, e.g.:
#
#   5 tests failed:
#       test.test_asyncio.test_subprocess test_ensurepip test_pyrepl
#       test_signal test_socket
#
# We parse this block rather than the per-test "test X failed" lines for
# two reasons: (1) CPython 3.14 colorizes those lines with ANSI SGR *and*
# private-mode escapes (\e[?2004h ...), so a naive "^test" match never
# fires; (2) timeout failures (e.g. test_signal hitting the faulthandler
# watchdog) never emit a "test X failed" line at all, but they do appear
# in this summary. Strip both SGR and private-mode/control escapes, then
# take the LAST summary block (post-rerun result is authoritative).
strip_ansi() { sed -E 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\x1b[=>]//g'; }
# tr -d '\r': the UML console emits CRLF, so names at the end of a
# continuation line would otherwise keep a trailing CR and never match
# the allowlist.
ACTUAL=$(strip_ansi < "$LOG" 2>/dev/null | tr -d '\r' | awk '
    /[0-9]+ tests? failed:/ { collecting = 1; delete names; n = 0; next }
    collecting {
        if ($0 ~ /^[[:space:]]+[A-Za-z]/) { for (i = 1; i <= NF; i++) names[++n] = $i }
        else { collecting = 0 }
    }
    END { for (i = 1; i <= n; i++) print names[i] }
' | LC_ALL=C sort -u)
# LC_ALL=C: comm requires byte-order sorting on both inputs; locale-aware
# sort folds the "." / "_" in dotted test names and breaks the compare.
EXPECTED=$(grep -vE "^\s*#|^\s*$" "$ALLOW" | LC_ALL=C sort -u)

UNEXPECTED=$(LC_ALL=C comm -23 <(echo "$ACTUAL") <(echo "$EXPECTED"))
FIXED=$(LC_ALL=C comm -13 <(echo "$ACTUAL") <(echo "$EXPECTED"))

echo "== summary =="
TOTAL_LINE=$(grep -aE "^Total tests:" "$LOG" | tail -1)
echo "  $TOTAL_LINE"
echo "  expected failures (allowlist): $(echo "$EXPECTED" | grep -c .)"
echo "  observed failures:             $(echo "$ACTUAL" | grep -c .)"

if [ -n "$UNEXPECTED" ]; then
    echo
    echo "FAIL: NEW regressions (not in expected_failures.txt):"
    echo "$UNEXPECTED" | sed 's/^/    /'
    exit 1
fi

if [ -n "$FIXED" ]; then
    echo
    echo "  Tests that now PASS but are still in expected_failures.txt"
    echo "  (please remove from allowlist):"
    echo "$FIXED" | sed 's/^/    /'
    # This is a soft warning, not a fail; fixing tests should never
    # break the gate.
fi

echo
echo "PASS: failure set matches expected_failures.txt"
exit 0
