# UML vector driver v2 R7 seccomp 30/30 checkpoint

**Status:** R7 partial - seccomp Tier 3 repetition proven.
**Date:** 2026-05-17.

This note records the first 30-iteration Tier 3 Django stdlib-shim
gate for vector v2 TAP under the seccomp backend.  It does not close
R7 because kvm-v2 readiness still fails before vector2-specific
validation is meaningful.

## Command

```text
tools/uml/uml-launcher/target/debug/umlctl gate loop \
  -f /tmp/um-vector-r7-dry/_tier3-django-v2-seccomp-w0.toml \
  --network-driver vector2 \
  -W 1 -M 30 --timeout 180 \
  --out /tmp/um-vector-r9-seccomp-v2-m30
```

## Result

```text
tier3_seccomp_v2_m30_rc=0
==> default PASS=30/30 FAIL=0 TIMEOUT=0 rate=100.0% (Wilson 95% CI [88.6%, 100.0%]) elapsed=234s
```

Every worker iteration log contained:

```text
SERVER_READY
TIER3_OK
REPRO_DONE rc=0
```

The post-run TAP check confirmed teardown:

```text
tap_absent_rc=1
Device "soak-tap0" does not exist.
```

No checked log line matched:

```text
panic
BUG:
Oops
Kernel mode fault
general protection fault
NULL pointer
```

## Meaning

This satisfies the seccomp side of the R7 30/30 Tier 3 Django gate for
vector2 TAP.  The R7 gate is still incomplete because:

- kvm-v2 no-network readiness currently fails before Linux boot output;
- the kvm-v2 vector2 Tier 3 30/30 gate cannot be interpreted until
  that baseline backend issue is fixed;
- longer FastAPI/uvicorn repetition and long-soak proof remain open.
  A later short seccomp FastAPI/uvicorn smoke is recorded in
  `34-uml-vector-driver-v2-fastapi-uvicorn-smoke.md`.
