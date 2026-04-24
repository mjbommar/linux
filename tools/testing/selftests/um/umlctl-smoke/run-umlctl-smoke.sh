#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/umlctl-smoke/run-umlctl-smoke.sh — regression guard for
# tools/uml/uml-launcher/src/bin/umlctl (the multi-instance
# lifecycle CLI described in Documentation/virt/uml/redesign/
# 08-future-phases/05-umlctl.md).
#
# Drives the full umlctl v1 lifecycle against a fake kernel (a
# shell script that prints the ready marker and then sleeps),
# so the test covers the CLI + supervision code without
# depending on a real UML build.
#
# The regression this really locks in: `umlctl stop` must
# terminate the child cleanly with no orphan-accumulation. If
# a future change breaks the signal + poll + escalate path, the
# final `pgrep` check fails and this selftest goes red.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.
#
# Environment:
#   UMLCTL        path to umlctl binary (default: walk up the
#                 tree to tools/uml/uml-launcher/target/debug/
#                 then release/)
#   KEEP_TMPDIR   if set, leave the state/runtime tmpdir for
#                 debugging.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)

if [ -n "${UMLCTL:-}" ]; then
	UMLCTL_BIN="$UMLCTL"
else
	ROOT=$(cd "$DIR/../../../../.." && pwd)
	UMLCTL_BIN=""
	for cand in \
		"$ROOT/tools/uml/uml-launcher/target/debug/umlctl" \
		"$ROOT/tools/uml/uml-launcher/target/release/umlctl"; do
		if [ -x "$cand" ]; then
			UMLCTL_BIN="$cand"
			break
		fi
	done
fi

if [ -z "$UMLCTL_BIN" ] || [ ! -x "$UMLCTL_BIN" ]; then
	echo "SKIP: umlctl not built (run: cargo build --bin umlctl -C tools/uml/uml-launcher)" >&2
	exit 4
fi

TMP=$(mktemp -d)
cleanup() {
	rc=$?
	if [ -z "${KEEP_TMPDIR:-}" ]; then
		rm -rf "$TMP"
	else
		echo "tmp retained at $TMP"
	fi
	exit $rc
}
trap cleanup EXIT INT TERM

# Fake kernel: print the ready marker so the detach-mode
# ready-wait trips, then sleep long enough that `ps` + `stop`
# land while it's alive.
cat > "$TMP/linux" <<'FAKE'
#!/bin/sh
echo "Linux version fake"
echo "Freeing unused kernel memory"
exec sleep 120
FAKE
chmod +x "$TMP/linux"

STATE_DIR="$TMP/state"
RUNTIME_DIR="$TMP/run"
ARGS="--state-dir $STATE_DIR --runtime-dir $RUNTIME_DIR"
NAME="smoke-1"

fail() {
	echo "UMLCTL_SMOKE: FAIL $1"
	exit 1
}

# --- Part A: name validation rejects bad names ---
if "$UMLCTL_BIN" $ARGS create "Bad Name" --kernel "$TMP/linux" 2>/dev/null; then
	fail "create accepted invalid name 'Bad Name'"
fi
if "$UMLCTL_BIN" $ARGS create "-leading" --kernel "$TMP/linux" 2>/dev/null; then
	fail "create accepted invalid name '-leading'"
fi

# --- Part B: create + manifest shape ---
"$UMLCTL_BIN" $ARGS create "$NAME" \
	--kernel "$TMP/linux" \
	--profile research \
	--mem 128M \
	--label env=smoketest \
	>/dev/null || fail "create failed"

MANIFEST="$STATE_DIR/instances/$NAME.toml"
[ -f "$MANIFEST" ] || fail "manifest file not written"
grep -q 'schema_version = 1' "$MANIFEST" \
	|| fail "manifest missing schema_version"
grep -q 'profile = "research"' "$MANIFEST" \
	|| fail "manifest missing profile"
grep -q '^env = "smoketest"$' "$MANIFEST" \
	|| fail "manifest missing label"

# Duplicate create without --force → exit 4 (conflict).
"$UMLCTL_BIN" $ARGS create "$NAME" --kernel "$TMP/linux" 2>/dev/null
rc=$?
[ $rc -eq 4 ] || fail "duplicate create exited $rc (want 4)"

# --- Part C: start + ps + logs + stop ---
START_OUT=$("$UMLCTL_BIN" $ARGS start "$NAME" --ready-timeout 10) \
	|| fail "start failed"
echo "$START_OUT" | grep -Eq 'run_id=[0-9A-HJKMNP-TV-Z]{26}' \
	|| fail "start output missing ULID run_id: $START_OUT"

PIDFILE="$RUNTIME_DIR/$NAME.pid"
RUN_ID_FILE="$RUNTIME_DIR/$NAME.run_id"
[ -f "$PIDFILE" ] || fail "pidfile not written"
[ -f "$RUN_ID_FILE" ] || fail "run_id side-file not written"
PID=$(cat "$PIDFILE")
RUN_ID=$(cat "$RUN_ID_FILE")
kill -0 "$PID" 2>/dev/null || fail "pid $PID not alive after start"

# Bundle directory exists and contains the expected skeleton.
BUNDLE_DIR="$STATE_DIR/runs/$RUN_ID"
[ -d "$BUNDLE_DIR" ] || fail "bundle dir missing: $BUNDLE_DIR"
[ -f "$BUNDLE_DIR/run.json" ] || fail "run.json missing: $BUNDLE_DIR/run.json"
[ -f "$BUNDLE_DIR/init.log" ] || fail "init.log missing: $BUNDLE_DIR/init.log"
grep -q "\"run_id\": \"$RUN_ID\"" "$BUNDLE_DIR/run.json" \
	|| fail "run.json missing run_id: $(cat "$BUNDLE_DIR/run.json")"
grep -q "\"instance\": \"$NAME\"" "$BUNDLE_DIR/run.json" \
	|| fail "run.json missing instance"
grep -q '"host_ts_ns_at_exec"' "$BUNDLE_DIR/run.json" \
	|| fail "run.json missing host_ts_ns_at_exec"

# ps lists the running instance.
PS_OUT=$("$UMLCTL_BIN" $ARGS ps)
echo "$PS_OUT" | grep -q "^$NAME" || fail "ps output missing $NAME: $PS_OUT"
echo "$PS_OUT" | grep -q "running" || fail "ps not marking running: $PS_OUT"

# ps --json emits NDJSON.
JSON_OUT=$("$UMLCTL_BIN" $ARGS --json ps)
echo "$JSON_OUT" | grep -q "\"name\":\"$NAME\"" \
	|| fail "json ps missing name: $JSON_OUT"
echo "$JSON_OUT" | grep -q "\"state\":\"running\"" \
	|| fail "json ps missing running state"

# ps --filter label=env=smoketest matches.
FILT=$("$UMLCTL_BIN" $ARGS ps --filter label=env=smoketest --quiet)
[ "$FILT" = "$NAME" ] || fail "label filter mismatch: got '$FILT'"

# ps --filter label=env=wrong does not match.
FILT_NO=$("$UMLCTL_BIN" $ARGS ps --filter label=env=wrong --quiet)
[ -z "$FILT_NO" ] || fail "negative label filter returned '$FILT_NO'"

# logs prints the fake kernel's ready marker.
LOG_OUT=$("$UMLCTL_BIN" $ARGS logs "$NAME")
echo "$LOG_OUT" | grep -q "Linux version fake" \
	|| fail "logs missing ready marker: $LOG_OUT"

# --- Part D: stop must actually kill the child ---
STOP_OUT=$("$UMLCTL_BIN" $ARGS stop "$NAME") || fail "stop failed"
echo "$STOP_OUT" | grep -q "run_id=$RUN_ID" \
	|| fail "stop output missing run_id: $STOP_OUT"

# Give the kernel a moment to reap.
for _ in 1 2 3 4 5 6 7 8 9 10; do
	if ! kill -0 "$PID" 2>/dev/null; then
		break
	fi
	sleep 0.1
done

if kill -0 "$PID" 2>/dev/null; then
	fail "pid $PID still alive after stop (the orphan-killer is broken)"
fi

[ ! -f "$PIDFILE" ] || fail "pidfile lingered after stop: $PIDFILE"
[ ! -f "$RUN_ID_FILE" ] || fail "run_id side-file lingered after stop"

# run.json should now be finalized with exit-side fields.
grep -q '"host_ts_ns_at_exit"' "$BUNDLE_DIR/run.json" \
	|| fail "run.json not finalized on stop (no host_ts_ns_at_exit)"
grep -q '"signal_sent": "TERM"' "$BUNDLE_DIR/run.json" \
	|| fail "run.json missing signal_sent=TERM after stop"

# stop of already-stopped instance → exit 6 (not running).
"$UMLCTL_BIN" $ARGS stop "$NAME" 2>/dev/null
rc=$?
[ $rc -eq 6 ] || fail "stop-of-stopped exited $rc (want 6)"

# --- Part E: rm + history ---
"$UMLCTL_BIN" $ARGS rm "$NAME" >/dev/null || fail "rm failed"
[ ! -f "$MANIFEST" ] || fail "manifest lingered after rm: $MANIFEST"

# rm of nonexistent → exit 3.
"$UMLCTL_BIN" $ARGS rm "$NAME" 2>/dev/null
rc=$?
[ $rc -eq 3 ] || fail "rm-of-missing exited $rc (want 3)"

# history.jsonl captured all four lifecycle events.
HIST="$STATE_DIR/history.jsonl"
[ -f "$HIST" ] || fail "history.jsonl not written"
for ev in create start stop rm; do
	grep -q "\"event\":\"$ev\"" "$HIST" \
		|| fail "history missing event '$ev'"
done

# start + stop history entries must include the run_id (added
# by the observability-spine O1.1 lift).
grep '"event":"start"' "$HIST" | grep -q "\"run_id\":\"$RUN_ID\"" \
	|| fail "history start event missing run_id"
grep '"event":"stop"' "$HIST" | grep -q "\"run_id\":\"$RUN_ID\"" \
	|| fail "history stop event missing run_id"

# Bundle should have been cleaned up by `rm` (no --keep-logs).
[ ! -d "$BUNDLE_DIR" ] || fail "bundle dir lingered after rm: $BUNDLE_DIR"

# --- Part G: observability-spine events.jsonl + schema verb ---
#
# Spin up a second instance just to populate events.jsonl so
# we can assert on the structured-event pipeline (O1.3). The
# bundle we torched in Part E took its events.jsonl with it.
N2="smoke-events"
"$UMLCTL_BIN" $ARGS create "$N2" --kernel "$TMP/linux" --profile research \
	>/dev/null || fail "events create failed"
"$UMLCTL_BIN" $ARGS start "$N2" --ready-timeout 10 >/dev/null \
	|| fail "events start failed"
N2_RUN_ID=$(cat "$RUNTIME_DIR/$N2.run_id")
"$UMLCTL_BIN" $ARGS stop "$N2" >/dev/null || fail "events stop failed"

EV="$STATE_DIR/runs/$N2_RUN_ID/events.jsonl"
[ -f "$EV" ] || fail "events.jsonl not written: $EV"

# Both start + stop uml.lifecycle.v1 records present.
grep -q '"schema":"uml.lifecycle.v1"' "$EV" \
	|| fail "events.jsonl missing uml.lifecycle.v1 schema"
grep -q '"event.action":"start"' "$EV" \
	|| fail "events.jsonl missing start action"
grep -q '"event.action":"stop"' "$EV" \
	|| fail "events.jsonl missing stop action"
grep -q "\"run_id\":\"$N2_RUN_ID\"" "$EV" \
	|| fail "events.jsonl missing run_id"
grep -q '"host_ts_ns":' "$EV" \
	|| fail "events.jsonl missing host_ts_ns (boot-offset clock)"

# umlctl schema lists declared schemas.
SCHEMA_OUT=$("$UMLCTL_BIN" schema)
echo "$SCHEMA_OUT" | grep -q uml.lifecycle.v1 \
	|| fail "schema verb missing uml.lifecycle.v1"
echo "$SCHEMA_OUT" | grep -q uml.panic.v1 \
	|| fail "schema verb missing uml.panic.v1"
echo "$SCHEMA_OUT" | grep -q uml.oom.v1 \
	|| fail "schema verb missing uml.oom.v1"

# umlctl events <name> reads the latest bundle's events.jsonl.
EVENTS_OUT=$("$UMLCTL_BIN" $ARGS events "$N2")
[ "$(echo "$EVENTS_OUT" | wc -l)" = "2" ] \
	|| fail "events verb should return 2 lifecycle events, got: $EVENTS_OUT"
echo "$EVENTS_OUT" | grep -q '"event.action":"start"' \
	|| fail "events output missing start action"
echo "$EVENTS_OUT" | grep -q '"event.action":"stop"' \
	|| fail "events output missing stop action"

# --filter event.action=start narrows to one event.
FILT_OUT=$("$UMLCTL_BIN" $ARGS events "$N2" --filter event.action=start)
[ "$(echo "$FILT_OUT" | wc -l)" = "1" ] \
	|| fail "filter event.action=start should narrow to 1, got: $FILT_OUT"

# Name-or-run-id ambiguity: passing the literal run_id reads
# the bundle directly without looking up the instance.
DIRECT_OUT=$("$UMLCTL_BIN" $ARGS events "$N2_RUN_ID" --filter event.action=stop)
[ "$(echo "$DIRECT_OUT" | wc -l)" = "1" ] \
	|| fail "events <run_id> should resolve directly, got: $DIRECT_OUT"

# Bad filter → exit 1.
"$UMLCTL_BIN" $ARGS events "$N2" --filter bogus 2>/dev/null
rc=$?
[ $rc -eq 1 ] || fail "bad filter should exit 1, got $rc"

"$UMLCTL_BIN" $ARGS rm "$N2" >/dev/null

# --- Part F: no orphans ---
LEAK=$(pgrep -f "$TMP/linux" || true)
[ -z "$LEAK" ] || fail "orphan processes left: $LEAK"

echo "UMLCTL_SMOKE: PASS"
exit 0
