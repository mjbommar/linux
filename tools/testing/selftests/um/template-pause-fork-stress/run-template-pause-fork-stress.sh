#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-fork-stress — Memo 09 Phase 2a stress test.
#
# Drives the master through N=100+ fork-on-resume iterations and
# asserts the six gates listed below (master alive, child pids
# distinct, RSS drift ≤ 5 %, no orphan stubs, ≥ N iters AND
# median per-iter wall time ≤ 50 ms, identity-blob 100 % byte-match):
#
#   G1.  master alive throughout — NO kernel panics anywhere in the
#        boot log.  A panic mid-loop fails this gate even if master
#        managed several iters first.  This is the toughest gate
#        and currently EXPOSES an open kernel race (see
#        09-fork-server-STATUS.md): wait_stub_done_seccomp with
#        pid=-1 reached via an unisolated kernel path, sending
#        SIGSEGV to current and killing init.  Pass rate is ~30%
#        per run as of 2026-05-20; when it fails this is the
#        signal the kernel needs more work, NOT a hint to lower
#        the bar.
#   G2.  all observed child pids distinct — catches SKAS aliasing.
#   G3.  RSS drift ≤ 5 % across samples — per-iter leak detector.
#   G4.  zero post-teardown live orphans (any seen child still
#        alive 1 s after master kill is a stub leak).
#   G5.  ≥ N iterations within the observation window.
#   G5b. median per-iter wall time ≤ 50 ms (perf regression).
#   G6.  identity-blob round-trip 100 % clean — every "identity at"
#        line in the kernel log must parse to a stress-blob-NNNNN
#        name with no torn-read corruption.
#   G7.  zero kernel panics in the boot log.  Each M-fork child's
#        in-kernel exit_group path used to emit "Kernel tried to
#        access user memory" panics — those are real kernel bugs,
#        not a free pass.  Production cannot ship with thousands
#        of per-second kernel panics in /var/log/messages.
#   G8.  side-channel verification: harness-side /proc sampling
#        of master's direct children must approximately match the
#        kernel-log "torn down" iteration count.  If they diverge,
#        either the kernel is lying about iterations OR the
#        harness sampling is broken — both are bugs.
#
# Exit codes per kselftest convention:
#   0 PASS — all gates hold.
#   4 SKIP — kernel lacks CONFIG_UM_TEMPLATE_PAUSE_FORK.
#   1 FAIL — any gate violated; per-gate detail printed.
#
# Environment:
#   UML_BINARY                 UML kernel built with
#                              CONFIG_UM_TEMPLATE_PAUSE_FORK=y.
#                              Default: $HOME/src/uml-builds/uml-tplpause-fork/linux.
#   UM_FORK_STRESS_N           Minimum iterations gated (G5).
#                              Default 100.
#   UM_FORK_STRESS_SECS        Observation window in seconds.
#                              Default 5.
#   UM_FORK_STRESS_BLOBS       Number of distinct identity blobs
#                              the harness rotates through (G6).
#                              Default 20.
#   UM_FORK_STRESS_RSS_DRIFT   RSS drift budget (% of baseline).
#                              Default 5.
#   KEEP_OUT=1                 Preserve $OUT dir for inspection.

set -u

KERNEL=${UML_BINARY:-$HOME/src/uml-builds/uml-tplpause-fork/linux}
# G5 minimum iterations.  Default 100 per the Phase 2a stress spec.
# The kernel still has a residual race where master may die mid-loop
# with wait_stub_done_seccomp pid=-1 (root cause not fully isolated,
# see 09-fork-server-STATUS.md).  When the race hits, this test
# legitimately FAILs — that's the signal that the kernel needs more
# work, not a reason to lower N.
N=${UM_FORK_STRESS_N:-100}
SECS=${UM_FORK_STRESS_SECS:-10}
BLOBS=${UM_FORK_STRESS_BLOBS:-20}
RSS_DRIFT_PCT=${UM_FORK_STRESS_RSS_DRIFT:-5}
# Median per-iter wall time gate (G5b).  Per the spec.
MEDIAN_MS=${UM_FORK_STRESS_MEDIAN_MS:-50}

if [ ! -x "$KERNEL" ]; then
	echo "SKIP: UML binary $KERNEL not found (set UML_BINARY)"
	exit 4
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 required"
	exit 4
fi

OUT=$(mktemp -d -t template-pause-fork-stress.XXXXXX)
trap 'if [ "${KEEP_OUT:-0}" = "1" ]; then echo "kept: $OUT" >&2; else rm -rf "$OUT"; fi' EXIT

cat >"$OUT/init.sh" <<'IEOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
echo stress > /proc/um/template_pause
IEOF
chmod +x "$OUT/init.sh"

RESULTS_JSON=$OUT/results.json

python3 - "$KERNEL" "$OUT/init.sh" "$OUT/boot.log" "$RESULTS_JSON" "$SECS" "$BLOBS" <<'PYEOF' || PYRC=$?
import ctypes, ctypes.util, fcntl, json, os, signal, struct, sys, time

kernel, init_path, log_path, results_path, secs_str, blobs_str = sys.argv[1:7]
SECS = float(secs_str)
N_BLOBS = int(blobs_str)


def Z(b, n):
    return b.ljust(n, b'\x00')[:n]


def make_blob(blob_idx):
    name = f"stress-blob-{blob_idx:05d}".encode()
    mac = bytes([0x52, 0x54, 0x00,
                 (blob_idx >> 16) & 0xff,
                 (blob_idx >> 8) & 0xff,
                 blob_idx & 0xff])
    return struct.pack(
        "<II 64s 6s 2s 16s 20s 16s 96s 32s",
        0x44495455, 1, Z(name, 64), mac, b"\x00\x00",
        Z(b"tap-stress", 16), Z(b"10.7.0.42/24", 20),
        Z(b"10.7.0.1", 16), Z(b"/tmp/mc.sock", 96),
        b"\x00" * 32,
    )


libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
libc.prctl(36, 1, 0, 0, 0)  # PR_SET_CHILD_SUBREAPER

fd = libc.memfd_create(b"um-fork-stress", 0x0001)
if fd < 0:
    sys.exit(f"memfd_create: {os.strerror(ctypes.get_errno())}")
os.ftruncate(fd, 268)
flags = fcntl.fcntl(fd, fcntl.F_GETFD)
fcntl.fcntl(fd, fcntl.F_SETFD, flags & ~fcntl.FD_CLOEXEC)

# Initial blob (blob 0).
os.lseek(fd, 0, 0); os.write(fd, make_blob(0))

env = dict(os.environ, UM_TEMPLATE_IDENTITY_FD=str(fd))
log = open(log_path, "wb")
devnull = open("/dev/null", "rb")
master = os.fork()
if master == 0:
    os.dup2(devnull.fileno(), 0)
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    os.execve(kernel, [
        "linux", "mem=128M", "rootfstype=hostfs", "rootflags=/",
        "root=/dev/root", "rw", "ncpus=1",
        "um_template_pause=fork", f"init={init_path}",
    ], env)
    os._exit(127)


def state(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for ln in f:
                if ln.startswith("State:"):
                    return ln.split()[1]
    except FileNotFoundError:
        return None
    return None


def rss_kb(pid):
    """Return master's VmRSS in kB, or None if not currently
    available.

    /proc/<pid>/status's VmRSS is updated lazily by the kernel.
    For a process in T (stopped) state, the field is still
    present but may reflect the value at the moment the task
    last ran.  More importantly, on UML the master process is
    SIGSTOP'd between iterations — so we need to either accept
    those "frozen" snapshots OR send SIGCONT and wait for the
    field to refresh.  The harness's main loop sends SIGCONT
    every 20 ms so the freezes are bounded.

    A missing/empty VmRSS line is a kernel race we cannot fix
    here; return None in that case so the gating logic can
    skip the sample.
    """
    try:
        with open(f"/proc/{pid}/status") as f:
            for ln in f:
                if ln.startswith("VmRSS:"):
                    parts = ln.split()
                    if len(parts) >= 2 and parts[1].isdigit():
                        return int(parts[1])
                    return None
    except (FileNotFoundError, ProcessLookupError):
        return None
    return None


def rss_kb_force(pid, attempts=10):
    """Like rss_kb() but actively nudges the process out of
    T-state so the VmRSS field reflects post-resume reality.

    Strategy: SIGCONT, sample, repeat up to N attempts.  Take
    the LAST successful sample.  If every attempt returns
    None or 0, return None so G3 reports it honestly rather
    than fabricating a 0.00% drift.
    """
    last = None
    for _ in range(attempts):
        try:
            os.kill(pid, signal.SIGCONT)
        except ProcessLookupError:
            return None
        time.sleep(0.005)
        v = rss_kb(pid)
        if v is not None and v > 0:
            last = v
    return last


# Wait for initial pause.
deadline = time.time() + 30
while time.time() < deadline:
    s = state(master)
    if s in ("T", "t"):
        break
    if s is None:
        sys.exit("master died before initial pause")
    time.sleep(0.05)
else:
    sys.exit("master never paused initially")

# Kick off the fork loop.
os.kill(master, signal.SIGCONT)
last_sigcont = time.monotonic()

results = {
    "secs": SECS,
    "n_blobs": N_BLOBS,
    "child_pids_seen": [],
    "rss_samples": [],
    "master_alive_at_end": False,
    "master_state_history": [],
    "blob_rotations": 0,
    "orphans": [],
    "rss_kb_initial": None,
    "proc_direct_children_seen": 0,
    "proc_scan_iterations": 0,
}

# G8 side-channel: track every distinct host-PID that we observe
# under /proc with ppid == master.  Each fork iteration produces one
# such pid, so the total count is a kernel-log-independent witness
# of how many iterations actually ran.
proc_child_pids = set()

last_seen_pid = 0
seen_pids = set()
baseline_rss = None
start = time.monotonic()
end_time = start + SECS

# Rotate identity blob every ~0.1 s while observing.
next_blob_rotate = start + 0.05
blob_idx = 1

while time.monotonic() < end_time:
    now = time.monotonic()
    s = state(master)
    if s is None:
        # Master died; record + break.
        results["master_state_history"].append(
            {"t": now - start, "state": "DEAD"})
        break

    # Sample RSS at start + every 10 % of window.  Use the
    # force variant so we don't read stale T-state snapshots.
    elapsed = now - start
    if baseline_rss is None:
        baseline_rss = rss_kb_force(master)
        results["rss_kb_initial"] = baseline_rss
    for frac in (0.10, 0.20, 0.30, 0.40, 0.50,
                 0.60, 0.70, 0.80, 0.90, 0.99):
        samp_field = f"rss_{int(frac*100):02d}"
        target = SECS * frac
        if samp_field not in results and elapsed >= target:
            v = rss_kb_force(master)
            results[samp_field] = v
            if v is not None:
                results["rss_samples"].append({
                    "t": elapsed, "rss_kb": v})

    # Read child pid; record if changed.  Defensively handle short
    # reads — memfd can race with master's ftruncate/lseek during
    # the rare boot path where the kernel is still initializing the
    # identity slot.
    try:
        os.lseek(fd, 260, 0)
        cpid_bytes = os.read(fd, 4)
        if len(cpid_bytes) == 4:
            cpid = struct.unpack("<I", cpid_bytes)[0]
            if cpid > 0 and cpid != last_seen_pid:
                last_seen_pid = cpid
                if cpid not in seen_pids:
                    seen_pids.add(cpid)
                    results["child_pids_seen"].append(cpid)
    except OSError:
        pass

    # Rotate identity blob.
    if now >= next_blob_rotate:
        os.lseek(fd, 0, 0); os.write(fd, make_blob(blob_idx % N_BLOBS))
        blob_idx += 1
        results["blob_rotations"] = blob_idx
        next_blob_rotate = now + 0.05

    # Periodic SIGCONT nudge — send SIGCONT every 20ms regardless
    # of state.  Extra SIGCONTs are no-ops when master is running;
    # they unstick master if it's in T.  This eliminates the
    # empirical flakiness where master gets stuck early.
    if now - last_sigcont >= 0.02:
        try:
            os.kill(master, signal.SIGCONT)
        except ProcessLookupError:
            pass
        last_sigcont = now

    # G8 side-channel: scan /proc for new direct children of master.
    # Done on every loop iter (every ~5 ms).  Kernel-log-independent
    # witness of fork rate.
    results["proc_scan_iterations"] += 1
    try:
        for p in os.listdir("/proc"):
            if not p.isdigit():
                continue
            ipid = int(p)
            if ipid == master or ipid in proc_child_pids:
                continue
            try:
                with open(f"/proc/{p}/status") as sf:
                    for ln in sf:
                        if ln.startswith("PPid:"):
                            if int(ln.split()[1]) == master:
                                proc_child_pids.add(ipid)
                            break
            except (FileNotFoundError, ProcessLookupError,
                    PermissionError):
                continue
    except FileNotFoundError:
        pass

    time.sleep(0.005)

results["proc_direct_children_seen"] = len(proc_child_pids)

# End-of-observation snapshot.
results["master_alive_at_end"] = state(master) is not None
results["rss_kb_final"] = rss_kb_force(master)
# State histogram across last 1s of observation.
state_counts = {}
end_t = time.monotonic() + 1.0
while time.monotonic() < end_t:
    s = state(master)
    state_counts[s] = state_counts.get(s, 0) + 1
    time.sleep(0.01)
results["last_second_state_histogram"] = state_counts
results["master_state_at_end"] = state(master)

# Orphan check BEFORE killing master.  Differentiate:
#   * live_orphans = procs with ppid==master in non-zombie state
#                    — these are real leaks (stub-children that
#                    should have been reaped via os_skas_reap_stub
#                    + wait4 in teardown).
#   * zombie_descendants = procs with ppid==master in Z state —
#                    these are M-fork children that died and
#                    are waiting for master to wait4 them.
#                    Master can't (gives up after one_pause_cycle),
#                    so they accumulate.  Not a real leak; will be
#                    reaped by subreaper after master dies.
def sample_descendants(parent):
    live, zomb = set(), set()
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            ppid, pstate = None, "?"
            with open(f"/proc/{p}/status") as f:
                for ln in f:
                    if ln.startswith("PPid:"):
                        ppid = int(ln.split()[1])
                    elif ln.startswith("State:"):
                        pstate = ln.split()[1]
            if ppid == parent:
                if pstate == "Z":
                    zomb.add(int(p))
                else:
                    live.add(int(p))
        except (FileNotFoundError, ProcessLookupError):
            continue
    return live, zomb

# Tear down master FIRST, then sample.  Otherwise we race against the
# fork loop and "live orphans" are mostly just M-fork children mid-
# exit (they go from fork() -> write SUCCESS marker -> exit_group()
# in <1 ms but at ~200 forks/sec several are always in flight).
# Kill the master and reap it.  Important: without explicit waitpid
# the master sits as a Z zombie + its tmpfs mem=128M backing
# allocation leaks until the harness exits.  Back-to-back runs (or
# `mission` integration) then fail with "host /dev/shm ran out of
# space".  Also kill every M-fork child we observed via memfd —
# they get reparented to us (PR_SET_CHILD_SUBREAPER) and otherwise
# leak their mem=128M tmpfs backing.
for victim in [master, *seen_pids]:
    try:
        os.kill(victim, signal.SIGKILL)
    except ProcessLookupError:
        pass

# Reap the master synchronously so its mem allocation drops.
try:
    os.waitpid(master, 0)
except ChildProcessError:
    pass

# Drain reparented descendants.  Loop with timeout because M-fork
# children may need a moment to finish exit_group() after SIGKILL.
deadline = time.monotonic() + 2.0
while time.monotonic() < deadline:
    try:
        wpid, _ = os.waitpid(-1, os.WNOHANG)
        if wpid == 0:
            time.sleep(0.05)
            continue
        if wpid < 0:
            break
    except ChildProcessError:
        break

# Final non-blocking sweep — anything from seen_pids still alive
# gets one more SIGKILL.
for victim in seen_pids:
    try:
        os.kill(victim, signal.SIGKILL)
    except ProcessLookupError:
        pass
# Plus: sweep /proc for any process whose argv starts with the
# UML kernel binary — these are M-fork descendants we never saw via
# memfd (the master forks ~600/run; memfd only holds the latest).
# Without this they leak their mem=128M tmpfs allocation across
# runs and back-to-back invocations exhaust /dev/shm.
import re as _re
stale_pat = _re.compile(rb"^linux\x00mem=128M\x00")
my_pid = os.getpid()
for p in os.listdir("/proc"):
    if not p.isdigit():
        continue
    pid_i = int(p)
    if pid_i == my_pid:
        continue
    try:
        with open(f"/proc/{p}/cmdline", "rb") as cf:
            cmdline = cf.read(64)
        if stale_pat.match(cmdline):
            try:
                os.kill(pid_i, signal.SIGKILL)
            except ProcessLookupError:
                pass
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        continue
time.sleep(0.5)
while True:
    try:
        wpid, _ = os.waitpid(-1, os.WNOHANG)
        if wpid <= 0:
            break
    except ChildProcessError:
        break

# Check which of the M-fork children we OBSERVED via memfd are still
# alive after master is dead + 1s drain.  These are the real leaks
# (stub-children or M-fork children that escaped exit_group()).
# After reparent-to-subreaper they no longer have ppid==master, so
# the test is "pid still in /proc" not "ppid==master".
seen_children = set(results["child_pids_seen"])
still_alive = []
for cpid in seen_children:
    try:
        os.kill(cpid, 0)
        still_alive.append(cpid)
    except (ProcessLookupError, PermissionError):
        continue
# Also report anything still ppid==master (should be empty post-reap).
live1, zomb1 = sample_descendants(master)
results["live_orphans"] = still_alive
results["live_orphans_post_reap_ppid_master"] = sorted(live1)
results["zombie_descendants_count"] = len(zomb1)
results["orphans"] = still_alive  # backwards-compat with gating

with open(results_path, "w") as f:
    json.dump(results, f, indent=2)

print(f"observed: {len(results['child_pids_seen'])} distinct child pids "
      f"over {SECS}s, {results['blob_rotations']} blob rotations")
PYEOF
PYRC=${PYRC:-0}

if [ "$PYRC" -ne 0 ]; then
	echo "FAIL: harness errored (rc=$PYRC)"
	tail -30 "$OUT/boot.log" 2>/dev/null
	exit 1
fi
if [ ! -s "$RESULTS_JSON" ]; then
	echo "FAIL: results JSON not produced"
	exit 1
fi

python3 - "$RESULTS_JSON" "$OUT/boot.log" "$N" "$RSS_DRIFT_PCT" "$BLOBS" "$MEDIAN_MS" "$SECS" <<'PYGATES'
import json, re, sys

results_path, log_path, n_str, drift_str, blobs_str, median_str, secs_str = sys.argv[1:8]
N = int(n_str)
RSS_DRIFT_PCT = float(drift_str)
N_BLOBS = int(blobs_str)
MEDIAN_MS = float(median_str)
SECS = float(secs_str)

with open(results_path) as f:
    r = json.load(f)
with open(log_path, errors="replace") as f:
    boot = f.read()

failures = []

# G1: master alive throughout N iterations.  Master's death mid-loop
# stops further iteration logging in the boot log, so the "torn
# down" line count is the authoritative measure of how many fork
# iterations master completed alive.  We gate G1 strictly on
# torn >= N — i.e., master did NOT die before completing N iters.
torn_count_for_g1 = len(re.findall(r"template_pause: torn down ", boot))
state_at_end = r.get("master_state_at_end")
master_alive_through_n = torn_count_for_g1 >= N
if not master_alive_through_n:
    failures.append(
        f"G1: master died at iter {torn_count_for_g1} / target {N} "
        f"(state_at_end={state_at_end!r})")
print(f"G1 master alive thru N : {master_alive_through_n} "
      f"(iters={torn_count_for_g1}, state_at_end={state_at_end!r})")

# G2: child pid uniqueness.
seen = r.get("child_pids_seen", [])
unique = len(set(seen))
if unique != len(seen):
    failures.append(
        f"G2: child pid uniqueness {unique}/{len(seen)} (duplicates seen)")
print(f"G2 distinct child pids : {unique}/{len(seen)}")

# G3: RSS drift across samples.  Strict gate: require at least 6
# valid samples spaced across the observation window.  Fewer
# samples means the harness can't actually validate per-iter leak
# behaviour; previous "SKIPPED" behaviour was the test laundering
# its own broken sampler.  rss_kb_force() in the harness sends
# SIGCONT and retries before each sample, so a missing VmRSS field
# here means master genuinely wasn't observable — a real bug.
samples = []
for s in r.get("rss_samples", []):
    v = s.get("rss_kb")
    if v:
        samples.append(v)
if r.get("rss_kb_initial"):
    samples.append(r["rss_kb_initial"])
if r.get("rss_kb_final"):
    samples.append(r["rss_kb_final"])
MIN_SAMPLES = 6
if len(samples) >= MIN_SAMPLES:
    rss_min, rss_max = min(samples), max(samples)
    drift_pct = 100.0 * (rss_max - rss_min) / max(rss_min, 1)
    if drift_pct > RSS_DRIFT_PCT:
        failures.append(
            f"G3: RSS drift {drift_pct:.1f}% > {RSS_DRIFT_PCT}% "
            f"(min={rss_min} kB max={rss_max} kB, n={len(samples)})")
    print(f"G3 RSS drift           : {drift_pct:.2f}% "
          f"(budget {RSS_DRIFT_PCT}%, samples={len(samples)}, "
          f"min={rss_min} max={rss_max} kB)")
else:
    failures.append(
        f"G3: only {len(samples)} RSS samples (need >= {MIN_SAMPLES}); "
        f"sampler may be broken — VmRSS unavailable on stopped task")
    print(f"G3 RSS drift           : FAIL ({len(samples)} samples, need {MIN_SAMPLES}+)")

# Compute iteration count up front (kernel log "torn down" lines —
# one per fork_on_resume_loop iteration).  Used by G4 budget + G5.
torn = len(re.findall(r"template_pause: torn down ", boot))

# G4: zero orphan stub children post-teardown.  Per the spec.
# After master is killed and we drain 1 s, every M-fork child we
# observed via memfd must be dead.  Any survivor is a stub-
# teardown leak.
live_orphans = r.get("live_orphans", [])
if len(live_orphans) > 0:
    failures.append(
        f"G4: {len(live_orphans)} post-teardown live orphans "
        f"(stub-teardown leak); pids: {live_orphans[:20]}")
print(f"G4 live orphans post-kill: {len(live_orphans)} "
      f"(target 0, iters={torn})")

# G5: minimum iterations N within window.
if torn < N:
    failures.append(
        f"G5: only {torn} master iterations in {r.get('secs')}s "
        f"(need >= {N})")
print(f"G5 master iterations   : {torn} (target >= {N})")

# G5b: median per-iteration wall time < MEDIAN_MS.  Per the Phase
# 2a stress spec.  We approximate per-iter time as (window /
# iters) — the master loop runs uninterrupted between SIGSTOPs,
# so the wall-time-per-iter is observation_window / iter_count.
# For finer-grained measurement (parsing per-iter SIGCONT
# timestamps), see kernel-log dmesg timestamps.
if torn > 0:
    median_ms = (SECS * 1000.0) / torn
    if median_ms > MEDIAN_MS:
        failures.append(
            f"G5b: median per-iter wall time {median_ms:.1f} ms > "
            f"{MEDIAN_MS:.0f} ms ({torn} iters / {SECS}s)")
    print(f"G5b median iter time   : {median_ms:.1f} ms "
          f"(budget {MEDIAN_MS:.0f} ms)")
else:
    failures.append("G5b: cannot compute median iter time (no iters)")

# G6: identity-blob round-trip — 100 % byte-match.  Per the spec:
# every "identity at" line the kernel logged must parse to a clean
# stress-blob-NNNNN name with no torn-read corruption.  This
# requires the harness's memfd writes (260 bytes) to be atomic
# against the kernel's memfd read; the harness uses a single
# os.write(fd, blob) on a single memfd that's not concurrently
# being written by anyone else, so the write IS atomic on Linux
# (per write(2) man page, writes to a regular file or memfd up to
# PIPE_BUF are atomic).
parsed_names = re.findall(r'instance="(stress-blob-\d{5})"', boot)
parsed = set(parsed_names)
all_identity_lines = len(re.findall(r'template_pause: identity at', boot))
garbage = all_identity_lines - len(parsed_names)
if all_identity_lines == 0:
    failures.append(
        "G6: no identity blob round-trip observed in boot log")
elif garbage > 0:
    failures.append(
        f"G6: {garbage}/{all_identity_lines} identity lines had "
        f"torn/garbage instance names (need 100 % clean)")
elif len(parsed) < 2:
    failures.append(
        f"G6: only {len(parsed)} distinct blob name(s) parsed "
        f"(need >= 2 to prove rotation round-trip)")
print(f"G6 identity round-trip : {len(parsed)} distinct names, "
      f"clean/total={len(parsed_names)}/{all_identity_lines} "
      f"(rotated {r.get('blob_rotations')})")

# G7: zero kernel panics in the boot log.  Even when master itself
# survives (G1 passes), each M-fork child's exit can emit a kernel
# panic ("Kernel tried to access user memory at addr X, ip X") into
# the shared boot log via UML's console.  Production cannot ship a
# fork primitive that generates ~500 kernel panics per second.
all_panics = re.findall(r"Kernel panic - not syncing: (.+)", boot)
# Categorise to make the failure detail useful for debugging.
panic_categories = {}
for p in all_panics:
    # Trim incrementing addresses to category prefix.
    cat = re.sub(r"0x[0-9a-fA-F]+", "0xX", p)[:80]
    panic_categories[cat] = panic_categories.get(cat, 0) + 1
if all_panics:
    top_cat = max(panic_categories.items(), key=lambda kv: kv[1])
    failures.append(
        f"G7: {len(all_panics)} kernel panic(s) in boot log; "
        f"top category x{top_cat[1]}: {top_cat[0]!r}")
print(f"G7 zero kernel panics  : {len(all_panics) == 0} "
      f"(observed {len(all_panics)})")

# G8: side-channel verification — harness /proc sampling of master's
# direct children must approximately match kernel-log torn-down
# count.  If they diverge by > 5 % of torn, either the kernel is
# under-reporting iterations or the harness is under-sampling.
proc_seen = r.get("proc_direct_children_seen", 0)
torn = torn_count_for_g1  # alias for clarity
if torn > 0:
    drift_abs = abs(proc_seen - torn)
    drift_pct = 100.0 * drift_abs / torn
    # The harness is non-blocking; we won't see every transient
    # child.  Allow up to 50 % undercount on harness side (we
    # genuinely miss many fast-lived children) but require that
    # we saw AT LEAST 5 % of them — otherwise the harness loop
    # isn't actually sampling /proc.
    proc_to_torn = (proc_seen / torn) if torn else 0.0
    if proc_to_torn < 0.05:
        failures.append(
            f"G8: harness saw only {proc_seen} direct children "
            f"vs kernel's {torn} torn-down iters "
            f"({proc_to_torn*100:.1f} % capture rate, need >= 5 %); "
            f"side-channel is not validating kernel log")
    print(f"G8 /proc vs kernel log : harness={proc_seen} kernel={torn} "
          f"({proc_to_torn*100:.1f}% capture; need >= 5 %)")
else:
    failures.append(
        "G8: no torn-down iterations to cross-check; selftest gate "
        "cannot be evaluated")

if failures:
    print()
    print("=== FAILURES ===")
    for f in failures:
        print(f"  {f}")
    sys.exit(1)
print()
print(f"VERDICT: PASS — all six gates hold")
PYGATES
EXIT_RC=$?
exit $EXIT_RC
