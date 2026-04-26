#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# CPython "tier 0" regression gate — single-process tests with no
# fork/exec, exercising C-extension dlopen + memory + computation.
# Each module's TestCase suite is loaded via importlib + run via
# unittest.TextTestRunner; we tally pass/fail/error counts and emit
# one line per module.
#
# Pass criterion: every module reports `ok=True` (zero failures, zero
# errors). The host-side runner asserts on that.
#
# Module list is intentionally narrow to keep run-time tractable
# inside a kselftest budget. Each module here must pass identically
# under backend=force=seccomp and backend=force=kvm — that's the
# whole point of the tier-0 gate.
#
# Adding a module: pick something pure-CPU (no subprocess, no
# threading-with-fork, no networking, no /dev/<x>). Keep run-time
# under ~60s on a typical dev host. If it fails identically under
# seccomp, it's not a backend bug — fix the test environment, don't
# add it here.

import os
import sys
import time
import unittest
import importlib

# CPython tests typically write temp files to CWD.
os.chdir("/tmp")

# Tier-0 list. Curated to be fast (~60s total on a modern x86_64),
# pure CPU + memory, no fork/exec/threads/networking. Every module
# here passed cleanly under both seccomp and kvm at landing time.
TIER0_MODULES = [
    "test.test_struct",       # 43 tests   — C-extension (built-in)
    "test.test_math",         # 89 tests   — libm dlopen
    "test.test_hashlib",      # 82 tests   — _hashlib + libcrypto dlopen
    "test.test_bisect",       # 46 tests
    "test.test_array",        # 890 tests  — large array exercises
    "test.test_decimal",      # 732 tests  — _decimal C extension
    "test.test_fractions",    # 50 tests
    "test.test_string",       # 54 tests
    "test.test_bytes",        # 317 tests
    "test.test_dict",         # 120 tests
    "test.test_list",         # 68 tests
    "test.test_set",          # 630 tests
    "test.test_tuple",        # 38 tests
    "test.test_int",          # 52 tests
    "test.test_float",        # 54 tests
    "test.test_complex",      # 37 tests
    "test.test_heapq",        # 69 tests
    "test.test_itertools",    # 137 tests
    "test.test_operator",     # 110 tests
    "test.test_typing",       # 709 tests  — large but all single-process
    "test.test_abc",          # 72 tests
]

total_run = 0
total_fail = 0
total_err = 0
all_ok = True

for name in TIER0_MODULES:
    t0 = time.time()
    try:
        mod = importlib.import_module(name)
    except ImportError as e:
        print("CPYTHON_TIER0: %s SKIP_NOT_PACKAGED" % name)
        continue
    suite = unittest.TestLoader().loadTestsFromModule(mod)
    runner = unittest.TextTestRunner(verbosity=0, stream=sys.stdout)
    result = runner.run(suite)
    elapsed = time.time() - t0
    ok = result.wasSuccessful()
    print("CPYTHON_TIER0: %s tests=%d fail=%d err=%d ok=%s elapsed=%.1fs" % (
        name, result.testsRun, len(result.failures), len(result.errors),
        ok, elapsed), flush=True)
    total_run += result.testsRun
    total_fail += len(result.failures)
    total_err += len(result.errors)
    if not ok:
        all_ok = False
        # Print first 200 chars of each failure for diagnostics
        for kind, errs in [("ERROR", result.errors), ("FAIL", result.failures)]:
            for test, tb in errs[:3]:
                print("CPYTHON_TIER0: %s %s/%s: %s" % (
                    name, kind, test.id().split(".")[-1],
                    tb.split("\n")[-2] if "\n" in tb else tb[:200]))

print("CPYTHON_TIER0: TOTAL run=%d fail=%d err=%d %s" % (
    total_run, total_fail, total_err, "PASS" if all_ok else "FAIL"))
sys.exit(0 if all_ok else 1)
