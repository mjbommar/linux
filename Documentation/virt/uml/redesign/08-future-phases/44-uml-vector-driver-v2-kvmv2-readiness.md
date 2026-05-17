# UML Vector Driver V2 KVM-V2 Readiness Checkpoint

Status: evidence checkpoint, not replacement approval.

This note updates the old vector2 KVM-v2 blocker.  The previous live
attempt used the vector2-only runtime kernel
`/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux`,
whose `.config` had:

```
# CONFIG_UM_BACKEND_KVM_V2 is not set
CONFIG_UM_BACKEND_DYNAMIC=y
CONFIG_UML_NET_VECTOR_V2=y
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
```

That kernel was valid for seccomp vector2 work but could not be used as
evidence for KVM-v2 vector2 readiness.

## Runtime Build

A separate out-of-tree runtime was built from the vector2-only config
with KVM-v2 enabled:

```
build=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2
cp /home/mjbommar/projects/personal/.build/um-vector-r1-v2only/.config \
   "$build/.config"
scripts/config --file "$build/.config" \
  -e UM_BACKEND_KVM_V2 \
  -e UML_NET_VECTOR_V2 \
  -e UML_NET_VECTOR_V2_SANDBOX \
  -e UM_BACKEND_DYNAMIC \
  -d UM_BACKEND_KVM_V2_KUNIT \
  -d UM_BACKEND_KVM_V2_STATE_TRACE
make ARCH=um O="$build" olddefconfig
make -j$(nproc) ARCH=um O="$build" linux
```

The resulting config has:

```
# CONFIG_SMP is not set
CONFIG_UM_BACKEND_SECCOMP=y
CONFIG_UM_BACKEND_KVM_V2=y
CONFIG_UM_BACKEND_KVM_V2_GADGET=y
CONFIG_UM_BACKEND_DYNAMIC=y
CONFIG_UML_NET_VECTOR_V2=y
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
```

`cargo build --manifest-path tools/uml/uml-launcher/Cargo.toml --bin
umlctl` also passed before the live gates.

## Gates

### No-Network Readiness

The same minimal `network.mode = "none"` Umlfile passed with both
backends on the KVM-v2-enabled kernel:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux \
  umlctl gate loop -f /tmp/uml-kvmv2-baseline-seccomp.toml \
    -W 1 -M 1 --timeout 90 \
    --pass-marker KVMV2_BASELINE_OK \
    --out /tmp/um-kvmv2-baseline-seccomp

PASS=1/1 FAIL=0 TIMEOUT=0
```

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux \
  umlctl gate loop -f /tmp/uml-kvmv2-baseline-kvmv2.toml \
    -W 1 -M 1 --timeout 90 \
    --pass-marker KVMV2_BASELINE_OK \
    --out /tmp/um-kvmv2-baseline-kvmv2

PASS=1/1 FAIL=0 TIMEOUT=0
```

This closes the specific "no KVM-v2 boot evidence because the selected
runtime kernel did not build KVM-v2" problem.  It does not close KVM-v2
application workload readiness.

### Vector2 FD Handoff Smoke

`vector2-fd-handoff.toml` with `[kernel].backend = "kvm-v2"` passed:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux \
  umlctl gate loop -f /tmp/vector2-fd-handoff-kvmv2.toml \
    -W 1 -M 1 --timeout 120 \
    --pass-marker VECTOR2_FD_HANDOFF_OK \
    --out /tmp/um-vector2-fd-handoff-kvmv2

PASS=1/1 FAIL=0 TIMEOUT=0
```

The copied log records:

- `um: backend = kvm-v2 (contract v2)`;
- `uml-vector2: registered netdev vec2.0 for vec2.0`;
- vector2 fd handoff through inherited fd 200;
- `VECTOR2_FD_HANDOFF_OK`;
- no lingering `v2fd0` TAP and no running UML instance.

The run also emits KVM-v2 diagnostics:

- five `um: kvm-v2 BUG_PR[...] high-cr2` lines at boot, matching the
  documented pre-existing init demand-paging diagnostic;
- `KVM_V2_TLB_LAG` with max observed lag 543 in this smoke.

### Tier 3 Django Vector2 Smoke

A generated Tier 3 Django stdlib-shim TOML using vector2 fd handoff,
tap `v2djkv0`, and backend `kvm-v2` passed once:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux \
  umlctl gate loop -f /tmp/tier3-django-v2-kvmv2.toml \
    -W 1 -M 1 --timeout 180 \
    --pass-marker TIER3_OK \
    --out /tmp/um-tier3-django-v2-kvmv2-smoke

PASS=1/1 FAIL=0 TIMEOUT=0
```

The copied log records `SERVER_READY`, `GUEST_CURL ok=100 fail=0`, and
`TIER3_OK`, with clean TAP/process teardown.

However, this run also emitted `KVM_V2_TLB_LAG` above the documented
high-risk threshold, with max observed lag 1734.

### Tier 3 Django 30-Run Attempt

The full KVM-v2 vector2 Django repetition is still not closed:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux \
  umlctl gate loop -f /tmp/tier3-django-v2-kvmv2.toml \
    -W 1 -M 30 --timeout 180 \
    --pass-marker TIER3_OK \
    --out /tmp/um-tier3-django-v2-kvmv2-30

PASS=29/30 FAIL=1 TIMEOUT=0
```

Iteration 25 failed before the HTTP gate:

```
/tmp/.../init.sh: line 90:    35 Aborted \
    python3 /tmp/tier3-django-app.py >/tmp/django.log 2>&1
SERVER_FAIL
Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000100
```

The panic is a secondary consequence of the init script exiting after
`SERVER_FAIL`.  The useful signal is the guest `python3` abort before the
server prints `SERVER_READY`.

The 30-run attempt also repeatedly emitted `KVM_V2_TLB_LAG` values above
1000.  Per-run max lag included values such as 2461, 2429, 2114, and
many runs around 1730-1750.  The failed iteration's max lag was 827, so
the lag diagnostic is correlated with this workload class but is not by
itself the exact failure predicate for iteration 25.

The TAP `v2djkv0` was absent after the failed 30-run gate, and no running
UML instance remained.

### Seccomp Control

The same generated Django/vector2 shape passed a short seccomp control
on the KVM-v2-enabled kernel:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux \
  umlctl gate loop -f /tmp/tier3-django-v2-seccomp-control.toml \
    -W 1 -M 3 --timeout 180 \
    --pass-marker TIER3_OK \
    --out /tmp/um-tier3-django-v2-seccomp-control

PASS=3/3 FAIL=0 TIMEOUT=0
```

Each seccomp control run reached `SERVER_READY`,
`GUEST_CURL ok=100 fail=0`, and `TIER3_OK`, with no lingering
`v2djsc0` TAP.

## Interpretation

The old "KVM-v2 no-network readiness" diagnosis was too coarse.  With a
properly configured runtime kernel, KVM-v2 boots, vector2 registers,
vector2 fd handoff works, and a single Django stdlib-shim run can pass.

The remaining blocker is narrower and more serious: KVM-v2 is not yet
stable enough for the vector2 Tier 3 Django 30/30 proof.  The failure is
a guest Python abort during server startup under KVM-v2, while the same
vector2 Django shape passes under seccomp.  The vector2 driver should not
claim KVM-v2 Tier 3 readiness until this backend abort/TLB-lag class is
fixed or otherwise explained with repeatable clean evidence.

## Next Work

1. Re-run the 30/30 gate with KVM-v2 state trace enabled or the existing
   TLB-lag diagnostics promoted into a per-run summary.
2. Determine whether high `KVM_V2_TLB_LAG` is causal, symptomatic, or
   unrelated to the Python abort.
3. After the KVM-v2 backend fix, rerun the vector2 Django 30/30 gate and
   only then update the completion audit from partial to done.

## Diagnostic Follow-Up

After the failed 30-run gate, the Tier 3 Django and FastAPI templates
were updated to dump `/tmp/django.log` or `/tmp/fastapi.log` between
`*_LOG_BEGIN` and `*_LOG_END` before printing `SERVER_FAIL`.  This does
not fix the KVM-v2 abort, but it ensures the next failing iteration
preserves userspace server stderr in the copied `run-*.log`.

Validation for the diagnostic change:

- generated Django and FastAPI vector2 fd handoff dry-runs contained the
  new log markers and rendered `backend=force=seccomp` plus
  `vec2.0:transport=fd,mode=fd,fd=200,depth=128`;
- a Django vector2 seccomp live check passed `PASS=1/1 FAIL=0 TIMEOUT=0`
  with `TIER3_OK`.
