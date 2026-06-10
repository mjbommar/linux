#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/pool-bench - pool acceptance harness.
#
# Boots one `umlctl pool serve` daemon and drives it through the pool
# acceptance gates:
#
#   1. take.p50/p99 latency over 1000 sequential takes.
#      gate: p50 <= 5 ms, p99 <= 50 ms
#   2. memory amplification across 100 sequential takes.
#      gate: RSS(supervisor + master + all live children) <= 200 MiB
#            (with a 128 MiB master)
#   3. lifecycle leak across 10 000 take+destroy cycles.
#      gate: RSS drift (supervisor + master) start vs. end <= 5 %
#   4. sustained throughput: 60 s of take+destroy at 50/sec target.
#      gate: successful takes >= 2700 (90 % of 3000 target)
#
# Each gate prints a single POOL_BENCH line with measurement, ceiling,
# and verdict.  The runner exits 0 only if all gates pass.
#
# Exit codes (kselftest convention):
#   0 PASS - all gates hold
#   4 SKIP - /dev/kvm absent, $UM_FORK_KERNEL missing, or umlctl unbuilt
#   1 FAIL - any gate violated
#
# Environment:
#   UM_FORK_KERNEL    Fork-mode UML kernel built with
#                     CONFIG_UM_TEMPLATE_PAUSE_FORK=y.  Default:
#                     $HOME/src/uml-builds/uml-tplpause-fork/linux.
#   UMLCTL            umlctl binary path (auto-detected if absent).
#   POOL_BENCH_TAKES         Latency-gate take count.  Default 1000.
#   POOL_BENCH_FORKS         Memory-gate fork count.   Default 100.
#   POOL_BENCH_LIFECYCLE_N   Leak-gate cycle count.    Default 10000.
#   POOL_BENCH_THROUGHPUT_S  Throughput window (s).   Default 60.
#   POOL_BENCH_THROUGHPUT_R  Target rate (/sec).      Default 50.
#   KEEP_OUT=1               Preserve $OUT dir for inspection.

set -u

KERNEL=${UM_FORK_KERNEL:-$HOME/src/uml-builds/uml-tplpause-fork/linux}
UMLCTL=${UMLCTL:-}

TAKES=${POOL_BENCH_TAKES:-1000}
FORKS=${POOL_BENCH_FORKS:-100}
LIFECYCLE_N=${POOL_BENCH_LIFECYCLE_N:-10000}
THROUGHPUT_S=${POOL_BENCH_THROUGHPUT_S:-60}
THROUGHPUT_R=${POOL_BENCH_THROUGHPUT_R:-50}

if [ -z "$UMLCTL" ]; then
	for cand in \
		"$(dirname "$0")/../../../../uml/uml-launcher/target/release/umlctl" \
		"$(dirname "$0")/../../../../uml/uml-launcher/target/debug/umlctl"; do
		if [ -x "$cand" ]; then
			UMLCTL=$cand
			break
		fi
	done
fi

# Skip semantics (per kselftest convention, exit 4).
if [ ! -c /dev/kvm ]; then
	echo "SKIP: /dev/kvm absent (UML host needs KVM for fork-mode boot speed)"
	exit 4
fi
if [ -z "$UMLCTL" ] || [ ! -x "$UMLCTL" ]; then
	echo "SKIP: umlctl not built (set UMLCTL=...)"
	exit 4
fi
if [ ! -x "$KERNEL" ]; then
	echo "SKIP: fork-mode UML kernel $KERNEL not found (set UM_FORK_KERNEL)"
	echo "  build with:  make ARCH=um O=\$HOME/src/uml-builds/uml-tplpause-fork \\"
	echo "                 defconfig && scripts/config --enable UM_TEMPLATE_PAUSE \\"
	echo "                 --enable UM_TEMPLATE_PAUSE_FORK && make ARCH=um O=..."
	exit 4
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 required"
	exit 4
fi

POOL_NAME="pool-bench-$$"
OUT=$(mktemp -d -t pool-bench.XXXXXX)
RUNTIME=$(mktemp -d -t pool-bench-rt.XXXXXX)
SOCK="$RUNTIME/pools/$POOL_NAME/api.sock"
PIDFILE="$RUNTIME/pools/$POOL_NAME/serve.pid"
RESULTS_JSON="$OUT/results.json"

cleanup() {
	if [ -S "$SOCK" ]; then
		python3 -c "
import socket, json
s = socket.socket(socket.AF_UNIX); s.settimeout(5)
try:
    s.connect('$SOCK')
    s.sendall(b'{\"op\":\"shutdown\"}\n')
    s.recv(4096)
except Exception:
    pass
" 2>/dev/null || true
	fi
	if [ -f "$PIDFILE" ]; then
		dpid=$(cat "$PIDFILE" 2>/dev/null || true)
		[ -n "$dpid" ] && kill -KILL "$dpid" 2>/dev/null || true
	fi
	if [ -n "${MASTER_PID:-}" ]; then
		kill -KILL "$MASTER_PID" 2>/dev/null || true
	fi
	# Sweep any UML masters that may have escaped - same defensive
	# pattern as template-pause-fork-stress, since each leaks a
	# 128 MiB tmpfs allocation otherwise.
	pgrep -f "linux mem=128M" 2>/dev/null | while read -r p; do
		kill -KILL "$p" 2>/dev/null || true
	done
	if [ "${KEEP_OUT:-0}" = "1" ]; then
		echo "kept: $OUT" >&2
		echo "kept: $RUNTIME" >&2
	else
		rm -rf "$OUT" "$RUNTIME"
	fi
}
trap cleanup EXIT

echo "pool-bench: $TAKES takes / $FORKS forks / $LIFECYCLE_N lifecycle"
echo "pool-bench: ${THROUGHPUT_S}s sustained @ ${THROUGHPUT_R}/s"
echo "pool-bench: kernel=$KERNEL"
echo "pool-bench: umlctl=$UMLCTL"

# Launch the daemon in the background.
"$UMLCTL" --runtime-dir "$RUNTIME" pool serve \
	--name "$POOL_NAME" \
	--kernel "$KERNEL" \
	--mem 128M \
	--background \
	>"$OUT/serve.out" 2>"$OUT/serve.err"
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: umlctl pool serve --background exited $RC"
	cat "$OUT/serve.err"
	exit 1
fi

# Wait for socket up.
DEADLINE=$(( $(date +%s) + 60 ))
while [ ! -S "$SOCK" ]; do
	if [ "$(date +%s)" -gt $DEADLINE ]; then
		echo "FAIL: socket $SOCK not created in 60s"
		ls -la "$RUNTIME/pools/$POOL_NAME/" 2>/dev/null
		tail -50 "$OUT/serve.err"
		exit 1
	fi
	sleep 0.1
done

DAEMON_PID=$(cat "$PIDFILE")
echo "pool-bench: daemon up, daemon_pid=$DAEMON_PID"

# Find the master pid via status RPC.
STATUS=$(python3 -c "
import socket, json, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(10)
s.connect('$SOCK')
s.sendall(b'{\"op\":\"status\"}\n')
buf=b''
while not buf.endswith(b'\n'):
    c = s.recv(4096)
    if not c: break
    buf += c
print(buf.decode().rstrip())
")
MASTER_PID=$(echo "$STATUS" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['master_pid'])")
echo "pool-bench: master_pid=$MASTER_PID"
if ! kill -0 "$MASTER_PID" 2>/dev/null; then
	echo "FAIL: master pid $MASTER_PID not alive at start"
	exit 1
fi

# Run the bench harness.  Single Python invocation so the gates share
# one daemon boot - boot is ~5-10 s on a healthy host and we don't
# want to amortize it four times.
python3 - \
	"$SOCK" "$DAEMON_PID" "$MASTER_PID" \
	"$TAKES" "$FORKS" "$LIFECYCLE_N" \
	"$THROUGHPUT_S" "$THROUGHPUT_R" \
	"$RESULTS_JSON" <<'PYEOF' || PYRC=$?
import json, os, socket, struct, sys, time

(sock_path, daemon_pid_s, master_pid_s,
 takes_s, forks_s, lifecycle_s,
 throughput_s_s, throughput_r_s,
 results_path) = sys.argv[1:10]
DAEMON_PID = int(daemon_pid_s)
MASTER_PID = int(master_pid_s)
TAKES = int(takes_s)
FORKS = int(forks_s)
LIFECYCLE_N = int(lifecycle_s)
THROUGHPUT_S = float(throughput_s_s)
THROUGHPUT_R = float(throughput_r_s)


def rpc(payload, timeout=10.0):
    """One JSON-line round-trip.  Returns the parsed response.

    A fresh socket per call matches the daemon's "one request per
    connection" contract (pool_serve.rs::handle_client), and the cost
    is ~10 us of AF_UNIX socket+connect+close - well below the 5 ms
    p50 budget.
    """
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    s.sendall((json.dumps(payload) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf.decode())


def rss_kb(pid):
    """Return VmRSS in kB for a pid, or None if not readable.

    The supervisor (umlctl pool serve) is a normal Rust process so its
    RSS is always immediately available.  The master is SIGSTOP'd
    between iterations and its VmRSS field is updated lazily; these
    gates sample it after a take returns (master is back in T state
    with a stable VmRSS), so the lazy-update issue does not matter
    here.
    """
    try:
        with open(f"/proc/{pid}/status") as f:
            for ln in f:
                if ln.startswith("VmRSS:"):
                    parts = ln.split()
                    if len(parts) >= 2 and parts[1].isdigit():
                        return int(parts[1])
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None
    return None


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, PermissionError):
        return False


def take_payload(instance, mac_suffix):
    """Build a take request with a per-call MAC + instance tag.

    The kernel validates the identity blob's magic + version, so we
    have to round-trip a structurally valid blob even though the M-fork
    child does not actually plumb identity.
    """
    a = (mac_suffix >> 16) & 0xff
    b = (mac_suffix >> 8) & 0xff
    c = mac_suffix & 0xff
    return {
        "op": "take",
        "instance": instance,
        "mac": f"52:54:00:{a:02x}:{b:02x}:{c:02x}",
        "tap": "tap-bench",
        "ipv4": "10.7.0.42/24",
        "gateway": "10.7.0.1",
        "mconsole": "",
    }


def percentile(sorted_xs, p):
    """Linear-interpolated percentile over a pre-sorted list."""
    if not sorted_xs:
        return float("nan")
    if len(sorted_xs) == 1:
        return float(sorted_xs[0])
    k = (len(sorted_xs) - 1) * p
    lo = int(k)
    hi = min(lo + 1, len(sorted_xs) - 1)
    frac = k - lo
    return sorted_xs[lo] * (1 - frac) + sorted_xs[hi] * frac


results = {
    "daemon_pid": DAEMON_PID,
    "master_pid": MASTER_PID,
    "takes": TAKES,
    "forks": FORKS,
    "lifecycle_n": LIFECYCLE_N,
    "throughput_s": THROUGHPUT_S,
    "throughput_r": THROUGHPUT_R,
}

# Warm-up: 5 takes so the first-take memfd/fork-stack mmap cost
# doesn't poison gate 1's p50.  These are explicitly NOT counted.
for i in range(5):
    r = rpc(take_payload(f"warm-{i}", i))
    if not r.get("ok"):
        print(f"FAIL: warm-up take {i} failed: {r}")
        sys.exit(1)

# ---------------------------------------------------------------------
# Gate 1: take.p50/p99 over $TAKES sequential takes.
# ---------------------------------------------------------------------
print(f"gate 1: measuring {TAKES} sequential takes...", flush=True)
latencies_ms = []
fails = 0
t0 = time.monotonic_ns()
for i in range(TAKES):
    a = time.monotonic_ns()
    r = rpc(take_payload(f"lat-{i}", 0x10000 + i))
    b = time.monotonic_ns()
    if not r.get("ok"):
        fails += 1
        continue
    latencies_ms.append((b - a) / 1e6)
t1 = time.monotonic_ns()
results["gate1_takes_attempted"] = TAKES
results["gate1_takes_successful"] = len(latencies_ms)
results["gate1_takes_failed"] = fails
results["gate1_wall_s"] = (t1 - t0) / 1e9
if not latencies_ms:
    results["gate1_p50_ms"] = None
    results["gate1_p99_ms"] = None
else:
    latencies_ms.sort()
    results["gate1_p50_ms"] = percentile(latencies_ms, 0.50)
    results["gate1_p99_ms"] = percentile(latencies_ms, 0.99)
    results["gate1_min_ms"] = latencies_ms[0]
    results["gate1_max_ms"] = latencies_ms[-1]
print(f"  -> {len(latencies_ms)} ok / {fails} fail / wall={results['gate1_wall_s']:.2f}s",
      flush=True)

# ---------------------------------------------------------------------
# Gate 2: memory amplification at 100 takes.
# ---------------------------------------------------------------------
# Baseline: supervisor + master only.
print(f"gate 2: measuring memory at {FORKS} takes...", flush=True)
rss_supervisor_start = rss_kb(DAEMON_PID)
rss_master_start = rss_kb(MASTER_PID)
fork_pids = []
fork_fails = 0
for i in range(FORKS):
    r = rpc(take_payload(f"fork-{i}", 0x20000 + i))
    if not r.get("ok"):
        fork_fails += 1
        continue
    p = r.get("result", {}).get("pid", 0)
    if p > 0:
        fork_pids.append(p)
# Sum the RSS of supervisor + master + every still-runnable fork pid.
# The M-fork children are SIGKILL'd by the master immediately
# post-fork and become zombies (VmRSS=0 / unreadable);
# the live-RSS bucket therefore captures actual amplification,
# which is what the gate is about.  Zombies are accounted as 0.
rss_supervisor_end = rss_kb(DAEMON_PID) or 0
rss_master_end = rss_kb(MASTER_PID) or 0
rss_children_kb = 0
live_children = 0
for p in fork_pids:
    if pid_alive(p):
        v = rss_kb(p)
        if v:
            rss_children_kb += v
            live_children += 1
total_kb = rss_supervisor_end + rss_master_end + rss_children_kb
results["gate2_supervisor_rss_kb_start"] = rss_supervisor_start
results["gate2_supervisor_rss_kb_end"] = rss_supervisor_end
results["gate2_master_rss_kb_start"] = rss_master_start
results["gate2_master_rss_kb_end"] = rss_master_end
results["gate2_children_rss_kb"] = rss_children_kb
results["gate2_live_children"] = live_children
results["gate2_total_kb"] = total_kb
results["gate2_forks_attempted"] = FORKS
results["gate2_forks_failed"] = fork_fails
print(f"  -> supervisor={rss_supervisor_end} kB master={rss_master_end} kB "
      f"children={rss_children_kb} kB (live={live_children}/{FORKS}) "
      f"total={total_kb} kB ({total_kb/1024:.1f} MiB)", flush=True)

# Destroy all the fork pids the daemon recorded so the daemon's
# member-table size drops back to ~0 before the lifecycle gate.
for p in fork_pids:
    try:
        rpc({"op": "destroy", "pid": p})
    except Exception:
        pass

# ---------------------------------------------------------------------
# Gate 3: lifecycle leak over $LIFECYCLE_N take+destroy cycles.
# ---------------------------------------------------------------------
print(f"gate 3: measuring drift over {LIFECYCLE_N} take+destroy cycles...",
      flush=True)
# Sample (supervisor + master) RSS at start, midpoint, and end.
g3_rss = lambda: (rss_kb(DAEMON_PID) or 0) + (rss_kb(MASTER_PID) or 0)
rss_start = g3_rss()
mid = LIFECYCLE_N // 2
rss_mid = None
g3_fails = 0
g3_t0 = time.monotonic_ns()
for i in range(LIFECYCLE_N):
    r = rpc(take_payload(f"lc-{i}", 0x30000 + (i & 0xffff)))
    if not r.get("ok"):
        g3_fails += 1
        continue
    p = r.get("result", {}).get("pid", 0)
    if p > 0:
        # Destroy via RPC so the daemon drops the member-table entry;
        # the master has already SIGKILL'd it, so this is primarily a
        # daemon-side bookkeeping unbind.
        try:
            rpc({"op": "destroy", "pid": p})
        except Exception:
            pass
    if i == mid:
        rss_mid = g3_rss()
g3_t1 = time.monotonic_ns()
rss_end = g3_rss()
drift_pct = 100.0 * abs(rss_end - rss_start) / max(rss_start, 1)
results["gate3_rss_kb_start"] = rss_start
results["gate3_rss_kb_mid"] = rss_mid
results["gate3_rss_kb_end"] = rss_end
results["gate3_drift_pct"] = drift_pct
results["gate3_cycles_attempted"] = LIFECYCLE_N
results["gate3_cycles_failed"] = g3_fails
results["gate3_wall_s"] = (g3_t1 - g3_t0) / 1e9
print(f"  -> start={rss_start} kB mid={rss_mid} kB end={rss_end} kB "
      f"drift={drift_pct:.2f}% wall={results['gate3_wall_s']:.1f}s "
      f"fails={g3_fails}/{LIFECYCLE_N}", flush=True)

# ---------------------------------------------------------------------
# Gate 4: sustained throughput at $THROUGHPUT_R/sec for $THROUGHPUT_S s.
# ---------------------------------------------------------------------
print(f"gate 4: sustained takes for {THROUGHPUT_S:.0f}s @ {THROUGHPUT_R:.0f}/sec...",
      flush=True)
target_period = 1.0 / THROUGHPUT_R
deadline = time.monotonic() + THROUGHPUT_S
ok_count = 0
fail_count = 0
i = 0
next_send = time.monotonic()
while time.monotonic() < deadline:
    now = time.monotonic()
    # Pace by sleeping until the next slot if we're ahead of schedule.
    if now < next_send:
        time.sleep(min(next_send - now, 0.01))
        continue
    try:
        r = rpc(take_payload(f"tp-{i}", 0x40000 + (i & 0xffff)),
                timeout=5.0)
    except Exception:
        fail_count += 1
        i += 1
        next_send += target_period
        continue
    if r.get("ok"):
        ok_count += 1
        p = r.get("result", {}).get("pid", 0)
        if p > 0:
            # Close = destroy; matches the spec's "take + close".
            try:
                rpc({"op": "destroy", "pid": p}, timeout=2.0)
            except Exception:
                pass
    else:
        fail_count += 1
    i += 1
    next_send += target_period
results["gate4_window_s"] = THROUGHPUT_S
results["gate4_target_rate"] = THROUGHPUT_R
results["gate4_target_total"] = int(THROUGHPUT_S * THROUGHPUT_R)
results["gate4_ok"] = ok_count
results["gate4_fail"] = fail_count
print(f"  -> ok={ok_count} fail={fail_count} "
      f"target_total={results['gate4_target_total']}", flush=True)

with open(results_path, "w") as f:
    json.dump(results, f, indent=2)
PYEOF
PYRC=${PYRC:-0}

if [ "$PYRC" -ne 0 ]; then
	echo "FAIL: harness errored (rc=$PYRC)"
	tail -30 "$OUT/serve.err" 2>/dev/null
	exit 1
fi
if [ ! -s "$RESULTS_JSON" ]; then
	echo "FAIL: results JSON not produced"
	exit 1
fi

# --------------------------------------------------------------------
# Gate evaluation.  One POOL_BENCH line per gate + a summary line.
# --------------------------------------------------------------------
python3 - "$RESULTS_JSON" <<'PYGATES'
import json, sys

with open(sys.argv[1]) as f:
    r = json.load(f)

# Gate ceilings.
GATE_P50_MS = 5.0
GATE_P99_MS = 50.0
GATE_RSS_TOTAL_MIB = 200.0
GATE_DRIFT_PCT = 5.0
GATE_THROUGHPUT_FRAC = 0.90  # 90 % of the target_total

passes = 0
total = 0
fails = []


def report(name, measured_s, gate_s, ok):
    global passes, total
    total += 1
    verdict = "PASS" if ok else "FAIL"
    if ok:
        passes += 1
    else:
        fails.append(f"{name}: {measured_s} (gate {gate_s})")
    print(f"POOL_BENCH {name}: {measured_s} (gate {gate_s}) {verdict}")


# Gate 1a + 1b: latency p50 + p99.
p50 = r.get("gate1_p50_ms")
p99 = r.get("gate1_p99_ms")
if p50 is None or p99 is None:
    report("take.p50", "n/a (no successful takes)", f"{GATE_P50_MS:.1f}ms", False)
    report("take.p99", "n/a (no successful takes)", f"{GATE_P99_MS:.1f}ms", False)
else:
    report("take.p50", f"{p50:.1f}ms", f"{GATE_P50_MS:.1f}ms", p50 <= GATE_P50_MS)
    report("take.p99", f"{p99:.1f}ms", f"{GATE_P99_MS:.1f}ms", p99 <= GATE_P99_MS)

# Gate 2: memory amplification.
#
# The gate is meaningless without live children to measure.
# Earlier fork paths SIGKILL'd children immediately post-fork, which
# would let this gate silently PASS at (master + supervisor) RSS only
# - a misleading "200 MiB for 100 forks" claim when the real number
# was "200 MiB for 0 live children."  Require at least half the
# attempted forks to be alive when we sample RSS; report a clear
# FAIL otherwise.  Operators reading this output get a real
# amplification number or an honest "gate cannot measure" verdict.
total_kb = r.get("gate2_total_kb", 0)
total_mib = total_kb / 1024.0
attempted = r.get("gate2_forks_attempted", 0) or 1
live = r.get("gate2_live_children", 0)
live_frac = live / attempted
if live_frac < 0.5:
    report("rss.100forks",
           f"{total_mib:.1f}MiB (only {live}/{attempted} live; gate cannot measure)",
           f"{GATE_RSS_TOTAL_MIB:.0f}MiB",
           False)
else:
    report("rss.100forks",
           f"{total_mib:.1f}MiB ({live}/{attempted} live)",
           f"{GATE_RSS_TOTAL_MIB:.0f}MiB",
           total_mib <= GATE_RSS_TOTAL_MIB)

# Gate 3: lifecycle drift.
drift = r.get("gate3_drift_pct", 0.0)
report("lifecycle.drift",
       f"{drift:.2f}%",
       f"{GATE_DRIFT_PCT:.1f}%",
       drift <= GATE_DRIFT_PCT)

# Gate 4: sustained throughput.
ok_count = r.get("gate4_ok", 0)
target = r.get("gate4_target_total", 0)
gate_floor = int(target * GATE_THROUGHPUT_FRAC)
report("throughput.60s",
       f"{ok_count}",
       f"{gate_floor}",
       ok_count >= gate_floor)

print()
print(f"POOL_BENCH: {passes}/{total} GATES PASS")
if fails:
    print("=== FAILURES ===")
    for f in fails:
        print(f"  {f}")
    sys.exit(1)
sys.exit(0)
PYGATES
EXIT_RC=$?

# Best-effort daemon shutdown.
if [ -S "$SOCK" ]; then
	python3 -c "
import socket
s = socket.socket(socket.AF_UNIX); s.settimeout(5)
try:
    s.connect('$SOCK')
    s.sendall(b'{\"op\":\"shutdown\"}\n')
    s.recv(4096)
except Exception:
    pass
" 2>/dev/null || true
fi

exit $EXIT_RC
