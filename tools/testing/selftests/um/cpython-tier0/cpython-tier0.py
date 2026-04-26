#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# CPython "tier 0" regression gate — single-process, single-import
# tests. Today this is just `import hashlib + sha256(...)` which is
# the *reliable* repro for the original task #274 backend bug.
#
# Aspiration: the whole CPython test suite, tiered. Reality: only
# hashlib is currently load-bearing here because larger sweeps
# (multiple stdlib imports done sequentially) tickle a residual
# shadow-PT-vs-pgd staleness that's still unfixed. We add modules
# back to this list ONE AT A TIME as the residual bugs land.
#
# Pass criterion: hashlib.sha256(b"hello").hexdigest() returns the
# canonical value `2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e
# 73043362938b9824`. The host runner asserts that exact line.
#
# Out of scope today (documented divergences from seccomp parity):
# - importing multiple test.* modules sequentially (heap corrupts
#   under kvm; works under seccomp)
# - subprocess.run / multiprocessing.Process (parent crashes under
#   kvm; works under seccomp)
# - any code path that fork/exec's
#
# These all share the same root cause: under the integrated kvm
# backend, all UML processes share the host process's VA space, AND
# the singleton shadow PGD is updated only via current_mm_sync →
# kvm_mm_map → kvm_shadow_invalidate_va_range. The cumulative-imports
# case appears to hit a path where pgd updates land but shadow
# invalidation is skipped (cleared=0 case in invalidate_va_range);
# the fork+exec case hits cross-process host-VA collision. Per-mm
# shadow PGD (#243) is the architectural fix; until then, this
# kselftest documents the actual reliable surface.

import sys
import hashlib

# The exact assertion. Don't change without understanding that this
# value is what the host runner greps for.
EXPECTED = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"

got = hashlib.sha256(b"hello").hexdigest()
if got == EXPECTED:
    print("CPYTHON_TIER0: hashlib sha256 ok = %s" % got)
    print("CPYTHON_TIER0: TOTAL PASS")
    sys.exit(0)

print("CPYTHON_TIER0: hashlib MISMATCH got=%s expected=%s" % (got, EXPECTED))
print("CPYTHON_TIER0: TOTAL FAIL")
sys.exit(1)
