# #17 — numpy import-after-cryptography pathology — investigation memo

**Date:** 2026-05-14.
**Track:** C (Polish).
**Status:** investigation memo — proposes a closure path. Actual
verification is operator-time (UML boot under tier2 venv).

## The symptom

When `tier1-smoketest.py` is invoked in its all-in-one mode
(`python3 tier1-smoketest.py` with no argv), three libraries are
imported in sequence inside ONE Python process:

  1. `requests`
  2. `cryptography`
  3. `numpy`

Under UML on EITHER backend (kvm-v2 OR seccomp), step 3 raises
`ImportError: cannot import name '__config__' from 'numpy'` (or
similar in-numpy submodule import failure).

This is NOT a v2-regression — seccomp shows the same failure —
so it's NOT triggered by anything the kvm-v2 backend adds. It is
some UML-host-side environment artefact that surfaces only when
the prior `cryptography` import leaves state that breaks the
later `numpy` import.

The workaround already in tree
(`tools/testing/selftests/um/soak/tier1-smoketest.py:113-114`):

  > "we hit a kvm-v2 import-chain pathology when requests +
  > cryptography + numpy share a process; tracked separately,
  > see soak/README.md"

The fix in the soak template runs each library in its own fresh
Python process via `tier1-pylibs.toml.template`'s per-phase
invocation. Phase J's daily-rotation 80/80 = 100 % on Tier 1
proves that workaround is effective.

## The hypothesis

The pathology is most likely one of:

**Hypothesis A — Debian/Ubuntu `apt`-installed `python3-numpy`
ships an under-populated install.** The numpy wheel that Debian
packages strips the `numpy/__config__.py` BLAS/LAPACK metadata
that upstream numpy auto-generates at wheel-build time. When
`cryptography` is imported first, it triggers a `_imp_lock` or
sys.modules pin that prevents numpy's lazy build-time-metadata
fallback path. The all-in-one mode then fails at
`from numpy import __config__`.

**Hypothesis B — hostfs lazy-load timing.** UML's hostfs walk
loads `cryptography`'s `_rust` shared object lazily; if the load
spans a clock_gettime boundary the subsequent `numpy`
`importlib.import_module('numpy.__config__')` confuses the
import lock, racing the cryptography submodule completion.

**Hypothesis C — FPU state corruption across cryptography → numpy.**
`cryptography` exercises AES-NI assembly. If the lazy XSAVE
arming (SMP-T57 Phase A's first-dispatch CPUID/OSXSAVE/XCR0
chicken-and-egg) leaves the AVX context dirty during the
following numpy BLAS dispatch, the BLAS load can spurious-fault.
But: this should ONLY happen on kvm-v2 (seccomp doesn't run
in-guest AVX). #17 fails on BOTH backends, so this hypothesis is
likely WRONG. Catalogued for completeness.

The PLAN-2026-05-14 §2.2 line item carries the **leading**
hypothesis:

  > "#17 numpy import-after-cryptography | numpy.__config__
  > ImportError under both backends after cryptography touch |
  > UML env (NOT v2-regression) | `uv`'s isolated venv likely
  > closes by-construction"

The `uv` venv installs the upstream numpy wheel with its full
`__config__.py` intact — making Hypothesis A the leading
candidate.

## What's already in tree to test this

Tier 2's `tier2-uv-pylibs.toml.template` uses a pre-built uv
venv that installs upstream wheels (not Debian's stripped
packages). The smoketest already imports `httpx`, `pyyaml`,
`pendulum`, `numpy` in the same process and the soak ran 80/80
= 100 % on 2026-05-14.

What's MISSING is the `cryptography` step. If we extend the uv
venv to include cryptography and the smoketest to import
cryptography THEN numpy in the same process, and the soak still
passes, that confirms Hypothesis A and closes #17 by venv-
isolation by-construction.

## Test plan

Operator-time steps to verify closure:

  1. Add `cryptography` to the persisted uv venv:

     ```sh
     uv pip install --python ~/.cache/uml-soak-tier2-venv/bin/python \
         cryptography
     ```

  2. Extend `tier2-uv-smoketest.py` with a `test_crypto_numpy_chain`
     function that does:

     ```py
     from cryptography.hazmat.primitives.ciphers import (
         Cipher, algorithms, modes,
     )
     # Quick AES round-trip (reuse tier1-smoketest's pattern).
     # ...
     import numpy as np
     m = np.eye(64)
     np.testing.assert_allclose(np.linalg.inv(m), m)
     ```

     Same-process. No `os.execve` between the two imports.

  3. Add an invocation phase to `tier2-uv-pylibs.toml.template`:

     ```toml
     [[init.phases]]
     name = "crypto-numpy-chain"
     cmd = "/home/mjbommar/.cache/uml-soak-tier2-venv/bin/python \
            {{SOAK_DIR}}/tier2-uv-smoketest.py crypto-numpy-chain"
     expect = "TIER2_OK crypto-numpy-chain"
     timeout_secs = 30
     ```

  4. Run a 1-iter smoke under run-pilot.sh on both backends.
     Expected: PASS on both.

  5. If PASS: close #17 with this memo + a decisions-log entry
     citing the uv venv as the closure mechanism.

  6. If FAIL on uv venv: the pathology is deeper than Hypothesis
     A — re-open with Hypothesis B (hostfs load timing) as the
     next investigation. Build a minimal C reproducer that
     mmap's a shared object then immediately mmap's another, and
     measure timing under UML.

## Why this isn't urgent

  - The Tier 1 workaround (per-library Python process) already
    works in production.
  - The PLAN-2026-05-14 lists #17 as Track C "Polish" and explicitly
    accepts the uv venv as the likely closure (PLAN §2.2 +
    PLAN §3.1).
  - The actual investigation requires UML boot time + operator
    venv mutation, both of which are bounded but not in the
    critical path for any other deliverable.

## Why this matters

  - If #17 reproduces under uv-venv too, it's a genuine UML
    host-environment bug (Hypothesis B territory) and warrants a
    targeted investigation — possibly involving hostfs-lazy-load
    semantics, which is broader than Phase J.
  - Closing #17 lets us write the merge gate's "Tier 1 +
    cryptography + numpy in one process" assertion, which would
    be a stronger statement than today's "each in a fresh
    process."

## Filing

Task #17 remains pending in the project task tree. This memo is
the investigation seed; closure happens in operator-time when
steps 1-4 above are run.

Re-reads:

  - `tools/testing/selftests/um/soak/tier1-smoketest.py:32-115`
    (the per-library check functions + the all-in-one trip
    comment).
  - `tools/testing/selftests/um/soak/tier2-uv-smoketest.py`
    (the venv-based version that currently does NOT import
    cryptography).
  - `Documentation/virt/uml/redesign/06-sequencing/PLAN-2026-05-14.md`
    §2.2 row "#17 numpy" + §3.1 Tier 2 wiring.
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
    phase-J-design-2026-05-07.md` §3.1 (Tier 1 Python
    smoketest design).
