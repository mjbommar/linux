#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/cpython-full — full CPython stdlib regrtest under UML.
#
# Replaces the previous misleading "cpython-test"-style curated
# gates that ran 29 of 439 modules and called it "the CPython test
# suite passing".  This gate runs `python3 -m test` with no module
# list, then compares the failure set against an explicit allowlist
# of known-failing modules.  PASS iff exactly the documented set
# fails — any NEW failure is a regression.
#
# Honest accounting: the allowlist starts (in expected_failures.txt)
# at the modules that fail today.  Every line should eventually have
# a tracked issue or be flipped to pass.  This is the inverse of the
# old setup: the gate exists to make it impossible to silently regress
# more tests into the "expected failure" bucket.
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

echo "== wait for completion or 90 min timeout =="
DEADLINE=$(($(date +%s) + 5400))
while umlctl ps 2>/dev/null | grep -q "$INSTANCE"; do
    if [ "$(date +%s)" -gt "$DEADLINE" ]; then
        echo "FAIL: timed out at 90 min; killing instance"
        umlctl stop "$INSTANCE" 2>/dev/null
        umlctl rm "$INSTANCE" 2>/dev/null
        tail -50 "$LOG"
        exit 1
    fi
    sleep 60
done

# Extract failed-module list (color-strip).
ACTUAL=$(grep -aE "^test .* failed" "$LOG" 2>/dev/null \
    | sed 's/\x1b\[[0-9;]*m//g' \
    | awk '{print $2}' | sort -u)
EXPECTED=$(grep -vE "^\s*#|^\s*$" "$ALLOW" | sort -u)

UNEXPECTED=$(comm -23 <(echo "$ACTUAL") <(echo "$EXPECTED"))
FIXED=$(comm -13 <(echo "$ACTUAL") <(echo "$EXPECTED"))

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
    # This is a soft warning, not a fail — fixing tests should never break the gate.
fi

echo
echo "PASS: failure set matches expected_failures.txt"
exit 0
