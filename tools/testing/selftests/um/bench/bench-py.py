#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# bench-py — canned, fixed-cost Python workload for cross-backend
# regression tracking. Boot UML with this as init (or run from
# inside an init shell). Exits 0 on success after printing one
# BENCH summary line per sample plus a final BENCH_MEDIAN line.
#
# Designed to:
#   - finish in ~5 s per sample on a modern host (5 samples = ~25 s)
#   - exercise the three syscall classes that diverge most between
#     UML backends: trap-mechanism (getpid), file I/O (open/write/
#     read/stat), socket-fd lifecycle (socketpair/close)
#   - use ONLY Python stdlib + tmpfs — no host-FS dependency
#     beyond what every UML init image already has
#
# Output (one line per sample, plus median):
#   BENCH: tier=py sample=<N> elapsed_ms=<X> getpid_ms=<G> \
#          fs_ms=<F> sock_ms=<S> hash_ms=<H>
#   BENCH_MEDIAN: tier=py samples=<N> elapsed_ms_p50=<X> \
#                 elapsed_ms_p10=<Y> elapsed_ms_p90=<Z>
#
# The host-side wrapper (run-bench.sh) parses BENCH_MEDIAN.

import hashlib
import os
import socket
import statistics
import struct
import sys
import tempfile
import time

# Tunable counts. Sized so each section is ~1 s on a modern host
# at python3 ~10 µs/syscall. Halved if BENCH_QUICK is set (used
# in CI to keep a single sample under 5 s).
_quick = os.environ.get("BENCH_QUICK") == "1"
HASH_ITERS = 1000 if _quick else 2000
FS_FILES = 100 if _quick else 200
GETPID_ITERS = 10000 if _quick else 20000
SOCKET_PAIRS = 50 if _quick else 100
SAMPLES = int(os.environ.get("BENCH_SAMPLES", "5"))
WARMUP = int(os.environ.get("BENCH_WARMUP", "1"))


def section_hash():
    """CPU + small mm churn. No syscalls in the hot loop."""
    h = hashlib.sha256()
    buf = b"x" * 4096
    for _ in range(HASH_ITERS):
        h.update(buf)
    return h.hexdigest()


def section_fs():
    """fs syscalls in tmpfs: open + write + close + stat + open + read + close."""
    with tempfile.TemporaryDirectory() as d:
        for i in range(FS_FILES):
            p = f"{d}/f{i}"
            with open(p, "wb") as f:
                f.write(struct.pack("<I", i) * 16)
            os.stat(p)
        total = 0
        for i in range(FS_FILES):
            with open(f"{d}/f{i}", "rb") as f:
                total += len(f.read())
    return total


def section_getpid():
    """Bare syscall churn — closest analogue to the C getpid-loop micro."""
    g = os.getpid
    for _ in range(GETPID_ITERS):
        g()


def section_socket():
    """fd lifecycle: socketpair + close. Two fds per pair."""
    for _ in range(SOCKET_PAIRS):
        a, b = socket.socketpair()
        a.close()
        b.close()


def run_one(sample_idx):
    t_total0 = time.monotonic_ns()

    t0 = time.monotonic_ns()
    section_hash()
    hash_ms = (time.monotonic_ns() - t0) / 1e6

    t0 = time.monotonic_ns()
    section_fs()
    fs_ms = (time.monotonic_ns() - t0) / 1e6

    t0 = time.monotonic_ns()
    section_getpid()
    getpid_ms = (time.monotonic_ns() - t0) / 1e6

    t0 = time.monotonic_ns()
    section_socket()
    sock_ms = (time.monotonic_ns() - t0) / 1e6

    elapsed_ms = (time.monotonic_ns() - t_total0) / 1e6

    print(
        f"BENCH: tier=py sample={sample_idx} elapsed_ms={elapsed_ms:.2f} "
        f"getpid_ms={getpid_ms:.2f} fs_ms={fs_ms:.2f} "
        f"sock_ms={sock_ms:.2f} hash_ms={hash_ms:.2f}",
        flush=True,
    )
    return elapsed_ms


def main():
    # Warm-up runs (results discarded). Stabilizes JIT / page cache.
    for w in range(WARMUP):
        section_hash()  # cheap warm-up; full run is ~1s/section

    samples = []
    for i in range(SAMPLES):
        samples.append(run_one(i))

    samples_sorted = sorted(samples)
    p50 = statistics.median(samples_sorted)
    if len(samples_sorted) >= 10:
        p10 = samples_sorted[len(samples_sorted) // 10]
        p90 = samples_sorted[(len(samples_sorted) * 9) // 10]
    else:
        p10 = samples_sorted[0]
        p90 = samples_sorted[-1]

    print(
        f"BENCH_MEDIAN: tier=py samples={len(samples)} "
        f"elapsed_ms_p50={p50:.2f} elapsed_ms_p10={p10:.2f} "
        f"elapsed_ms_p90={p90:.2f}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
