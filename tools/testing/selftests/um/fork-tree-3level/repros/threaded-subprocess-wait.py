#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Threaded fork+wait stress reproducer for user-space crashes under
# concurrent subprocess creation.
#
# Pattern: 2 Python threads, each loops subprocess.Popen([sys.executable,
# "-c", "import time; time.sleep(0.001)"]) + popen.wait(timeout=10).
# Mimics regrtest's `python -m test -j2` worker spawning shape.
#
# Failure manifests as random user-space SIGSEGV:
#   python3[122]: segfault at <high cr2> ip <legit user ip> error 4
# i.e. the user process accesses a random VA and segfaults. Suggests
# CR3 / page-table corruption under heavy mm-create churn.
#
# Many KVM_V2_TLB_LAG diagnostic lines precede each failure, with lag
# values into the thousands.

import threading, subprocess, sys, traceback

ITERS_PER_WORKER = 200
N_WORKERS = 2

fails = []

def worker(wid):
    for i in range(ITERS_PER_WORKER):
        try:
            p = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(0.001)"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            out, err = p.communicate(timeout=10)
            rc = p.returncode
            if rc != 0:
                # Capture worker stdout/stderr so the subprocess failure
                # is visible without flooding the kselftest log.
                tag = f"[w{wid} iter={i}] PYRC1: rc={rc}"
                fails.append((wid, i, "rc", rc, err[:500]))
                print(f"{tag} stdout={out[:200]!r} stderr={err[:500]!r}",
                      flush=True)
        except BaseException as e:
            fails.append((wid, i, type(e).__name__, str(e)))
            print(f"[w{wid} iter={i}] FAIL: {type(e).__name__}: {e!r}",
                  flush=True)
            traceback.print_exc()
            return

threads = [threading.Thread(target=worker, args=(w,))
           for w in range(N_WORKERS)]
for t in threads:
    t.start()
for t in threads:
    t.join()

total = ITERS_PER_WORKER * N_WORKERS
print(f"DONE iters={total} fails={len(fails)}", flush=True)
sys.exit(1 if fails else 0)
