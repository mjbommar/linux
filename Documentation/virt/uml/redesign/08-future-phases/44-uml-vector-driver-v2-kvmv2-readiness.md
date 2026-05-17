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

### Tier 3 Django 30-Run Attempts

The first full KVM-v2 vector2 Django repetition was not clean:

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

After the Tier 3 templates were updated to dump server stderr before
`SERVER_FAIL`, the same KVM-v2 vector2 Django shape was rerun with tap
`v2djkv1`:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux \
  umlctl gate loop -f /tmp/tier3-django-v2-kvmv2-diag.toml \
    -W 1 -M 30 --timeout 180 \
    --pass-marker TIER3_OK \
    --out /tmp/um-tier3-django-v2-kvmv2-diag-30

PASS=30/30 FAIL=0 TIMEOUT=0
```

The copied logs contained:

```
SERVER_READY=30
GUEST_CURL ok=100 fail=0: 30
TIER3_OK=30
bad_signatures=0
```

`v2djkv1` was absent after teardown, and no UML instance remained.  The
KVM-v2 logs still emitted the documented boot-time `BUG_PR` diagnostics
and high `KVM_V2_TLB_LAG`; max observed lag in the clean rerun was 2117.

This provides one successful KVM-v2 vector2 Django 30/30 gate, but it
does not fully close the KVM-v2 workload item because the immediately
preceding 29/30 failure is still unexplained and the high TLB-lag
diagnostics persist.

### Tier 3 Django 60-Run Flake Sample

A longer diagnostic sample used the same KVM-v2 vector2 Django shape
with tap `v2djkv2`:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux \
  umlctl gate loop -f /tmp/tier3-django-v2-kvmv2-diag-60.toml \
    -W 1 -M 60 --timeout 180 \
    --pass-marker TIER3_OK \
    --out /tmp/um-tier3-django-v2-kvmv2-diag-60

PASS=57/60 FAIL=2 TIMEOUT=1
```

The 57 passing runs reached all expected markers:

```
SERVER_READY=57
GUEST_CURL ok=100 fail=0: 57
TIER3_OK=57
SERVER_FAIL=2
```

Failure details:

- iteration 16 timed out during `django-up`; the copied log reached
  `[umlctl phase] django-up START` but never printed `SERVER_READY` or
  `SERVER_FAIL`;
- iteration 18 printed `Segmentation fault` for the guest `python3`
  server process, then dumped an empty `DJANGO_LOG_BEGIN` /
  `DJANGO_LOG_END` block before `SERVER_FAIL`;
- iteration 43 printed `Aborted` for the guest `python3` server process;
  the dumped server log contained
  `Fatal Python error: _PyEval_EvalFrameDefault: Executing a cache.`
  while importing `http` / `enum.py`.

The TAP `v2djkv2` was absent after teardown, and no UML instance
remained.  `uml-vector2: registered netdev vec2.0 for vec2.0` appeared
in the failing logs before the guest Python failure, so this sample
continues to point at KVM-v2 userspace execution rather than vector2
registration or fd handoff.

`KVM_V2_TLB_LAG` remains noisy but not a precise failure predicate in
this sample: 57/60 logs had max lag above 1000, with max observed lag
2172.  The three non-passing iterations had lower max lag values
(`675`, `732`, and `741`), so TLB lag remains a workload risk signal but
not the direct per-iteration classifier.

### Tier 3 Django State-Trace Capture

The next diagnostic checkpoint built a separate KVM-v2/vector2 runtime
with the state trace ring enabled:

```
/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux

# CONFIG_SMP is not set
CONFIG_UM_BACKEND_KVM_V2=y
CONFIG_UM_BACKEND_KVM_V2_GADGET=y
CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE=y
CONFIG_DEBUG_FS=y
CONFIG_UML_NET_VECTOR_V2=y
CONFIG_UML_NET_VECTOR_V2_SANDBOX=y
```

The Tier 3 Django and FastAPI templates now attempt a no-op-unless-
available KVM-v2 trace dump on `SERVER_FAIL`: they mount debugfs if
needed, write `1` to `/sys/kernel/debug/um_kvm_v2_trace/dump` when the
file exists, and bracket the copied output with
`KVM_V2_TRACE_DUMP_BEGIN` / `KVM_V2_TRACE_DUMP_END`.

A rendered Django/vector2 dry-run confirmed that the trace dump function
coexists with the normal fd handoff:

```
backend=force=kvm-v2
vec2.0:transport=fd,mode=fd,fd=200,depth=128
mode=tap driver=vector2 guest_dev=vec2.0 host_tap=v2trace0 transport=fd
```

A one-shot trace-enabled smoke passed:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux \
  umlctl gate loop -f /tmp/tier3-django-v2-kvmv2-trace-smoke.toml \
    -W 1 -M 1 --timeout 180 \
    --pass-marker TIER3_OK \
    --out /tmp/um-tier3-django-v2-kvmv2-trace-smoke

PASS=1/1 FAIL=0 TIMEOUT=0
```

The smoke log contained:

```
um: kvm-v2 state-trace: ENABLED at boot via kvm_v2_trace_enable
um: kvm-v2 state-trace debugfs at /sys/kernel/debug/um_kvm_v2_trace/
uml-vector2: registered netdev vec2.0 for vec2.0
SERVER_READY
GUEST_CURL ok=100 fail=0
TIER3_OK
```

The trace-enabled 60-run used the same Django/vector2 shape with
`append = ["kvm_v2_trace_enable"]`:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux \
  umlctl gate loop -f /tmp/tier3-django-v2-kvmv2-trace-60.toml \
    -W 1 -M 60 --timeout 180 \
    --pass-marker TIER3_OK \
    --out /tmp/um-tier3-django-v2-kvmv2-trace-60

PASS=59/60 FAIL=1 TIMEOUT=0
```

Marker counts:

```
SERVER_READY=59
TIER3_OK=59
SERVER_FAIL=1
KVM_V2_TRACE_DUMP_BEGIN=1
Fatal Python error=1
```

Iteration 21 failed before `SERVER_READY`:

```
/tmp/.../init.sh: line 106: 35 Aborted python3 /tmp/tier3-django-app.py
DJANGO_LOG_BEGIN
Fatal Python error: _PyEval_EvalFrameDefault: Executing a cache.
...
  File "/usr/lib/python3.14/re/_parser.py", line 514 in _parse
  File "/usr/lib/python3.14/re/__init__.py", line 315 in <module>
  File "/usr/lib/python3.14/email/utils.py", line 26 in <module>
  File "/usr/lib/python3.14/http/server.py", line 94 in <module>
  File "/tmp/tier3-django-app.py", line 1 in <module>
DJANGO_LOG_END
KVM_V2_TRACE_DUMP_BEGIN
KVMV2T_DUMP_BEGIN reason=debugfs entries=5140
...
KVMV2T_DUMP_END entries=5140
KVM_V2_TRACE_DUMP_END
SERVER_FAIL
```

The TAP `v2djtr1` was absent after teardown, and no matching UML
instance remained.  The failed iteration's max `KVM_V2_TLB_LAG` was
943, while the maximum across the 60-run was 2410 and 59/60 logs had
max lag above 1000.  As with the earlier flake sample, TLB lag is not a
per-iteration classifier; the new value is that the Python abort is now
paired with a captured KVM-v2 state-ring dump.

### Trace Summary Helper

The copied `KVMV2T-*` trace lines are intentionally split into tagged
sections so printk interleaving cannot corrupt a complete record.  The
helper below reassembles those sections by `(cpu, seq)` and summarizes
the failure markers, TLB-lag lines, operation counts, port counts, exit
reasons, pids, maximum observed mm-generation lag, fatal Python context,
and final trace entries:

```
tools/testing/selftests/um/soak/kvmv2-trace-summary.py \
  /tmp/um-tier3-django-v2-kvmv2-trace-60/p0_default/w0/run-21.log \
  --limit 8
```

The first summary of the failing iteration produced:

```
entries: parsed=5140 complete=5140
seq_range: 2468741..2473880
dump_begin: reason=debugfs declared_entries=5140
dump_end_entries: 5140,5140
markers: fatal_python=1 python_abort=1 server_fail=1 trace_dump_begin=1 trace_dump_end=1
tlb_lag: count=30 max=943
tlb_lag_by_pid: 1=28 35=2
ops top: POST_FPU_INSTALL=506 POST_IST_RESTORE=506 POST_KVM_RUN=506 POST_LOAD_SREGS=506 POST_TLB_SYNC=506 PRE_KVM_RUN=506 VCPU_RUN_ENTRY=506 VCPU_RUN_EXIT=504
ports: 0xf6=1832 0xf4=1269 0xfd=1127 0x0=912
exit_reasons: 2=4228 0=912
pids: 161=4004 1=1136
max_mm_lag: lag=4822 cpu=0 seq=2473317 pid=1 op=HANDLE_SYSCALL_POST mmgen=4947 vlast=125
last_entry: seq=2473880 pid=1 op=HANDLE_SYSCALL_PRE port=0xf4 rip=ffffe000000000f5 cr2=0x800e90be0e
```

Neighboring passing logs do not contain trace dumps because the template
only dumps the ring on `SERVER_FAIL`, but the helper still captures their
markers and TLB-lag lines:

```
run-20.log: entries=0 server_ready=1 tier3_ok=1 tlb_lag_count=30 tlb_lag_max=1745
run-22.log: entries=0 server_ready=1 tier3_ok=1 tlb_lag_count=30 tlb_lag_max=1754
```

This does not prove root cause.  It does prove that the captured failure
dump is structurally complete and analyzable, that high `KVM_V2_TLB_LAG`
is still not a direct pass/fail classifier, and that the next backend
debugging pass can focus on the pid 161/1 transition history and the
final syscall/page-fault sequence rather than vector2 registration or fd
handoff.

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

The remaining blocker is narrower than the original readiness failure:
KVM-v2 can boot and can produce a clean vector2 Django 30/30 run, but a
longer 60-run sample still produced guest Python segfault/abort failures
and one startup timeout under the same workload shape.  The vector2
driver should not claim final KVM-v2 Tier 3 readiness until this backend
userspace-execution flake class is fixed or otherwise explained with
repeatable clean evidence.  The trace-enabled rerun confirms the same
Python abort class and now preserves a complete KVM-v2 state-ring dump
for backend analysis.

## Next Work

1. Use the parsed iteration 21 `KVMV2T` dump to inspect the pid 161/1
   transition history, final syscall/page-fault sequence, and the large
   `mmgen - vlast` gap around `HANDLE_SYSCALL_POST`.
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

After the trace-enabled run, the same templates also dump the KVM-v2
state trace ring on `SERVER_FAIL` when a trace-capable kernel exposes
`/sys/kernel/debug/um_kvm_v2_trace/dump`.  The behavior is a no-op for
normal kernels without `CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE`.

Validation for the diagnostic change:

- generated Django and FastAPI vector2 fd handoff dry-runs contained the
  new log markers and rendered `backend=force=seccomp` plus
  `vec2.0:transport=fd,mode=fd,fd=200,depth=128`;
- a Django vector2 seccomp live check passed `PASS=1/1 FAIL=0 TIMEOUT=0`
  with `TIER3_OK`.
- a Django vector2 KVM-v2 trace smoke passed `PASS=1/1 FAIL=0
  TIMEOUT=0` with the state trace enabled at boot;
- a Django vector2 KVM-v2 trace 60-run captured a failing iteration with
  `KVMV2T_DUMP_BEGIN reason=debugfs entries=5140`;
- `tools/testing/selftests/um/soak/kvmv2-trace-summary.py` reassembled
  that dump into 5140 parsed / 5140 complete entries and validated JSON
  output for follow-on tooling.
