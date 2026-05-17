# Tier 2 `uv-pylibs` landed (2026-05-14)

## What

Phase J Track A §3.1 — Tier 2 pip+pytest workload. Spec at
`phase-J-design-2026-05-07.md` §3.2.

## What landed

  - `tier2-uv-smoketest.py` (~140 LoC): deterministic exercise of
    httpx + pyyaml + pendulum + numpy. Per-library and all-in-one
    modes.
  - `tier2-uv-pylibs.toml.template`: invokes the pre-built venv's
    python on the smoketest. 120 s per-iter timeout.
  - README §"Tier 2 bootstrap": operator one-time setup —
    `curl … | sh` install + `uv venv` + `uv pip install httpx
    pyyaml pendulum numpy`.

## Pivot from the design memo

The original §3.2 spec called for `uv run --with <pkg>` per
invocation (ephemeral venv per call). Empirically this was
unreliable in the UML guest: `uv`'s offline resolver walked the
simple index back through historical versions without finding
cached wheels, even with `UV_OFFLINE=1 UV_CACHE_DIR=…` pointed
at a hostfs-shared warm cache. The walk-back behaviour suggests
`uv`'s cached simple-index pages tell the resolver "versions
0.25.2 … 0.28.1 exist" but the actual wheel-cache lookup misses
or fails to match. Filing as a follow-up if anyone wants to debug
`uv` itself; for our purposes the bypass is fine.

Pivot: build a **persistent venv** once via `uv venv` +
`uv pip install`, point the template at `<venv>/bin/python`
directly. No resolver at run time. ~70 MB on disk, hostfs-
visible to the guest.

The memo's `--with X` per invocation rationale was about
isolation (each call fresh venv, no chain contamination). That
rationale still holds with the persistent venv approach because
the venv is at a different prefix from the host's system python3
— the system's `cryptography → numpy` import-chain pathology (#17)
cannot trip because the venv doesn't ship cryptography.

## Verification

Daemon smoke under the post-T57-Phase-A kernel (HEAD
`a1e17e22cbad`):

```
budget 180 s, W=1, M=2, workload=tier2-uv-pylibs, both backends
-> 20 rotations × 2 backends × 2 iters = 80 / 80 PASS = 100.00 %
-> Phase elapsed 2-3 s per phase (boot + 4-lib smoke)
```

All four libraries report OK on both kvm-v2 and seccomp.

## #17 status

Tier 1's numpy-after-cryptography bug remains open — Tier 2's
sidestep is by-construction (no cryptography in the venv) but
doesn't prove the root cause. The narrower test "import
cryptography then import numpy in a single Python process under
the venv's interpreter, both packages from the venv" would
disambiguate. Tracked as follow-up; not blocking Phase J.

## Next

Plan §8 Month 1 Week 1 done for Tier 2. Next:

  - Tier 3 design resolution (TAP IP question, memo §3.3).
  - LTP curation walk (memo §4).
  - Start #168 snapshot port (Track B).

## Commit

Pending — will commit + push after this diary entry.
