#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# template-pause-fork-stress gates self-test.
#
# Validates that the strict G1-G8 gates correctly FAIL on synthetic
# inputs that simulate each failure mode.  This is a meta-test: it
# tests the test itself, not the kernel.
#
# Why: in production runs the gates always pass (kernel is healthy),
# so we never see them fail.  A bug in the gate logic could silently
# hide a kernel regression.  This self-test guarantees each gate
# actually fires when its failure mode is present.
#
# Exit codes:
#   0 - all gates correctly failed on their respective synthetic
#       inputs (the gates work).
#   1 - at least one gate did NOT detect its failure mode (the test
#       framework is broken; fix before relying on results).

import json
import os
import sys
import tempfile

SCRIPT = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "run-template-pause-fork-stress.sh",
)


def extract_gate_python():
    """Pull the inline `python3 - ... <<'PYGATES'` block out of the
    selftest shell script.  Avoids vendoring the gate logic; the
    self-test always validates the SAME code that runs in production.
    """
    with open(SCRIPT) as f:
        text = f.read()
    mark_start = text.find('python3 - "$RESULTS_JSON"')
    heredoc_start = text.find("<<'PYGATES'", mark_start)
    content_start = text.index("\n", heredoc_start) + 1
    mark_end = text.find("\nPYGATES\n", content_start)
    return text[content_start:mark_end]


def run_gates(results, boot, n, drift, blobs, median, secs):
    """Exec the gate logic with the given synthetic inputs.
    Returns (exit_code, stdout_text).
    """
    rj = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump(results, rj)
    rj.close()
    bl = tempfile.NamedTemporaryFile("w", suffix=".log", delete=False)
    bl.write(boot)
    bl.close()

    gate_py = extract_gate_python()

    # Run in a subprocess so SystemExit doesn't kill us, and so each
    # case starts fresh.
    import subprocess
    proc = subprocess.run(
        ["python3", "-c", gate_py,
         rj.name, bl.name, str(n), str(drift),
         str(blobs), str(median), str(secs)],
        capture_output=True, text=True,
    )
    os.unlink(rj.name)
    os.unlink(bl.name)
    return proc.returncode, proc.stdout + proc.stderr


def baseline_results(**overrides):
    r = {
        "secs": 10,
        "n_blobs": 20,
        "child_pids_seen": list(range(100)),
        "rss_samples": [{"t": float(i), "rss_kb": 100000} for i in range(8)],
        "rss_kb_initial": 100000,
        "rss_kb_final": 100000,
        "master_alive_at_end": True,
        "master_state_at_end": "T",
        "master_state_history": [],
        "blob_rotations": 100,
        "orphans": [],
        "live_orphans": [],
        "live_orphans_transient_count": 0,
        "zombie_descendants_count": 0,
        "live_orphans_post_reap_ppid_master": [],
        "proc_direct_children_seen": 100,
        "proc_scan_iterations": 100,
    }
    r.update(overrides)
    return r


def baseline_boot(iters=100, distinct_blobs=20, clean=True):
    """A boot.log that satisfies all gates by default.  Override
    iters / distinct_blobs / clean to break specific gates.
    """
    lines = []
    for i in range(iters):
        if clean:
            name = f"stress-blob-{(i % distinct_blobs):05d}"
        else:
            name = "garbage-NOT-stress-blob"
        lines.append(
            f'template_pause: identity at "x" instance="{name}" '
            f'mac=00:00 tap=t ipv4=i gw=g'
        )
        lines.append("template_pause: torn down 0 stub(s) pre-fork")
    return "\n".join(lines) + "\n"


# Baseline: 500 iters in 10 s = 20 ms/iter, under G5b's 50 ms budget.
BASE_ITERS = 500

CASES = [
    # (description, results, boot, expected_failure_keyword)
    ("baseline passes",
     baseline_results(child_pids_seen=list(range(BASE_ITERS)),
                       proc_direct_children_seen=BASE_ITERS),
     baseline_boot(iters=BASE_ITERS),
     None),

    ("G1: master died early (50 iters < N=100)",
     baseline_results(), baseline_boot(iters=50), "G1"),

    ("G2: duplicate child pids",
     baseline_results(child_pids_seen=[1] * BASE_ITERS,
                       proc_direct_children_seen=BASE_ITERS),
     baseline_boot(iters=BASE_ITERS), "G2"),

    ("G3: RSS drift > 5%",
     baseline_results(
         rss_samples=[{"t": float(i), "rss_kb": 100000 + i*2000}
                      for i in range(8)],
         rss_kb_final=120000,
         child_pids_seen=list(range(BASE_ITERS)),
         proc_direct_children_seen=BASE_ITERS),
     baseline_boot(iters=BASE_ITERS), "G3"),

    ("G3: insufficient samples",
     baseline_results(rss_samples=[{"t": 1.0, "rss_kb": 100000}],
                       rss_kb_initial=100000, rss_kb_final=None,
                       child_pids_seen=list(range(BASE_ITERS)),
                       proc_direct_children_seen=BASE_ITERS),
     baseline_boot(iters=BASE_ITERS), "G3"),

    ("G4: live orphans post-kill",
     baseline_results(live_orphans=[1234, 5678],
                       orphans=[1234, 5678],
                       child_pids_seen=list(range(BASE_ITERS)),
                       proc_direct_children_seen=BASE_ITERS),
     baseline_boot(iters=BASE_ITERS), "G4"),

    ("G5: too few iterations",
     baseline_results(), baseline_boot(iters=50), "G5"),

    ("G5b: median iter time > budget",
     baseline_results(secs=60.0,
                       child_pids_seen=list(range(BASE_ITERS)),
                       proc_direct_children_seen=BASE_ITERS),
     baseline_boot(iters=BASE_ITERS), "G5b"),

    ("G6: torn-read garbage blob names",
     baseline_results(child_pids_seen=list(range(BASE_ITERS)),
                       proc_direct_children_seen=BASE_ITERS),
     baseline_boot(iters=BASE_ITERS, clean=False), "G6"),

    ("G7: kernel panic in log",
     baseline_results(child_pids_seen=list(range(BASE_ITERS)),
                       proc_direct_children_seen=BASE_ITERS),
     baseline_boot(iters=BASE_ITERS) +
     "Kernel panic - not syncing: synthetic\n",
     "G7"),

    ("G8: harness saw too few children",
     baseline_results(child_pids_seen=list(range(BASE_ITERS)),
                       proc_direct_children_seen=1),
     baseline_boot(iters=BASE_ITERS), "G8"),
]


def main():
    fails = 0
    for desc, results, boot, expected_gate in CASES:
        # secs param matches results.secs so G5b math is consistent
        secs = results.get("secs", 10.0)
        # Default tuning: N=100, drift=5, blobs=20, median=50
        rc, out = run_gates(results, boot, n=100, drift=5,
                             blobs=20, median=50, secs=secs)
        if expected_gate is None:
            # Baseline should pass.
            ok = rc == 0
        else:
            # Specific gate should appear in FAILURES section.
            ok = rc == 1 and f"{expected_gate}:" in out

        marker = "OK  " if ok else "FAIL"
        print(f"{marker} {desc}")
        if not ok:
            fails += 1
            print("  --- output ---")
            for line in out.splitlines()[-20:]:
                print(f"  {line}")
            print("  ---")

    if fails:
        print(f"\nFAIL: {fails} gate self-test case(s) did not behave as expected.")
        return 1
    print(f"\nPASS: all {len(CASES)} gate self-test cases behaved correctly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
