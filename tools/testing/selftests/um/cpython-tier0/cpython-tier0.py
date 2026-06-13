#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# CPython "tier 0" regression gate - single-process, single-import
# tests. Today this is just `import hashlib + sha256(...)`, which
# exercises the dlopen path for Python C extensions.
#
# Coverage is intentionally narrow: a single import in a single
# process. Broader sequential stdlib imports and fork/exec workloads
# are not yet reliable under the integrated KVM backend, so this
# tier-0 gate stays on the single-process, single-import surface and
# grows only as that surface widens.
#
# Pass criterion: hashlib.sha256(b"hello").hexdigest() returns the
# canonical value `2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e
# 73043362938b9824`. The host runner asserts that exact line.
#
# Out of scope for this gate:
# - importing multiple test.* modules sequentially
# - subprocess.run / multiprocessing.Process
# - any code path that fork/exec's
#
# These stress the integrated KVM backend's singleton shadow PGD: all
# UML processes share the host process's VA space, and the shadow PGD
# is updated via current_mm_sync -> kvm_mm_map ->
# kvm_shadow_invalidate_va_range. A per-mm shadow PGD would remove
# that constraint.

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
