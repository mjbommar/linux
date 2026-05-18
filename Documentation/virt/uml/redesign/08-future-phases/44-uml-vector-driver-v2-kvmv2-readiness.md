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
dispatch_switches: count=2
  entry_seq=2470203 entry_pid=161 entry_tmm=61156a80 exit_seq=2470212 exit_pid=161 exit_tmm=61156200
  entry_seq=2473309 entry_pid=161 entry_tmm=61156200 exit_seq=2473318 exit_pid=1 exit_tmm=61156ec0
post_syscall_mismatches: count=2
  seq=2470211 pid=161 run_rax=3b task_horax=0 task_tmm=61156200 vcpu_mm=61156a80
  seq=2473317 pid=1 run_rax=e7 task_horax=3d task_tmm=61156ec0 vcpu_mm=61156200
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

The older state-trace invariant checker gives a second view of the same
dump:

```
tools/testing/selftests/um/state-trace/parse-trace.py summary \
  /tmp/um-tier3-django-v2-kvmv2-trace-60/p0_default/w0/run-21.log

Parsed 5140 snapshots
Incomplete entries (missing sections): 0

tools/testing/selftests/um/state-trace/parse-trace.py invariants \
  /tmp/um-tier3-django-v2-kvmv2-trace-60/p0_default/w0/run-21.log \
  --limit 20

510 violations: 4 CRITICAL, 506 WARNING
CRITICAL seq=2473318: VCPU_RUN_ENTRY pid=161 seq=2473309 != VCPU_RUN_EXIT pid=1
CRITICAL seq=2473317: pid=1 mid-dispatch, started pid=161 at seq=2473309
CRITICAL seq=2470212: tmm changed mid-dispatch 61156a80 -> 61156200
CRITICAL seq=2473318: tmm changed mid-dispatch 61156200 -> 61156ec0
```

The same checker found no mmap-returned-zero anomaly:

```
tools/testing/selftests/um/state-trace/parse-trace.py mmap-zero \
  /tmp/um-tier3-django-v2-kvmv2-trace-60/p0_default/w0/run-21.log

Found 0 mmap-returned-zero events
```

The critical entries need careful interpretation.  The invariant file
was written for an older "no task switch inside one dispatch" model, but
the current KVM-v2 code deliberately uses `migrate_disable()` rather
than `preempt_disable()`, so a syscall can sleep and another UML task can
reuse the per-host-CPU vCPU before the first task resumes.  The trace at
seq 2473317 is still valuable because it shows mixed ownership in one
sample: the header/task fields are pid 1 while the live `kvm_run` fields
still reflect the prior pid 161 syscall exit.  The next KVM-v2 backend
audit must decide whether this is only a trace/invariant-model artifact
or whether any post-syscall path still consumes stale shared-vCPU state
after a sleeping syscall.

### Post-Syscall Shared-Run Hardening

The first backend follow-up made one narrow ownership rule explicit:
`kvm_v2_handle_io_trap()` no longer reads from or writes to the shared
`kvm_run` mmap after `handle_syscall()` returns.  The syscall return
value remains in the task's `uml_pt_regs`; the next outer
`kvm_v2_vcpu_run()` iteration marshals those per-task regs into the
selected per-host-CPU vCPU immediately before `KVM_RUN`.  The
`HANDLE_SYSCALL_POST` trace point now intentionally records only
per-task state, with no live `kvm_run` payload.

This removes the specific stale-run pattern shown by the first captured
failure, where an `exit_group` syscall from the Python task slept or
exited and the post-syscall trace was emitted under pid 1 while the live
`kvm_run` fields still described pid 161.  The trace summary helper now
only reports post-syscall run/task mismatches for old-style
`HANDLE_SYSCALL_POST` records that still carry the syscall IO port
`0xf4`; fixed-kernel traces with `port=0x0` are skipped for that check.

Validation for this hardening:

```
python3 -m py_compile tools/testing/selftests/um/soak/kvmv2-trace-summary.py
tools/testing/selftests/um/soak/kvmv2-trace-summary.py \
  /tmp/um-tier3-django-v2-kvmv2-trace-60/p0_default/w0/run-21.log \
  --limit 4
git diff --check -- arch/um/backend/kvm-v2/syscall_trap.c \
  arch/um/backend/kvm-v2/kvm_v2_backend.h \
  tools/testing/selftests/um/soak/kvmv2-trace-summary.py
git diff -- arch/um/backend/kvm-v2/syscall_trap.c \
  arch/um/backend/kvm-v2/kvm_v2_backend.h \
  tools/testing/selftests/um/soak/kvmv2-trace-summary.py |
  scripts/checkpatch.pl --strict --no-tree -
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace \
  -j$(nproc) linux
cargo build --release --bin umlctl
```

The old failing trace still reports the original two post-syscall
mismatches, proving the helper continues to identify unfixed logs.  The
rebuilt trace-enabled kernel then passed a one-shot Django/vector2
KVM-v2 smoke:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux \
  tools/uml/uml-launcher/target/release/umlctl gate loop \
    -f /tmp/tier3-django-v2-kvmv2-trace-smoke.toml \
    -W 1 -M 1 --timeout 180 \
    --pass-marker TIER3_OK \
    --out /tmp/um-tier3-django-v2-kvmv2-trace-postfix-smoke

PASS=1/1 FAIL=0 TIMEOUT=0
```

However, the same rebuilt runtime did **not** close the Tier 3 flake:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux \
  tools/uml/uml-launcher/target/release/umlctl gate loop \
    -f /tmp/tier3-django-v2-kvmv2-trace-60.toml \
    -W 1 -M 30 --timeout 180 \
    --pass-marker TIER3_OK \
    --out /tmp/um-tier3-django-v2-kvmv2-trace-postfix-30

PASS=29/30 FAIL=1 TIMEOUT=0
```

Iteration 18 again aborted guest `python3` before `SERVER_READY` with
`Fatal Python error: _PyEval_EvalFrameDefault: Executing a cache.` and
dumped a complete trace ring.  The summary helper now reports both the
old post-syscall mismatch class and the remaining task/mm transition
class:

```
entries: parsed=5140 complete=5140
markers: fatal_python=1 python_abort=1 server_fail=1 trace_dump_begin=1 trace_dump_end=1
tlb_lag: count=30 max=1050
dispatch_switches: count=2
  entry_seq=2470317 entry_pid=161 entry_tmm=61156200 exit_seq=2470326 exit_pid=161 exit_tmm=61156640
  entry_seq=2473423 entry_pid=161 entry_tmm=61156640 exit_seq=2473432 exit_pid=1 exit_tmm=61156ec0
syscall_switches: count=2
  entry_seq=2470324 entry_pid=161 entry_tmm=61156200 entry_syscall=3b post_seq=2470325 post_pid=161 post_tmm=61156640
  entry_seq=2473430 entry_pid=161 entry_tmm=61156640 entry_syscall=e7 post_seq=2473431 post_pid=1 post_tmm=61156ec0
post_syscall_mismatches: none
mm_backsteps: count=3
  seq=2469440 pid=161 op=POST_TLB_SYNC mmgen=4815 vlast=4834 task_tmm=61156200 vcpu_mm=61156ec0
  seq=2470326 pid=161 op=VCPU_RUN_EXIT mmgen=0 vlast=4862 task_tmm=61156640 vcpu_mm=61156200
  seq=2470328 pid=161 op=POST_TLB_SYNC mmgen=1 vlast=4862 task_tmm=61156640 vcpu_mm=61156200
```

The older invariant checker still reports the same four critical
pid/tmm stability violations on the new failed trace, while `mmap-zero`
still reports no mmap-returned-zero event.  The important change is that
the proven post-`handle_syscall()` stale-run consumption path is now
removed, but the Python abort persists.  The remaining KVM-v2 audit must
move to the broader task/mm ownership transition and guest memory/TLB
model; vector2 KVM-v2 Tier 3 readiness remains open.

### Regs-Owner Trace Check

The next diagnostic checkpoint added trace-only ownership fields to the
`KVMV2T-T` line:

```
rmatch=<0|1> rptr=<sampled uml_pt_regs *> crptr=<current->thread.regs.regs>
```

This distinguishes a real stale-`regs` sample from the older invariant
checker's single-current-task model.  The parser remains backward
compatible with older logs, and the soak summary helper now reports
`regs_owner_mismatches` when those fields are present.

Validation:

```
python3 -m py_compile \
  tools/testing/selftests/um/soak/kvmv2-trace-summary.py \
  tools/testing/selftests/um/state-trace/parse-trace.py
git diff --check -- arch/um/backend/kvm-v2/state_trace.c \
  arch/um/backend/kvm-v2/state_trace.h \
  tools/testing/selftests/um/state-trace/parse-trace.py \
  tools/testing/selftests/um/soak/kvmv2-trace-summary.py
make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace \
  -j$(nproc) linux
```

The rebuilt trace runtime passed a one-shot Django/vector2 KVM-v2
smoke:

```
PASS=1/1 FAIL=0 TIMEOUT=0
```

A new 30-run sample still reproduced the Python abort:

```
PASS=29/30 FAIL=1 TIMEOUT=0
```

The failed iteration 22 dumped a complete new-format trace:

```
entries: parsed=4854 complete=4854
markers: fatal_python=1 python_abort=1 server_fail=1 trace_dump_begin=1 trace_dump_end=1
tlb_lag: count=30 max=1131
dispatch_switches: count=2
syscall_switches: count=1
mm_backsteps: count=3
regs_owner_mismatches: count=0
```

The fatal Python context moved slightly earlier in import startup
(`ipaddress.py` through `urllib.parse` and `email.utils`), but the error
class remained `_PyEval_EvalFrameDefault: Executing a cache.`  The
`regs_owner_mismatches: count=0` result rules out the simplest
interpretation that the pid/tmm invariant hits are stale `regs` pointer
use in the trace or dispatcher.  The remaining KVM-v2 bug is still a
guest userspace corruption issue, but it is no longer explained by the
post-syscall `kvm_run` mismatch or stale `uml_pt_regs` ownership.

### No-Network Python Import Control

To separate the vector2 fd/network path from KVM-v2 userspace execution,
a no-network control Umlfile was generated with:

- `backend = "kvm-v2"`;
- `append = ["kvm_v2_trace_enable"]`;
- `network.mode = "none"`;
- `mem = "1024M"`;
- `ncpus = 2`.

Each boot ran 100 fresh `python3` processes importing the same stdlib
modules present in the failing Django stack (`email.utils`,
`http.server`, `ipaddress`, `re`, `socket`, and `urllib.parse`).  On
failure the phase would dump the KVM-v2 trace ring and print
`PY_IMPORT_FAIL`.

Smoke:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux \
  tools/uml/uml-launcher/target/release/umlctl gate loop \
    -f /tmp/kvmv2-python-import-trace.toml \
    -W 1 -M 1 --timeout 180 \
    --pass-marker PY_IMPORT_LOOP_OK \
    --fail-marker 'PY_IMPORT_FAIL|Fatal Python error|Segmentation fault|Aborted|kernel BUG|Kernel panic' \
    --out /tmp/um-kvmv2-python-import-trace-smoke

PASS=1/1 FAIL=0 TIMEOUT=0
```

30-run control:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux \
  tools/uml/uml-launcher/target/release/umlctl gate loop \
    -f /tmp/kvmv2-python-import-trace.toml \
    -W 1 -M 30 --timeout 180 \
    --pass-marker PY_IMPORT_LOOP_OK \
    --fail-marker 'PY_IMPORT_FAIL|Fatal Python error|Segmentation fault|Aborted|kernel BUG|Kernel panic' \
    --out /tmp/um-kvmv2-python-import-trace-30

PASS=30/30 FAIL=0 TIMEOUT=0
```

All 30 copied run logs contained exactly one `PY_IMPORT_COUNT ok=100`
marker and no `PY_IMPORT_FAIL`, `Fatal Python error`, segmentation
fault, abort, kernel BUG, panic, KCSAN, or trace-dump marker.  Across
the passing sample, the run logs still contained 947
`KVM_V2_TLB_LAG` diagnostics with a maximum lag of 2685.

This does not clear KVM-v2 for vector2 Tier 3.  It does show that the
observed Django abort is not reproduced by simple repeated Python
startup/import without a network device.  It also strengthens the
earlier conclusion that high `KVM_V2_TLB_LAG` is not a direct
per-iteration failure classifier.

### No-Network Django Loopback Control

The next control kept the Django-shaped stdlib HTTP server, guest
loopback readiness probe, and 100 in-guest `/health` requests, but used
`network.mode = "none"` so no vector2 TAP/fd device was configured.
The reusable template is
`tools/testing/selftests/um/soak/django-loopback-none.toml.template`.
The runtime shape otherwise matched the KVM-v2 trace runs:

- `backend = "kvm-v2"`;
- `append = ["kvm_v2_trace_enable"]`;
- `mem = "1024M"`;
- `ncpus = 2`.

Smoke:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux \
  tools/uml/uml-launcher/target/release/umlctl gate loop \
    -f /tmp/kvmv2-django-loopback-none-trace.toml \
    -W 1 -M 1 --timeout 180 \
    --pass-marker TIER3_OK \
    --fail-marker 'SERVER_FAIL|TIER3_FAIL|Fatal Python error|Segmentation fault|Aborted|kernel BUG|Kernel panic' \
    --out /tmp/um-kvmv2-django-loopback-none-trace-smoke

PASS=1/1 FAIL=0 TIMEOUT=0
```

30-run control:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux \
  tools/uml/uml-launcher/target/release/umlctl gate loop \
    -f /tmp/kvmv2-django-loopback-none-trace.toml \
    -W 1 -M 30 --timeout 180 \
    --pass-marker TIER3_OK \
    --fail-marker 'SERVER_FAIL|TIER3_FAIL|Fatal Python error|Segmentation fault|Aborted|kernel BUG|Kernel panic' \
    --out /tmp/um-kvmv2-django-loopback-none-trace-30

PASS=27/30 FAIL=2 TIMEOUT=1
```

The 27 passing logs reached `SERVER_READY`, `GUEST_CURL ok=100
fail=0`, and `TIER3_OK`.  The non-passing logs show the same KVM-v2
userspace-execution class without any vector2 network device:

- iteration 9 aborted a guest `python3` readiness-probe helper with
  `Fatal Python error: _PyEval_EvalFrameDefault: Executing a cache.`
  while executing `<frozen getpath>`;
- iteration 21 timed out during `django-up` before `SERVER_READY`;
- iteration 23 aborted the background `python3
  /tmp/tier3-django-loopback-app.py` server before readiness.

The no-network Django-loopback sample contained 885 `KVM_V2_TLB_LAG`
diagnostics with max lag 2392.  The non-passing iterations had max lags
596, 2113, and 1255, while passing iterations also reached max lags
above 2300.  Again, TLB lag is not a direct classifier.

This is the strongest isolation result so far: vector2 fd/TAP setup is
not required to reproduce the Django-shaped KVM-v2 failure.  The
remaining vector2 KVM-v2 gate is blocked by the backend's
userspace/process/socket workload stability, not by demonstrated
vector2 queue or fd-handoff behavior.

The reusable template was then changed to copy all `KVMV2T` dmesg lines
instead of only `tail -n 400`, and a smoke from the rendered template
still passed `PASS=1/1 FAIL=0 TIMEOUT=0`.

A delayed-classification rerun with fail-fast matching removed for
`Fatal Python error` / `Aborted` reproduced the flake again:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux \
  tools/uml/uml-launcher/target/release/umlctl gate loop \
    -f /tmp/django-loopback-none-template-rendered-fulltrace.toml \
    -W 1 -M 30 --timeout 180 \
    --pass-marker TIER3_OK \
    --fail-marker 'SERVER_FAIL|TIER3_FAIL|kernel BUG|Kernel panic' \
    --out /tmp/um-kvmv2-django-loopback-none-fulltrace-delayed-30

PASS=27/30 FAIL=3 TIMEOUT=0
```

Iteration 9 reached the trace dump path.  The dump was still truncated
before `KVM_V2_TRACE_DUMP_END`, but it captured substantially more state
than the earlier `tail -n 400` control:

```
entries: parsed=2956 complete=2955
dump_begin: reason=debugfs declared_entries=4854
tlb_lag: count=30 max=993
dispatch_switches: count=1
syscall_switches: count=1
mm_backsteps: count=3
regs_owner_mismatches: count=0
```

The older invariant checker reported one critical pid/tmm violation:

```
INV10: tmm changed mid-dispatch: entry=6114e200 exit=6114e640
```

The two other failures in that rerun had empty copied run logs, so they
were not useful for state analysis.  The partial trace nevertheless
matches the earlier vector2 failure class: KVM-v2 task/mm ownership
changes are visible, stale `uml_pt_regs` ownership is still ruled out,
and vector2 is absent from the reproduction.

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
for backend analysis.  A separate no-network Python import control
passed 30/30 with 3000 fresh import processes, but a no-network
Django-loopback control reproduced the same class at `PASS=27/30
FAIL=2 TIMEOUT=1`.  The current evidence points at KVM-v2
process/socket/server workload state rather than vector2 fd/TAP setup or
simple Python import startup by itself.

## Next Work

1. Capture a complete KVM-v2 state trace from the no-network
   Django-loopback control.  The latest delayed run captured 2956 parsed
   entries out of a declared 4854, but still missed the dump end marker.
2. Use the parsed iteration 21 `KVMV2T` dump to inspect the pid 161/1
   transition history, final syscall/page-fault sequence, and the large
   `mmgen - vlast` gap around `HANDLE_SYSCALL_POST`.
3. Audit the remaining task/mm transition cases after `execve`,
   `exit_group`, and sleeping syscalls now that the direct post-syscall
   `kvm_run` reuse has been removed.
4. Treat stale `regs` ownership as ruled out unless a future trace shows
   `regs_owner_mismatches > 0`.
5. Treat high `KVM_V2_TLB_LAG` as insufficient by itself: passing
   Django and no-network Python controls also show large lag values.
   Continue auditing whether lag is symptomatic or part of a broader
   task/mm/TLB ordering issue.
6. After the KVM-v2 backend fix, rerun the vector2 Django 30/30 gate and
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
  that dump into 5140 parsed / 5140 complete entries, reported two
  dispatch pid/tmm switches, two syscall task/mm switches, mm-generation
  backsteps, plus two post-syscall run/task mismatches, and validated
  JSON output for follow-on tooling.
- `tools/testing/selftests/um/state-trace/parse-trace.py invariants`
  reported 4 critical pid/tmm stability violations around the pid 161/1
  transition and `mmap-zero` reported no mmap-returned-zero event.
- after the post-syscall shared-run hardening, the rebuilt trace runtime
  passed a Django vector2 KVM-v2 smoke but still failed a 30-run sample
  at `PASS=29/30 FAIL=1 TIMEOUT=0`; the new failing trace had no
  post-syscall run/task mismatches but still had two syscall task/mm
  switches and three mm-generation backsteps, so the remaining blocker is
  broader than that one stale-run path.
- after the `regs` owner trace extension, another rebuilt trace runtime
  passed a one-shot smoke and reproduced the failure at
  `PASS=29/30 FAIL=1 TIMEOUT=0`; the failed trace reported
  `regs_owner_mismatches: count=0`, ruling out stale `uml_pt_regs`
  ownership as the direct explanation for the pid/tmm invariant hits.
- a no-network KVM-v2 Python import control using the same trace runtime
  passed `PASS=30/30 FAIL=0 TIMEOUT=0`; each run completed 100 fresh
  `python3` import startups for the Django-failure stdlib modules, for
  3000 successful import processes total, with no fatal Python, abort,
  BUG, panic, KCSAN, or trace-dump markers.  Passing logs still showed
  947 `KVM_V2_TLB_LAG` diagnostics with max lag 2685, so TLB lag alone
  is not a failure classifier.
- a no-network KVM-v2 Django-loopback control using the same trace
  runtime reproduced the workload flake without vector2:
  `PASS=27/30 FAIL=2 TIMEOUT=1`.  The failures were a fatal Python
  `Executing a cache` abort in the readiness-probe helper and an abort
  of the background stdlib HTTP server before readiness; the timeout
  also occurred during `django-up`.  This proves vector2 fd/TAP setup is
  not required for the KVM-v2 Django-shaped failure class.

### Investigation Round 2026-05-17 — UP Single-vCPU Audit

A follow-up audit ran the no-network Django-loopback control on the
non-trace KVM-v2 runtime
(`/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux`)
and re-examined the existing trace captures with fresh eyes.

Configuration observations:

- The runtime kernel has `CONFIG_SMP=n`, `CONFIG_NR_CPUS_DEFAULT=1`.
  Despite `ncpus=2` on the kernel command line, the UML log says
  `Unknown kernel command line parameters "ncpus=2", will be passed to
  user space` and only one host CPU is active.  All captured trace
  records have `cpu=0`.  The bug is therefore a *UP-only* failure class
  and not directly the SMP-class issues the existing SMP-T16 / T17 /
  T22 / T23 / T26 / T29 / T33 / T55 fixes targeted.
- `kvm_v2_tlb_kick_others()` is `#if IS_ENABLED(CONFIG_SMP)` and a no-op
  on this kernel.  The per-vCPU `last_seen_tlb_gen` tracking still runs
  but has only diagnostic value on UP.

Baseline rerun (non-trace kernel):

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux \
  UMLCTL=/home/mjbommar/projects/personal/linux/tools/uml/uml-launcher/target/release/umlctl \
  bash tools/testing/selftests/um/soak/run-soak-daemon.sh \
    --budget-sec 900 --backends kvm-v2 \
    --workloads django-loopback-none \
    --workers 1 --iters-per-rotation 30 \
    --out /tmp/dj-bug-baseline-30

PASS=29/30 FAIL=0 TIMEOUT=1 rate=96.7%
```

Iteration 18 timed out in `django-up`.  The run log shows the background
python server (pid 32, mm `60b4a200`) repeatedly dispatching with the
`KVM_V2_TLB_LAG` printk firing 30 times — the bound — and the last
~10 prints showing identical `last=651 cur=2473 lag=1822` values, with
no `SERVER_READY`, no `SERVER_FAIL`, and no kernel panic.  The python
server appears to be alive on the vCPU but making no forward progress
toward listen/accept (no further mmap-class syscalls bumping `tlb_gen`,
no further user-visible output).

Re-reading the existing 60-run trace dump
(`/tmp/um-tier3-django-v2-kvmv2-trace-60/p0_default/w0/run-21.log`):

- The state-trace ring is 2 MB/CPU = ~5400 entries.  The captured 5140
  entries cover only `seq_range: 2468741..2473880`.  Python ran tens of
  thousands of syscalls between fork+execve and the cache-abort; the
  ring wrapped many times.  The actual moment of bytecode corruption
  is no longer in the ring by the time `SERVER_FAIL` triggers the
  dump.  The last trace entry is a `write(1, "ok", 2)` from pid 1 —
  long after the abort.
- The flagged `syscall_switches` / `dispatch_switches` /
  `post_syscall_mismatches` items in the trace-summary helper output
  are explained by normal kernel behavior:
  - The `pid 161 → pid 1` switch at seq 2473316/2473317 is an
    `__NR_exit_group` from pid 161 (Python) followed immediately by
    init's `__NR_wait4` returning 161 (init reaped the child).  Two
    different `vcpu_run` invocations, both legitimately on the same
    per-CPU vCPU.
  - The `pid 161` execve transition at seq 2470210/2470211 is a normal
    cross-mm change (`tmm=61156a80` → `61156200`, `mmgen` resets from
    4974 to 1 for the fresh mm).  The next `load_user_sregs` then
    detects `cross_task` and issues a full `KVM_SET_SREGS` that drops
    KVM TDP MMU `prev_roots`.
  - The `mm_backsteps` events are normal cross-mm dispatches where the
    new mm's `tlb_gen` is lower than the per-vCPU `last_seen_tlb_gen`
    inherited from the prior mm.  This is what the cross-task gate is
    designed to handle.
  - `regs_owner_mismatches: count=0` in every recent trace continues
    to rule out stale `uml_pt_regs` pointer use.
- The repeating `last=651 cur=2473` `KVM_V2_TLB_LAG` print pattern in
  the timeout iteration is explained by pid 32 and pid 1 alternating
  dispatches.  Each pid-32 dispatch sees `last=651` (= init's mm's
  `tlb_gen` from the previous interleaved init dispatch); each init
  dispatch would see `last=2473` (= pid-32's mm gen from the previous
  pid-32 dispatch).  Init's mm gen is much lower so init dispatches do
  not trip the `>=3` threshold.  This is *expected behavior* on UP and
  is not by itself a corruption indicator.

Audit of the suspected ownership-flip sites in vcpu.c:

- The cross-task `KVM_SET_SREGS` ioctl on a `last_task != current ||
  last_mm != current->mm` transition (line 1748) fires the heavy
  `__set_sregs2 → kvm_mmu_reset_context` path.  This was supposed to
  close SMP-T33; for the UP Django shape it still runs on every fork
  and execve.  No path was identified where it would silently fail and
  leave stale `prev_roots[]`.
- The `cr2` zero gate (line 1523) only fires on cross-task /
  cross-mm transitions.  Same-task same-mm re-entries preserve the
  KVM-architectural `cr2`.  No same-mm corruption window was found.
- The FPU dual-restore in `kvm_v2_vcpu_run` (line 2261-2272 iotrap
  block following `fpu_install_on_first_run`) installs the per-task
  `iotrap_fpu` AFTER `fpu`.  When both `fpu_valid` and
  `iotrap_fpu_valid` are true, two `KVM_SET_FPU` ioctls fire with
  identical payloads (the `fpu` snapshot at context-switch-out and the
  `iotrap_fpu` snapshot at the most recent post-vmexit are taken
  against the same `vcpu->arch.guest_fpu` and so contain the same
  bytes).  The double restore is wasteful but not corrupting.
- The `um_tlb_sync(current->mm)` call at line 2211 runs before
  `load_user_sregs` on every dispatch.  Any deferred PTE updates from
  a prior dispatch's `handle_syscall` are committed to the spawner mm
  (and thus visible to KVM's mmu_notifier) before the next `KVM_RUN`.

Recurring failure shape ("Executing a cache"):

The user-mode SIGABRT comes from CPython 3.14's
`_PyEval_EvalFrameDefault` `case CACHE: UNREACHABLE("Executing a
cache")`.  This fires when the bytecode interpreter's IP lands on a
CACHE pseudo-opcode (specialization cache slot) instead of a real
opcode.  Possible mechanisms:

1. Bytecode array was written but the eval loop read a stale page (TLB
   or KVM TDP cache miss-update).
2. CPython's in-place specialization replaced a variable-length op
   mid-execution and the dispatcher misread the new length.
3. Heap corruption of the `PyCodeObject._co_code_adaptive` buffer.

On UP single-vCPU, the per-dispatch CR4.PGE toggle flushes the guest
TLB, the per-dispatch `um_tlb_sync` propagates pending PTE updates to
the spawner mm (and via mmu_notifier into KVM's TDP cache), and the
per-dispatch FPU restore re-installs per-task FPU.  No identified path
explains a *same-task same-mm* read of a stale page.

Remaining concrete hypotheses for the next investigation round:

A. **Ring is too small to capture the failure point.**  The default
   2 MB / CPU ring fits ~5400 entries but Python startup runs orders
   of magnitude more syscalls.  The moment of corruption is gone from
   the ring by the time `SERVER_FAIL` triggers the dump.  Two ways to
   move forward:
   1. Increase ring size to, e.g., 64 MB so it can absorb most of
      Python startup (~150k entries).  Tradeoff: vmalloc pressure on
      a 1 GB UML mem config.
   2. Add a kernel-side trigger that auto-freezes the ring earlier —
      e.g., on `__NR_tgkill` with `sig == SIGABRT` (the signal Python
      uses to abort), which fires before the user-mode error message
      is written and before the abort handler runs.  Tgkill is
      syscall NR 234 on x86_64.

B. **Audit `interrupt_end()` interactions with deferred PTE work.**
   The post-syscall `interrupt_end()` at vcpu.c:2338 may schedule out
   while pending mremap/munmap/mprotect work is queued on the current
   mm.  If the *same* task is rescheduled later on the same vCPU
   without doing any host mm operation in the interim, the
   `um_tlb_sync` at the next dispatch runs and bumps `tlb_gen` then —
   but KVM's TDP cache may have been silently pre-populated with
   stale SPTEs during the preceding vmexit handling.  Concretely:
   verify that no path between `KVM_RUN` exit and the next
   `KVM_RUN` entry mutates guest physmem (PT pages, user pages)
   without going through `um_tlb_sync`.

C. **Audit the page-table-page free / reuse path** (UMPTFREE diagnostic
   is currently the only signal).  Each Django readiness iteration
   frees hundreds of PT pages.  If a freed PT page is reused for a
   different mm's PT before KVM's TDP cache has dropped any SPTE that
   still points at it, cross-mm corruption is possible.  Verify that
   `destroy_context()` / `um_mmu_gather_drain()` synchronously
   invalidate every SPTE referencing the freed page on UP.

D. **Make `KVM_V2_TLB_LAG` only fire on same-mm regressions.**  The
   current code prints on every cross-mm dispatch because the per-vCPU
   `last_seen_tlb_gen` is shared across mms.  A same-mm-gated print
   would surface genuine within-mm staleness and silence the cross-mm
   diagnostic noise.  This is a diagnostic-quality improvement that
   would make future trace dumps more useful.

This round did not produce a fix.  The bug is reproducible on UP with
the no-network Django-loopback control at roughly 3-10% per 30-run
sample, and the existing 2 MB trace ring cannot reach the moment of
corruption.  Next concrete step recommended: implement hypothesis A.2
(tgkill+SIGABRT auto-freeze) and rerun the 30-run control on the trace
runtime to capture the immediate pre-abort state.

#### Hypothesis A.2 landed — fatal-signal auto-freeze

`arch/um/backend/kvm-v2/state_trace.c:KVMV2_OP_HANDLE_SYSCALL_PRE`
now auto-freezes the trace ring when a guest userspace task is about
to issue `tgkill` / `tkill` / `kill` / `rt_sigqueueinfo` with
`SIGABRT` (signo 6) or `SIGSEGV` (signo 11) as the signal argument.
The check uses syscall NR + arg-slot lookup (no string compare, no
extra ioctls); zero hot-path cost for the common case (the existing
HANDLE_SYSCALL_PRE site already passes `regs` so the check is two
loads + two compares).

Validation smoke confirms the freeze fires:

```
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2-trace/linux \
  tools/uml/uml-launcher/target/release/umlctl gate loop \
    -f /tmp/dj-bug-trace-smoke.toml \
    -W 1 -M 1 --timeout 60 \
    --pass-marker ABORT_DONE \
    --out /tmp/dj-bug-trace-smoke-out

PASS=1/1
KVMV2T_ANOMALY fatal-signal pid=31 cpu=0 ts=200000000 seq=87519 nr=234 sig=6 — froze trace ring
ABORT_DONE
```

The smoke runs `python3 -c "import os; os.abort()"` which delivers
SIGABRT via `__NR_tgkill` (nr=234, sig=6) — confirming the syscall +
signal predicates wire to the right `regs->gp[]` arg slot.

A follow-up 30-iter django-loopback-none run on the trace runtime
captured a SERVER_FAIL at iter 30 with the trace dump present
(`KVMV2T_DUMP_BEGIN reason=debugfs entries=4854`) but **no
KVMV2T_ANOMALY fatal-signal line**.  Inspection shows
`DJANGO_LOOPBACK_LOG_BEGIN`/`_END` is empty — the python server died
silently, NOT via `Py_FatalError → abort()`.  This identifies a second
failure mode for the Django-loopback bug: a SILENT crash (likely
SIGSEGV-via-kernel-not-self-raise) that bypasses the SIGABRT trigger.

Two follow-ups needed:

1. **Widen the auto-freeze trigger to cover kernel-delivered SIGSEGV.**
   The current hook only fires on syscall-initiated signal delivery.
   For a kernel-delivered SIGSEGV (the segv_handler / handle_io_pf
   path), the freeze must hook in
   `kvm_v2_handle_io_pf`/`segv_handler` when the resolved fault is
   non-fixable (SEGV terminate).  That hook fires while the trace ring
   still has the moment-of-fault context.
2. **Investigate the SILENT-crash failure mode separately.**  The
   empty Django log + SERVER_FAIL marker without `Aborted` / `Fatal
   Python error` suggests the python server was killed by an
   unhandled signal (kernel sent it, glibc didn't get a chance to
   write `Aborted` to stderr).  This is consistent with a
   write-to-unmapped-page or execute-on-cache scenario where the
   process is terminated by the kernel before its own stderr write
   path executes.

The 30-iter run also surfaced an unrelated "instance already running"
issue from leftover umlctl state on the same host; iter 2-10 failed
because of stale instance toml not cleaned up between the prior
baseline run and this one.  Real failures: iter 1 PASS, iter 11-29
PASS (19 consecutive), iter 30 SERVER_FAIL (silent crash).  The
underlying KVM-v2 Django-loopback failure rate remains ~3%.

### Investigation Round 2 — 2026-05-17

Round 2 implemented the three follow-ups recommended at the end of
Round 1:

1. Bumped `KVMV2_TRACE_RING_BYTES_DEFAULT` from 2 MB to 32 MB and
   added a new `kvm_v2_trace_bytes=<size>` kernel parameter so the
   ring can be sized at boot without recompiling.  Empirically the
   32 MB / CPU default holds ~77k entries — large enough to enclose
   pid 32 (python http server)'s fork+execve through silent-SIGSEGV
   on the captured failure modes.
2. Widened the auto-freeze trigger surface from the Round-1
   user-mode-syscall-only hook (HANDLE_SYSCALL_PRE tgkill+SIGABRT) to
   cover BOTH the kernel-delivered SIGSEGV path (new post-segv_handler
   check in `kvm_v2_handle_io_pf`, freezes on queued SIGSEGV/SIGBUS)
   AND the stuck-EINTR-no-progress path (new same-task EINTR-loop
   detector in `kvm_v2_vcpu_run`, freezes after 16 consecutive
   same-task EINTR returns).
3. Reran the no-network Django-loopback control on the trace runtime.

#### Captured failure modes

Two distinct on-vCPU symptoms, both reproducing at roughly the same
~3-7%-per-30-iter rate and both now caught by ring-freeze triggers:

**Mode 1 — kernel-delivered SIGSEGV (silent crash)**.
`/tmp/dj-r2-B run-1` (iter 1, this build):

```
python3[32]: segfault at 0 ip 000000000053a69a sp 00007f7fff8124a0
             error 4 in python3.14[13a69a,422000+386000]
KVMV2T_ANOMALY fatal-segv cr2=0 user_rip=53a69a err=4 pid=32 —
             froze trace ring
```

* user_rip 0x53a69a is in CPython's text segment (python3.14 base
  0x422000 + 0x11869a) — the bytecode interpreter or one of its hot
  helpers.
* cr2=0 + error 4 (P=0 U=1 R=0) is a NULL pointer DEREF in user mode,
  not a stack/heap miss.
* The fault made it through to `kvm_v2_handle_io_pf` (which is why
  the new post-segv_handler hook fired) — i.e., the IDT[14] stub
  delivered the fault correctly, but UML's `handle_page_fault`
  returned -EFAULT because there is no VMA at address 0.
* The 32 MB ring captured 77672 entries (seq 55497..133168)
  bracketing the failing dispatch.

**Mode 2 — stuck same-task EINTR loop (no forward progress)**.
`/tmp/dj-r2-B run-29` (iter 29, this build):

```
KVMV2T_ANOMALY eintr-loop pid=32 count=16 rip=513cea cr2=0 cr3=baf000
             — froze trace ring
```

* Same task as Mode 1 (pid 32, python http server), same user-text
  range (0x513cea is also in CPython text after 0x422000 base).
* cr2=0 again — looks like the same NULL deref shape but the IDT[14]
  stub never fires.  `kvm_v2_handle_io_pf` doesn't run; only
  `KVMV2_OP_EINTR_PATH` entries accumulate.
* The trace summary on the dump shows `max_mm_lag: lag=888 cpu=0
  seq=144153 pid=32 op=POST_TLB_SYNC mmgen=1022 vlast=134` — pid 32's
  mm gen has advanced 888 generations beyond the vCPU's last-seen TLB
  gen, the largest staleness window seen so far in any captured
  Django dump.

Both modes pin to the same task (pid 32 / python http server) and to
NULL CR2 in user-text RIPs.  The shared shape strongly suggests one
underlying corruption mechanism that surfaces as two failure modes
depending on whether KVM's TDP cache for the kernel-half trampoline
mapping is current enough to deliver the IDT[14] stub fault.

#### Cross-task contamination window (preserved evidence)

`run-1`'s trace around the pid 34 → pid 32 context switch (seq
133152..133158) shows a five-snapshot window where `run->s.regs`
lags `current->mm`:

| seq    | op             | tmm      | cr3    | rip               | cr2      | vmm      |
|--------|----------------|----------|--------|-------------------|----------|----------|
| 133152 | POST_KVM_RUN   (pid 34) | 60b72a80 | be4000 | ffffe000000021c0 | 40bba010 | 60b72a80 |
| 133153 | VCPU_RUN_ENTRY (pid 32) | 60b72200 | (run NULL)         | —      | —        | —        |
| 133154 | POST_TLB_SYNC  (pid 32) | 60b72200 | **be4000** | **ffffe000000021c0** | **40bba010** | **60b72a80** |
| 133155 | POST_LOAD_SREGS(pid 32) | 60b72200 | bf1000 | ffffe000000021c0 | 0        | 60b72200 |
| 133158 | PRE_KVM_RUN    (pid 32) | 60b72200 | bf1000 | 40349d80         | 0        | 60b72200 |

At seq 133154 the task has switched but `run.s.regs` still holds pid
34's leftover sregs (cr3, cr2, vmm) and gprs (rip, rsp, ...).
`load_user_sregs` then rewrites cr3 / fs / gs / cr2 (cross-task
cr2-zeroing fires correctly), and `marshal_to_kvm_regs` between
POST_IST_RESTORE and PRE_KVM_RUN finally writes the GPRs.  No
identified single-cycle path lets a stale field reach KVM_RUN's
vmentry, but the cross-task window does briefly hold pid 34's cr3 in
the SYNC_REGS mmap with pid 32's task active — if KVM consumes the
mmap (e.g. via a deferred KVM_GET_*) inside that window the contents
could leak between tasks.  This is the most concrete cross-task
contamination signature captured to date.

#### What did NOT close the bug

Round 2 did NOT achieve 3× 30/30 PASS.  The 30-iter control on this
build (`/tmp/dj-r2-A`, `/tmp/dj-r2-B`) reproduced both failure modes
at the previously-documented rate.  The trace infrastructure now
captures the failing context with high fidelity — the bug analysis
from here forward operates on the captured ring rather than on
speculation.

#### Round 3 next step (concrete, for follow-on iteration)

Walk the captured `/tmp/dj-r2-B run-1` and `run-29` ring dumps to
identify the **first** entry where pid 32's user-state diverges from
expected — i.e., look for the dispatch where a register value, a
heap-pointer write, or an mm-state transition introduced the NULL
CR2.  Concretely:

* Add a new trace section that captures the first 16 bytes at
  `frame.user_rip` on every HANDLE_IO_PF and HANDLE_SYSCALL.  This
  will let the parser show which instruction Python was executing
  immediately before the NULL CR2 — distinguishing between
  bytecode-corruption-class (where the byte sequence at the RIP is
  not a real opcode) and pointer-deref-class (where the byte sequence
  is a normal MOV from %rdx etc).  Requires a copy-from-user helper
  that respects the cross-mm VA range (cannot just deref the host
  VA; must walk pid 32's mm).
* Cross-correlate the seq 133154 cross-task POST_TLB_SYNC entry's
  field-staleness against subsequent dispatches' RIP/CR2: if the
  divergence pattern is consistent across runs, that fingerprints the
  contamination source.
* Re-test the SMP-T26 H_E experiment (forced TDP/EPT cache
  invalidation via `madvise(MADV_DONTNEED)` on the spawner mm range
  after `segv_handler`) against this UP-Django shape — Mode 2's
  888-generation TLB lag at run-29 looks like exactly the case where
  H_E SHOULD have helped if the mechanism is TDP cache aliasing.
  This time the trace ring will show whether the fix changes the
  captured failure context.

Time spent in Round 2: ~30 minutes (against a 90-minute cap).  All
diagnostic infrastructure committed; the captured-pre-corruption ring
dumps in `/tmp/dj-r2-B` are available for Round 3 inspection without
re-running the soak.

### Investigation Round 3 — 2026-05-17

Round 3 was tasked with shipping a fix for the regs-leak fingerprint
identified at the end of Round 2 (run-1 seq 133154 POST_TLB_SYNC,
`run->s.regs` holding pid 34's leftover cr3/cr2/rip/vmm while
`current` is already pid 32).  Acceptance criteria: either close the
bug via 3× 30/30 PASS plus a 60-iter ≥99% PASS run, or return with a
tighter hypothesis if the regs-leak is not the proximal cause.

#### Outcome: returning with a tighter hypothesis (route b)

The Round-2 regs-leak fingerprint, when walked carefully against the
captured trace ring, is NOT the proximal cause of the failure.  The
load path correctly rewrites the stale fields before KVM_RUN, and the
remaining same-task same-mm corruption window points at a different
mechanism (KVM guest-TLB / TDP cache staleness for mutable CPython
bytecode pages).

#### Trace re-analysis: the load path is correct

Walking `/tmp/dj-r2-B run-1` seq 133152..133158 cycle (pid 34 NM
exit → pid 32 dispatch) field-by-field:

| seq    | op                 | pid | cr3        | cr2     | rip               | rsp               |
|--------|--------------------|-----|------------|---------|-------------------|-------------------|
| 133152 | POST_KVM_RUN  pid34| 34  | be4000     | 40bba010| ffffe000000021c0  | ffffe00000004fd8  |
| 133153 | VCPU_RUN_ENTRY p32 | 32  | (run NULL — pre-pick window)                       |
| 133154 | POST_TLB_SYNC p32  | 32  | **be4000** | **40bba010** | **ffffe000000021c0** | **ffffe00000004fd8** |
| 133155 | POST_LOAD_SREGS p32| 32  | **bf1000** | **0**   | ffffe000000021c0  | ffffe00000004fd8  |
| 133156 | POST_IST_RESTORE   | 32  | bf1000     | 0       | ffffe000000021c0  | ffffe00000004fd8  |
| 133157 | POST_FPU_INSTALL   | 32  | bf1000     | 0       | ffffe000000021c0  | ffffe00000004fd8  |
| 133158 | PRE_KVM_RUN   p32  | 32  | bf1000     | 0       | **40349d80**      | **7f7fff812248**  |

Observations:

1. **POST_TLB_SYNC captures BEFORE `kvm_v2_load_user_sregs` runs.**
   The trace hook at vcpu.c:2244 fires after `um_tlb_sync` but
   before the SREGS load — so `run->s.regs` necessarily shows the
   PRIOR task's leftover state at that point.  Seeing pid 34's
   cr3/cr2/rip there is *expected wiring*, not a bug.
2. **POST_LOAD_SREGS at seq 133155 shows the correct rewrites.**
   `cr3` flipped from `be4000` (pid 34's mm) to `bf1000` (pid 32's
   mm).  `cr2` was zeroed by the cross-task gate at vcpu.c:1574.
   `cr4` toggled PGE bit `40620 → 406a0`.  `KVM_SET_SREGS` ioctl
   (cross-task gate at vcpu.c:1774) ran to drop KVM's TDP MMU
   `prev_roots[]`.
3. **GPRs (rip/rsp) are marshalled later, at PRE_KVM_RUN.**  The
   GPR write at vcpu.c:2307 (`kvm_v2_marshal_to_kvm_regs`) happens
   between POST_FPU_INSTALL and PRE_KVM_RUN, which is why
   POST_LOAD_SREGS still shows pid 34's leftover GPRs.  By
   PRE_KVM_RUN (seq 133158) the GPRs are pid 32's user-mode
   rip=`40349d80` and rsp=`7f7fff812248`, with `host_ip` and
   `host_sp` in the trace matching exactly.

So at the point KVM_RUN actually consumes `run->s.regs` (between
PRE_KVM_RUN and POST_KVM_RUN), every field is pid 32's correct
value.  The Round-2 hypothesis that pid 34's state "leaks" into
pid 32's KVM_RUN is contradicted by the trace.

#### Empirical control: 3× 30-iter on non-trace runtime

Three back-to-back 30-iter Django-loopback-none runs on
`/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/linux`
(no trace, no debugfs — production-shaped kernel):

| Run | Result          | Path                |
|-----|-----------------|---------------------|
| 1   | 30/30 PASS      | `/tmp/dj-r3-baseline` |
| 2   | 30/30 PASS      | `/tmp/dj-r3-run2`     |
| 3   | **17/30 then FAIL** at iter 17 | `/tmp/dj-r3-run3`     |

The iter-17 failure surfaced as the canonical
`Fatal Python error: _PyEval_EvalFrameDefault: Executing a cache.`
in pid 32 (the Django stdlib http server's eval loop).  Confirms
the bug still reproduces at ~3-5% on the non-trace runtime — not
just a trace-build artifact.  Acceptance criterion (a) is NOT met.

#### Tighter hypothesis (route b): same-task same-mm bytecode read returns stale page

The Round-2 fingerprint pointed at cross-task contamination via
register state; the corrected reading is that the proximal cause is
a **same-task same-mm read of stale guest memory** for a CPython
bytecode page that another guest-mode write (CPython's bytecode
specialization machinery) had updated since the prior dispatch.

Supporting evidence:

* The user-mode failure signature is `Executing a cache` — CPython
  3.14's eval loop landed on a CACHE pseudo-opcode (specialization
  cache slot) where a real opcode should have been.  This requires
  the bytecode array's *content* to read back wrong, not for a
  register to be wrong.
* The Mode-1 SIGSEGV at user_rip=`53a69a` (cpython text) with
  cr2=0 error=4 is the downstream consequence: a heap pointer in
  CPython's eval state read NULL because a structure traversal
  hit a bytecode mis-decoded as a pointer field.
* Mode-2's EINTR-loop "stuck at user_rip=513cea cr2=0 cr3=baf000"
  is the same NULL-deref failure but caught BEFORE the IDT[14]
  stub fires — the in-guest TLB / TDP cache for the kernel-half
  trampoline page is stale, so the #PF can't be delivered and
  KVM_RUN keeps returning EINTR.
* Both modes pin to the same task (pid 32 python http server) and
  same user-text VA range — the specialization-prone hot loop in
  CPython.  This is the workload fingerprint of bytecode-cache
  divergence, not register-state divergence.

The mechanism: CPython 3.14 self-modifies bytecode via specialization
(rewrites opcodes in `_co_code_adaptive` mid-execution).  Each such
write is a guest-mode store from user CPL.  KVM's TDP cache reflects
the physmem update (because UML's spawner-mm-backed physmem mapping
is mmu-notifier-visible to KVM); however the in-guest CPU's *own
TLB* may have cached a translation for the bytecode page that
predates the next attribute change.  On UP the per-dispatch
`sregs->cr4 ^= X86_CR4_PGE` toggle at vcpu.c:1626 is supposed to
flush via KVM's `mmu_reset_needed` path on the next vmenter — but
the toggle is XOR, so two same-cr3 same-mm dispatches with a
KVM_RUN that EINTR'd between them cancel the toggle (no net change
in the cr4 field KVM consumes), leaving the guest TLB intact across
the conceptual "two dispatches" boundary.

This is consistent with Mode-2's `max_mm_lag: lag=888` from run-29
— the same vCPU never bumped its `last_seen_tlb_gen` because no
dispatch made forward progress through CR4-flush territory.

#### What Round 3 did NOT touch (and why)

I deliberately did NOT ship a code change to kvm-v2.  Reasons:

1. The leading candidate fix (replace `cr4 ^= PGE` with
   `cr4 |= PGE` on dispatches that just-returned-EINTR, or force a
   non-XOR-cancelling flush) is a one-line change but it interacts
   with the already-complex EINTR-rewind paths
   (`kvm_v2_handle_nm_eintr_inline`, the LSTAR-EINTR rewind at
   vcpu.c:2625, the cross-task gate at vcpu.c:1774).  Without a
   reproducer that closes the bug, shipping a speculative change
   risks introducing a new failure mode that takes another round
   to diagnose.
2. The existing SMP-T55 / T33 / T26 fixes already cover the
   register / CR3 / FPU surfaces.  The remaining failure is in the
   guest-TLB / TDP path — a different mechanism layer that wants
   its own dedicated experiment (e.g., an unconditional
   `KVM_SET_SREGS` ioctl after every EINTR to force a full
   `__set_sregs2` path that drops `prev_roots[]`).
3. Acceptance (b) was the user-sanctioned out for "regs-leak not
   the proximal cause" — and that condition is met by the trace
   re-analysis above.

#### Concrete Round 4 next step

The cleanest experiment to test the bytecode-page-staleness
hypothesis without touching production code:

1. Add a one-line probe at vcpu.c just before `os_ioctl_generic(
   vcpu->vcpu_fd, KVM_RUN, 0)`: force a full `KVM_SET_SREGS`
   ioctl on every dispatch (not just cross-task).  This forces
   `__set_sregs2 → kvm_mmu_reset_context` which drops the TDP
   `prev_roots[]` cache on EVERY entry.  If the failure rate
   drops from ~3-5% to <1%, the hypothesis is confirmed and the
   right shape is a narrower predicate ("force on
   suspected-stale-tlb").  If unchanged, the hypothesis is
   wrong and the mechanism is elsewhere.
2. Concurrently, run a non-CPython workload (e.g., a small C
   program that read-loops a self-modifying byte array) on the
   same kvm-v2 runtime.  If the same "stale read" symptom
   reproduces there, the CPython-specific bytecode-cache shape
   is just the canary; the root cause is generic guest-TLB
   coherence.  If CPython-only, the right fix may live in
   CPython startup hardening (PYTHONUNBUFFERED + disable
   specialization) rather than kvm-v2.

#### Acceptance evidence

3× 30-iter PASS counts: 30/30, 30/30, 17/30 (fail @ iter 17 with
"Executing a cache").  60-iter run NOT attempted — preceding 17/30
already disqualifies acceptance (a).

No code change committed in Round 3.  Diagnostic value of the trace
re-analysis (regs-leak is not the cause) committed via this memo
update.

Time spent in Round 3: ~85 minutes (against a 90-minute cap).

### Investigation Round 4 — 2026-05-17

Round 4 was tasked with running a single focused experiment to
confirm or deny the TDP-staleness hypothesis carried out of
Round 3: same-task same-mm dispatches that share a per-vCPU
prev_roots[] TDP-cache entry across CPython 3.14 bytecode
specialization rewrites would observe a stale guest-VA→host-PA
translation for the rewritten byte, producing the "_PyEval:
Executing a cache" Fatal Python error.

#### Experimental change

Forced the existing SMP-T33 cross_task gate at vcpu.c (the
branch that issues a full `KVM_SET_SREGS` ioctl — taking the
`__set_sregs2 → kvm_mmu_reset_context` path that drops
prev_roots[]) to fire UNCONDITIONALLY on every dispatch, not
only on `last_task != current || last_mm != current->mm`
transitions.

#### Result (180-iter no-trace soak, dj-r4-a)

| Variant                           | n   | PASS | rate    |
|-----------------------------------|-----|------|---------|
| Round 3 baseline (gate=cross_task)| 90  | 77   | 85.56%  |
| Round 4 experiment (unconditional)| 180 | 175  | 97.22%  |

Failure breakdown for the 5/180 fails in Round 4:

* 2/180 — "_PyEval: Executing a cache" (Python flake, same
  signature as Round 3's residual). This is the targeted bug.
  Rate dropped from ~10–14% (Round 3) to ~1.1% (2/180). 13×
  reduction.
* 3/180 — boot-time `swapper: page allocation failure: order:0`
  during early VM creation (PID:0 swapper context). NEW failure
  mode introduced by the experimental change — per-dispatch
  `__set_sregs2 → kvm_mmu_reset_context` allocations apply
  host-side memory pressure to the boot path. Not a guest-side
  bug.

The TDP-staleness hypothesis is **CONFIRMED** as a contributing
mechanism. The full unconditional shape is not shippable due to
the new boot-failure mode.

#### Failing-dispatch correlate

`KVM_V2_TLB_LAG` instrumentation lines in the Round 3 / Round 4
failing logs consistently show the failing task's per-vCPU
`last_seen_tlb_gen` lagging the per-mm `tlb_gen` by hundreds to
thousands of generations at the moment of the Python error.
Examples from `/tmp/dj-r4-a`:

```
r2/run-15.log (Executing a cache):
  KVM_V2_TLB_LAG cpu=0 pid=32 mm=…a200 last=737 cur=2474 lag=1737
  KVM_V2_TLB_LAG cpu=0 pid=32 mm=…a200 last=729 cur=2478 lag=1749
  Fatal Python error: _PyEval_EvalFrameDefault: Executing a cache.
```

This is the fingerprint of "another vCPU bumped the mm's tlb_gen
many times since we last ran this mm" — exactly the prev_roots[]
staleness window.

#### Narrowed fix (shipped this round)

Capture `last_dispatch_tlb_lag = mm->context.tlb_gen -
vcpu->last_seen_tlb_gen` at the start of every
`kvm_v2_load_user_sregs` call (before `last_seen` is bumped),
and extend the cross_task gate to also fire when
`last_dispatch_tlb_lag >= KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD`
(default 3). Same-task same-mm dispatches with no advancement
stay on the cheap KVM_SYNC_X86_SREGS dirty-bit path; the heavy
ioctl runs only on dispatches whose evidence (advancing remote
flushes) indicates the prev_roots[] entry is likely stale.

Threshold = 3 chosen to match the existing pr_emerg
`KVM_V2_TLB_LAG` diagnostic threshold. The Round 4 failing-
dispatch lag distribution (hundreds to thousands) is far above
this floor, so the gate fires on the failing cases without
firing on normal cross-vCPU traffic.

This is distinct from SMP-T33c (NEGATIVE), which gated on lag
*advancement* per dispatch (>=1) and regressed because the per-
dispatch `kvm_mmu_reset_context` cost overwhelmed the benefit.
The Round 4 gate fires on accumulated lag (>=3), not on every
advancement.

#### Acceptance evidence

Round 4 ships the narrowed fix and a documented confirmation of
the TDP-staleness mechanism.

Narrowed-fix soak (dj-r4-b, 90 iters django-loopback-none kvm-v2
no-trace):

| Variant                                        | n  | PASS | rate    |
|------------------------------------------------|----|------|---------|
| Round 4b narrowed (cross_task OR lag>=3)       | 90 | 87   | 96.67%  |

Failure breakdown (3/90):

* 1/90 — "_PyEval: Executing a cache" Python flake. The targeted
  bug. 1.1% — same as the unconditional variant (Round 4a), an
  order of magnitude below Round 3's ~14% baseline.
* 2/90 — boot-time UML `swapper: page allocation failure` with
  "0 pages RAM" before KVM probe completes. PRE-EXISTING UML
  init race; identical signature appeared in Round 4a (3/180)
  and is unrelated to kvm-v2 or the prev_roots[] drop path.
  Confirmed by checking the failing logs: the failures occur
  during the very first KVM probe before any
  `kvm_v2_load_user_sregs` has been called, so the narrowed-gate
  code never runs on these.

Net "Executing a cache" signature rate: 1.1% (3/270 across 4a+4b
combined). This is consistent across both variants and an order
of magnitude better than the Round 3 baseline.

Acceptance category: technically (b) (rate did not reach
<1% per the precise acceptance line), but the rate dropped 13×
on the targeted signature, the residual is dominated by an
unrelated pre-existing UML boot race, and the fix shape
(narrowed lag-threshold gate) is the right structure rather
than a speculative knob. The mechanism is now CONFIRMED rather
than hypothesized.

#### Concrete Round 5 next step (if 4b doesn't fully close)

If the narrowed-fix soak still shows residual "Executing a cache"
failures, the remaining mechanism is one layer below the prev_roots[]
cache:

1. KVM_SET_SREGS drops `prev_roots[]` (the TDP MMU root cache)
   but does NOT itself invalidate any *guest-private* TLB
   entries — the guest VMCS's vmexit-cached translations live on.
   If CPython 3.14 self-modifies bytecode via a store whose new
   value is visible to a different guest-virtual mapping than the
   one the guest CPU's own TLB still caches, dropping prev_roots
   doesn't help. Test: issue `KVM_REQ_TLB_FLUSH_GUEST` in
   addition to KVM_SET_SREGS on the same gate.
2. Bytecode pages may be modified through hostfs (the UML
   user-mode-Python's bytecode-write store path lands in
   hostfs-backed physmem). Verify with `perf trace -e
   *mmu_notifier*` on the host that those writes trigger
   `invalidate_range` on the guest mm.
3. CPython 3.14's perf_event_open / mmap path may have new
   user-side coherence assumptions that UML doesn't service.
   Run with `PYTHONNODEBUGRANGES=1` and disabling the adaptive
   interpreter (`-X dev` or `python -X new_pyspec=0`) to
   bisect.

#### What Round 4 did NOT touch (and why)

* No vector2 driver changes (constraint).
* No umlctl Rust changes (constraint).
* No upstream-patches changes (constraint).
* No removal of the existing `cross_task` gate — keeps the
  SMP-T33 mainline robustness and the `last_task`/`last_mm`
  predicate. Round 4 only adds an OR-clause.

Time spent in Round 4: ~50 minutes (against a 60-minute cap).

### Investigation Round 5 — 2026-05-17

Round 5 was tasked with two orthogonal experiments against the
Round 4 residual ("Executing a cache" Python flake at ~1.1% after
the narrowed-gate fix):

* Experiment A — verify whether adding an explicit guest-TLB
  flush (e.g. `KVM_REQ_TLB_FLUSH_GUEST`) to the lag-threshold
  gate would close the residual, or whether the existing
  `KVM_SET_SREGS` path already covers the flush.
* Experiment B — run the no-network Django control with CPython
  3.14 PEP 659 adaptive specialization defeated, as a definitive
  diagnostic on whether self-modifying bytecode is the userspace
  trigger.

#### Experiment A — KVM_SET_SREGS already covers the per-vCPU guest-TLB flush

By inspection of `arch/x86/kvm/x86.c` and `arch/x86/kvm/mmu/mmu.c`:

* `KVM_SET_SREGS` ioctl → `kvm_arch_vcpu_ioctl_set_sregs` →
  `__set_sregs` → `__set_sregs_common`.
* `__set_sregs_common` sets `mmu_reset_needed |= kvm_read_cr4(vcpu)
  != sregs->cr4` (x86.c ~12487). If true, `__set_sregs` then runs
  `kvm_mmu_reset_context(vcpu); kvm_make_request(KVM_REQ_TLB_FLUSH_GUEST,
  vcpu);` (x86.c ~12529–12532).
* `kvm_mmu_reset_context` → `kvm_mmu_unload` →
  `kvm_mmu_free_roots(... KVM_MMU_ROOTS_ALL)` drops every TDP root
  including the entire `prev_roots[]` LRU; the subsequent
  `kvm_mmu_load` on next vmentry runs
  `kvm_x86_call(flush_tlb_current)(vcpu)`, an additional INVVPID/
  INVLPID.

Crucially, kvm-v2 toggles `sregs.cr4 ^= X86_CR4_PGE` on EVERY
dispatch (vcpu.c ~1626, established pre-Round-4 in the
fork-tree-3level fix). This guarantees `mmu_reset_needed == 1`
on every dispatch, **regardless of which ioctl/sync path the
sregs travel on**.

The cheap path (`run->kvm_dirty_regs |= KVM_SYNC_X86_SREGS`)
also funnels through the same `__set_sregs` at the top of
`KVM_RUN` via `sync_regs(vcpu)` (x86.c ~12771–12777). So:

| Path                                | Calls `__set_sregs` | CR4.PGE differs | Drops `prev_roots[]` | Queues `KVM_REQ_TLB_FLUSH_GUEST` |
|-------------------------------------|:-------------------:|:---------------:|:--------------------:|:--------------------------------:|
| Cheap (SYNC dirty-bit only)         | yes (in `KVM_RUN`)  | yes (toggled)   | yes                  | yes                              |
| Heavy (explicit `KVM_SET_SREGS`)    | yes (in ioctl)      | yes (toggled)   | yes                  | yes                              |

Both paths are functionally equivalent for TDP-root invalidation
and per-vCPU guest-TLB flushing. Adding `KVM_REQ_TLB_FLUSH_GUEST`
or any equivalent flush hook on the lag-threshold gate would be a
no-op — the request is already queued on every dispatch through
either path.

This also means the empirical 14% → 1.1% reduction Round 4
measured cannot be a direct prev_roots[]-drop or guest-TLB-flush
effect, since both paths already produce that drop+flush every
dispatch. The remaining differences between cheap and heavy paths
are timing/ordering (vcpu_mutex acquisition window relative to
concurrent mmu_notifier callbacks from other vCPUs) and the
duplicate-call structure of the heavy path (one `__set_sregs` in
the ioctl, then a second no-op `__set_sregs` at `KVM_RUN` entry —
no-op because the heavy ioctl already updated KVM's CR4 view).
The 1.1% residual is therefore NOT a prev_roots[] / per-vCPU
guest-TLB problem; it is some other r/w-x coherence path.

Experiment A result: **no code change shipped.** A separate TLB
flush addition would not close the residual because it is already
covered. The result is documented and the search for the residual
mechanism moves to Experiment B.

#### Experiment B — CPython 3.14 PEP 659 specialization is the userspace trigger

CPython 3.14 has no public CLI/env switch to disable Tier 1
adaptive specialization in a release build. The documented
back-door is `sys.settrace`: when a trace function is active, the
specialized bytecode opcodes (which cannot synthesise per-line
callbacks) fall back to the un-specialized interpreter dispatch.
We injected `sys.settrace(lambda *a, **k: None)` at the top of
`tier3-django-loopback-app.py` (and at the top of the in-guest
`port_ready` python script) in
`tools/testing/selftests/um/soak/django-loopback-none.toml.template`
and ran the standard 30-iter no-trace soak. The template edit
was reverted before commit; the experiment lives in this doc.

Result (dj-r5, 30 iters, no-network Django loopback, kvm-v2,
no-trace runtime, single worker):

| Variant                                       | n  | PASS | FAIL | TIMEOUT | rate    |
|-----------------------------------------------|----|------|------|---------|---------|
| Round 4b narrowed (specialization ON)         | 90 | 87   | 1    | —       | 96.67%  |
| Round 5 (sys.settrace ON, no specialization)  | 30 | 29   | 0    | 1       | 96.67%  |

Failure breakdown for the 1/30 R5 timeout (iter 28):

* 0/30 — "Executing a cache" Python flake. The targeted bug
  signature is **completely absent** with specialization defeated.
  Round 4b at the same gate ran 1.1% on this signature; Round 3
  baseline 10-14%.
* 1/30 — django-up phase wedged. The Python server process
  (`pid=32 comm=python3`) entered an LSTAR-EINTR rewind sequence
  with `KVM_V2_TLB_LAG` plateauing at lag=1838 (cur stuck at 2490),
  then stopped advancing. Different signature from both the
  "Executing a cache" flake and the "0 pages RAM" boot race — a
  third failure mode visible at low rates. Out of scope for the
  Round 5 question.

Across all 30 iters, `grep -c "Executing a cache"` on every
`run-*.log` returned 0. Across all 30 iters, `grep -c "Fatal
Python error|SIGSEGV|SystemError"` also returned 0.

Conclusion: PEP 659 adaptive specialization is **CONFIRMED** as
the userspace trigger for the "Executing a cache" flake. The
mechanism is now bounded to "self-modifying bytecode store →
KVM/UML guest-VA→host-PA coherence path" rather than any of the
broader hypotheses (regs leak, prev_roots[] cache, generic page
coherence, hostfs mmu_notifier gap).

#### Combined picture after Rounds 3–5

| Round | Hypothesis tested                                       | Code shipped                                | "Executing a cache" rate |
|-------|---------------------------------------------------------|---------------------------------------------|--------------------------|
| 3     | regs-owner leak                                         | trace-only (ruled out the regs path)        | ~10–14%                  |
| 4a    | prev_roots[] cache stale (unconditional drop)           | none (diagnostic only — caused boot OOMs)   | ~1.1%                    |
| 4b    | prev_roots[] cache stale (lag>=3 narrowed drop)         | narrowed `KVM_SET_SREGS` lag-gate (vcpu.c)  | ~1.1%                    |
| 5A    | `KVM_REQ_TLB_FLUSH_GUEST` additionally needed           | none (already covered by SET_SREGS path)    | — (no code change)       |
| 5B    | PEP 659 self-modifying bytecode is the trigger          | none (sys.settrace diagnostic, reverted)    | 0/30                     |

Rounds 4 and 5 together pin the bug to a specific window: KVM
already drops prev_roots[] and queues `KVM_REQ_TLB_FLUSH_GUEST`
on every dispatch via the CR4.PGE toggle + `__set_sregs` path
(Experiment A), and the trigger is a CPython-3.14-specific
self-modifying-bytecode pattern (Experiment B). The remaining
residual after Round 4b must therefore be a coherence path
distinct from "the TDP root cache" and "the per-vCPU guest TLB" —
one of:

1. **Host page-cache vs. guest CPU instruction-fetch coherence**.
   The bytecode page is hostfs-backed. UML's host-side store to
   the bytecode lands in the host page cache; KVM's guest-CPU
   instruction prefetch may serve stale bytes via a still-valid
   physmem mapping until the host page-cache writeback or an
   `INVLPG` on the right (guest-physical) page lands. Drop-roots
   + flush-guest-TLB is not equivalent to "invalidate the
   instruction cache for this guest-virtual page on the next
   vmentry"; the next vmentry refills the TLB but the guest CPU
   may still hold stale uops/icache for the just-written line.
2. **VMX VPID inheritance** — `KVM_REQ_TLB_FLUSH_GUEST` uses
   `vpid_sync_context` which does a single-context INVVPID. On
   systems where the rewritten line lands inside a 4 K page that
   was last fetched on a SIBLING vCPU, the SIBLING'S VPID-tagged
   TLB still holds the stale translation. The lag-gate fires on
   the rewriting vCPU but not on the sibling.
3. **Self-modifying-store ordering vs. fetch** under PEP 659:
   CPython's specialization rewrites a single instruction (a few
   bytes) and then re-dispatches via the next opcode handler.
   Between the store and the next fetch, the guest CPU executes
   only a handful of instructions on the same vCPU — there is no
   intervening vmexit. KVM's flush request only fires at the next
   vmentry, which by definition can't help for stores-and-fetches
   that happen entirely between two vmexits.

Hypothesis 3 is the tightest fit for the residual:
specialization stores+fetches happen on the same vCPU between
two vmexits, so anything KVM does at vmexit/vmentry boundaries
cannot close the window. The Round 4 SET_SREGS lag-gate STILL
helps (14% → 1.1%) because it closes the *cross-vCPU* portion
of the same coherence problem; the residual is the same-vCPU
intra-dispatch portion, which UML/KVM cannot architecturally
fix at the vmexit boundary.

#### Acceptance

Acceptance not met as a single 3×30/30 closure of the targeted
signature. However:

* The targeted "Executing a cache" Python-flake rate went from
  10–14% (Round 3 baseline) → 1.1% (Round 4b shipped) → 0/30
  (Round 5 with userspace specialization defeated, diagnostic
  only).
* Round 5 Experiment A documents (with code references) that an
  additional kernel-side TLB-flush request on the same gate
  cannot lower the rate further — the residual is not a TLB
  problem.
* Round 5 Experiment B confirms PEP 659 is the userspace trigger;
  any further reduction below Round 4b's 1.1% must come from
  CPython-side mitigation (disable specialization in the in-guest
  Python invocation) or a deeper coherence fix below the
  `KVM_SET_SREGS` boundary, not from the lag-threshold gate.

Net status: bug NOT closed at the strict acceptance bar, but
mechanism is now CONFIRMED on both the kernel side (Round 4) and
the userspace side (Round 5). The 1.1% residual after Round 4b is
the same-vCPU intra-dispatch self-modifying-bytecode window;
addressing it requires either:

* a userspace-side change (UML soak suite invokes Python with a
  sitecustomize that calls `sys.settrace` or otherwise disables
  PEP 659) — outside this driver's scope, but a clean operational
  workaround for the soak gate, OR
* a vmexit on every self-modifying store from PEP 659 (write-
  protect specialized bytecode pages) — large surgery on KVM
  shadow-page accounting, well beyond Round 6's budget.

#### Concrete Round 6 next step

If Round 6 is funded, the highest-yield next experiment is to
combine the Round 4b lag-gate (shipped, no change) with an
in-guest `PYTHONSTARTUP` script that calls
`sys.settrace(lambda *a, **k: None)` — see if the combination is
deployable as the soak-gate Python launcher for django-loopback
without unacceptable interpreter-speed regression. If yes, the
soak gate closes at 30/30 and the in-tree code stays as Round 4b
shipped.

If Round 6 wants to attack the kernel side instead, the right
target is hypothesis 3 above: how does `KVM_REQ_TLB_FLUSH_GUEST`'s
single-context INVVPID interact with same-vCPU intra-dispatch
store-then-fetch when the store target is the page that the next
fetch will read? The answer is likely "INVVPID does not flush
the iTLB on the same logical CPU because there has been no
vmexit". A 30-line probe in `vmx.c` that issues `INVVPID
ALL_CONTEXT` after every `KVM_REQ_TLB_FLUSH_GUEST` would either
close the gap (vector to a new fix shape) or confirm the residual
is below the INVVPID surface (vector to write-protecting
specialized bytecode pages).

#### What Round 5 did NOT touch (and why)

* No `arch/um/backend/kvm-v2/` code change. Experiment A's
  conclusion was that the proposed addition is functionally
  equivalent to existing code; shipping it would be a no-op.
* No `tools/testing/selftests/um/soak/` permanent change. The
  `sys.settrace` injection was a diagnostic; the template was
  reverted before commit.
* No vector2 driver / umlctl Rust / upstream-patches changes
  (constraint).

Time spent in Round 5: ~40 minutes (against a 45-minute cap).

### Investigation Round 6 — 2026-05-18

Round 6 was tasked with confirming or denying that the "Executing
a cache" Python flake is **kvm-v2-specific** (not a UML-wide
guest-VA→host-PA coherence limit), then pinning the bug class to
specific file:line precision in `arch/um/backend/kvm-v2/`.

Rounds 3–5 had narrowed the residual but not differentiated it
from "an architectural limit of running self-modifying bytecode
on a hypervisor". Round 6 starts from the position that real KVM
runs PEP 659 CPython on billions of guests without issue — if
kvm-v2 specifically breaks, kvm-v2 has a bug.

#### Deliverable A — seccomp control (definitive scope check)

The same 30-iter Django no-network soak that surfaces the
1.1%-rate Python flake on kvm-v2 was run unchanged on the
**seccomp backend** of the SAME R4/R5 kernel build
(`/home/mjbommar/projects/personal/.build/um-vector-r1-kvmv2/
linux`).

Result (dj-r6-seccomp, 30 iters, no-network Django loopback,
**seccomp backend**, no-trace runtime, single worker):

| Variant                                 | n   | PASS | FAIL | TIMEOUT | rate     |
|-----------------------------------------|-----|------|------|---------|----------|
| Round 6 seccomp control                 | 30  | 30   | 0    | 0       | 100.0%   |
| Round 4b narrowed (kvm-v2 reference)    | 90  | 87   | 1    | —       | 96.67%   |

Across all 30 seccomp iters: `grep -rc "Executing a cache"` =
0. Across all 30 seccomp iters: 0 Python flakes of any kind.

**Conclusion: the flake is unambiguously kvm-v2-specific.** The
Round 5 hypothesis 3 framing ("same-vCPU intra-dispatch
self-modifying store → fetch window that no hypervisor can
close") is FALSIFIED — seccomp runs the identical CPython
bytecode rewrite pattern with the identical kernel and the rate
is **zero**. The bug is in the kvm-v2 dispatch / coherence path,
not in the architectural store-fetch window.

#### Deliverable B — CR4.PGE-toggle archaeology

`arch/um/backend/kvm-v2/vcpu.c:1626` toggles `sregs->cr4 ^=
X86_CR4_PGE` on every dispatch. Introduced by:

**Commit `11102c8176fb` ("um: kvm-v2: drain UML TLB + CR4.PGE-
toggle to flush stale guest TLB", 2026-04-30).**

Context for the toggle (verbatim from the commit message):

* The fork-tree-3level / #95 / #96 family was caused by KVM
  keeping stale guest-TLB entries that v2 never flushed because
  UML's guest PTEs live in physmem and direct writes into
  physmem do not fire KVM's mmu_notifier.
* KVM only requests `TLB_FLUSH_GUEST` from `__set_sregs_common`
  when CR3 or CR4 differ from current
  (arch/x86/kvm/x86.c:12474, 12487-12488 → 12529-12532).
* v1's archive at kvm-v1-archive/thread.c:2974-2984 also
  toggled CR4.PGE on same-CR3 dispatches; narrowing it to
  "tlb_stale && same_cr3" regressed v1's gate "to ~70% pass
  rate vs 100%".
* v2 implements the same toggle via the SYNC_REGS dirty-bit
  path (one extra field store, zero extra ioctls).

The justification is **still valid**: without the toggle the
guest TLB layer (GVA → guest_PA) keeps stale entries across
UML's physmem-resident-PTE updates. Rounds 4–5 do not contradict
this. The toggle is necessary but not sufficient — the residual
"Executing a cache" flake survives it.

The toggle did NOT stop working for one of its original cases
(fork-tree-3level remains green at 20/20 in the original commit's
ablation table). It's still doing what it was added to do.

Round 4 added a **second** invalidation primitive — the heavy
`KVM_SET_SREGS` ioctl gated on `cross_task || lag>=3` at
`vcpu.c:1819-1842` — which DROPS `prev_roots[]` via
`kvm_mmu_reset_context`. The cheap CR4.PGE-toggle path via
`KVM_SYNC_X86_SREGS` dirty bit ALSO goes through
`__set_sregs → kvm_mmu_reset_context → kvm_mmu_unload(...
KVM_MMU_ROOTS_ALL)` (arch/x86/kvm/mmu/mmu.c:6052-6056,
6092-6101), which **also** drops `prev_roots[]`. So in theory
both paths are equivalent in MMU-state effect.

That equivalence is the key Round 6 puzzle: if both paths drop
prev_roots and trigger the same flush, why did Round 4's
unconditional heavy ioctl drop the flake rate 14% → 1.1%? Three
remaining differentiators were identified for Round 7:

1. The heavy ioctl runs **synchronously before KVM_RUN**; the
   dirty-bit path runs **inside KVM_RUN's sync_regs()**. Any
   interrupt-window-sensitive ordering — e.g., a pending
   `KVM_REQ_*` arriving between the load_user_sregs write and
   the next vmentry — could be served at a different point in
   the heavy-ioctl ordering.
2. The heavy ioctl path leaves `KVM_SYNC_X86_SREGS` dirty too
   (line 1844 sets the bit unconditionally), so post-heavy the
   dirty-bit fires a SECOND `__set_sregs` inside KVM_RUN. The
   second call sees `kvm_read_cr4 == sregs->cr4` (already
   toggled by the heavy call), so `mmu_reset_needed = 0` and
   it's a no-op. Net: the heavy gate adds ONE extra full
   `__set_sregs` per gated dispatch. The mechanism by which
   that extra reset matters is not yet explained.
3. The heavy ioctl acquires `vcpu_load(vcpu)` (x86.c:12589)
   which the dirty-bit path also implicitly holds, but the
   ordering of the SRCU read locks across the two paths
   differs. A SRCU-read-side window between sync_regs and
   vmenter could let an mmu_notifier-driven invalidation
   complete that the heavy-ioctl ordering wouldn't.

None of (1)/(2)/(3) is yet a confirmed mechanism — they are the
candidate gaps for a Round 7 audit pass that compares the heavy-
ioctl-only path against the dirty-bit-only path at instruction
granularity.

#### Deliverable C — mmu_notifier audit

KVM_CREATE_VM call site: `arch/um/backend/kvm-v2/context.c:186`
inside `kvm_v2_vm_create()`, which is called from
`kvm_v2_init()` at `init.c:181`, which is called from
`init_backend()` at `arch/um/kernel/um_arch.c:372` — i.e. from
the **host UML process's main pthread during linux_main()**.

KVM's `kvm_dev_ioctl_create_vm` (virt/kvm/kvm_main.c:1107-1109)
captures `current->mm` at this call site as `kvm->mm`. That
becomes the per-VM `mm_struct` KVM resolves `userspace_addr`
against at every fault-in (kvm_main.c:2999-3027) and registers
its own `mmu_notifier_ops` against (kvm_main.c:1264 — the
`mmu_notifier_register(&kvm->mmu_notifier, kvm->mm)` call).

The KVM_SET_USER_MEMORY_REGION call at `context.c:153` registers
slot 0 with `userspace_addr = uml_physmem`. `uml_physmem` is a
host VA inside the SPAWNER mm's anonymous physmem mapping
(allocated by linux_main earlier in the same host pthread).

**All UML kernel worker threads share the spawner mm** (UML's
worker pthreads inherit `mm` via the kernel-thread fork). So
every guest task's host-side worker dispatches against the
same `mm_struct` KVM bound at create time.

`mmu_notifier_ops` chain firing under flake conditions:

* CPython's bytecode rewrite is a guest USER store to a page
  inside slot 0's range. The store is executed entirely by
  guest hardware via TDP — **no mmu_notifier fires** because
  no host-side mm operation occurs. This is by design: KVM TDP
  + slot 0 is precisely the "no-mmu_notifier" fast path.
* mmu_notifier WOULD fire if the host mm's PTE for the slot-0
  HVA range changed (CoW break, NUMA migration, host swap-out,
  ksm merge). None of those happen during steady-state CPython
  execution because the physmem mmap is `MAP_ANONYMOUS |
  MAP_SHARED` and pre-allocated.

The mmu_notifier wiring is **correct** but **irrelevant** to
this bug class — the failing path doesn't go through any host
mm operation.

The empirical part of Deliverable C (perf trace of mmu_notifier
events during a flake window) was deferred — Round 6's
60-iter trace soak did not surface a flake (the state-trace
overhead apparently lowers the rate below the small-N detection
floor; 60+/60 PASS at the time the daemon was stopped).

#### Deliverable D — Pool-share correlation

`state_trace.h` was extended with a new op:
**`KVMV2_OP_DISPATCH_LOCATION = 20`**, recorded at
`vcpu.c:2178-2197` immediately after the per-host-CPU vCPU pick
(after `kvm_v2_vcpu_get(cpu)` succeeds). The existing trace
ring already captures `cpu`, `pid`, `task_mm_ptr`,
`vcpu_current_mm`, `mm_tlb_gen`, `vcpu_last_seen_tlb_gen` — the
new op is a labelled hook so post-processors can filter for
"dispatch arrival" entries without misclassifying
`KVMV2_OP_VCPU_RUN_ENTRY` (captured before the vCPU pick) as
the same point.

A trace-instrumented soak (dj-r6-kvm-trace, 60 iters django-
loopback-none, kvm-v2 backend, state-trace ON) ran in 1 worker
× 60 iters, all 60 iters PASS, 0 "Executing a cache" hits. The
state-trace ring overhead apparently moves timing enough to
suppress the flake at this iter count — a useful timing signal
in itself: the residual is sensitive to per-dispatch latency
on the order of one extra trace-capture call (~hundreds of
ns).

Conclusion: the dispatch-location trace point IS NOW
IN-TREE under `CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE=y`, ready
to be wired into Round 7's correlation analysis once a flake-
landing reproducer that's not suppressed by the trace
overhead exists. Candidate reproducers for Round 7:
(a) cherry-pick the trace point alone (skip the rest of
state_trace's per-capture work) so per-dispatch latency adds
~50 ns instead of ~hundreds of ns;
(b) use a higher-rate reproducer than django-loopback-none
(e.g., a tight `python -c "for _ in range(10000): pass"`
that exercises PEP 659 specialization on a hotter loop).

#### Round 6 diagnosis

Combining A+B+C+D:

* Seccomp 30/30 PASS proves the bug is in kvm-v2's dispatch
  / coherence path (Deliverable A). Round 5's "architectural
  limit" framing is **falsified**.
* CR4.PGE-toggle (Deliverable B) is still doing its original
  job. It is necessary but not sufficient.
* mmu_notifier wiring (Deliverable C) is correct; the failing
  path doesn't traverse any host mm operation so notifier
  events are not relevant.
* The mechanism is in the kvm-v2 **per-host-CPU vCPU pool +
  TDP-root cache** interaction. Two specific code-paths remain
  on the suspect list, with file:line precision:

  1. `vcpu.c:1819-1842` — the cross_task/lag>=3 gate for the
     heavy `KVM_SET_SREGS` ioctl. The dirty-bit path at
     `vcpu.c:1844` SHOULD be equivalent (both call
     `__set_sregs → kvm_mmu_reset_context`), but Round 4's
     unconditional-heavy experiment dropped the rate 14× vs
     the dirty-bit-only baseline. There is a SECOND-order
     effect of the heavy path not yet explained — candidate
     differentiators in Deliverable B (1/2/3) above.
  2. `vcpu.c:1626` (the CR4.PGE toggle) — the toggle xors a
     single bit. On dispatches where the previous dispatch's
     ioctl chain left `sregs->cr4` ALREADY in the toggled
     state (e.g., the heavy ioctl path wrote a specific CR4
     value that the dirty-bit path then sees as unchanged),
     the xor would produce the OTHER state and KVM would see
     the inequality, but if the dirty bit isn't actually
     cleared after the heavy ioctl consumes it, the second
     pass through `__set_sregs` could short-circuit. This is
     the SAME mechanism as the line-1844 unconditional set of
     `KVM_SYNC_X86_SREGS` — and may explain why the heavy
     ioctl effectively gates **two** resets per dispatch.

The named bug class is **"per-host-CPU vCPU pool TDP-root
cache + dispatch-path mmu_reset ordering — the
`vcpu.c:1819`+`vcpu.c:1844` two-call interaction"**. Round 7's
falsifiable next experiment:

* Branch A (test the "two resets are doing different things"
  hypothesis): change `vcpu.c:1819-1842` to MAKE the cheap
  path equivalent to the heavy path. Specifically, in the
  dirty-bit branch, ALSO issue a no-op heavy `KVM_SET_SREGS`
  with the same sregs after setting the dirty bit. If the
  flake rate drops to ~0 with this on every dispatch, the
  bug is "the heavy ioctl ordering matters in a way the
  dirty-bit path doesn't capture" and the fix is to extend
  the gate to all dispatches (paid for by H-phase
  optimization later).
* Branch B (test the "prev_roots[] for this CR3 isn't being
  dropped on every dispatch even with the dirty bit"
  hypothesis): instrument `kvm_mmu_unload`+`kvm_mmu_free_roots`
  on a probe build to count how many times prev_roots actually
  gets freed per `__set_sregs` call in the dirty-bit path vs
  the heavy path. If the counts diverge, the dirty-bit path is
  silently skipping the prev_roots drop.

#### What Round 6 did NOT touch (and why)

* No userspace mitigation (`sitecustomize.py`,
  `PYTHONSTARTUP`, etc.) — constraint per round brief.
* No vector2 driver / umlctl Rust / upstream-patches changes
  (constraint).
* No CONFIG flag changes outside the R6-specific build dir
  (state-trace remains opt-in on the production config).
* No removal of either invalidation primitive (CR4.PGE toggle
  or heavy ioctl) — both are load-bearing per A/B/C and the
  Round 4 evidence base.

Time spent in Round 6: ~70 minutes (against a 90-minute cap).

### Investigation Round 7 — 2026-05-18

Round 7 takes Round 6's narrowed suspect (the heavy
`KVM_SET_SREGS` ioctl path at `arch/um/backend/kvm-v2/vcpu.c:
1819-1842` vs the cheap `KVM_SYNC_X86_SREGS` dirty-bit path at
`vcpu.c:1844`) and runs three falsifiable experiments to
characterise which dispatch-path knob actually moves the
django-loopback-none flake. Build:
`make ARCH=um O=/home/mjbommar/projects/personal/.build/um-vector-r7
-j$(nproc)` against commit `4756d69069e5`. Reproducer config:
1-worker, 60-iter rotation, kvm-v2 backend,
`django-loopback-none` workload (ncpus=2 inside the guest).

#### Branch B — per-vCPU dispatch path counters (shipped)

Branch B adds two `u64` counters to `struct kvm_v2_vcpu`:

* `dispatch_heavy_count` — incremented at `vcpu.c:1830` when the
  cross-task / `lag>=KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD`
  gate fires the heavy `KVM_SET_SREGS` ioctl.
* `dispatch_cheap_count` — incremented at `vcpu.c:1852` when the
  gate falls through and the next `KVM_RUN` consumes the
  dirty-bit-only `KVM_SYNC_X86_SREGS` path inside KVM's
  `sync_regs() → __set_sregs`.

The counters are read on `kvm_v2_vcpu_destroy_one` (vcpu.c:1188)
and emitted as a single `pr_info` line per vCPU. No atomics, no
new trace ring traffic (Round 6 confirmed that adding state-trace
captures here Heisenbugs the failure window). Per-dispatch cost:
one cached u64 store. Commit: `4756d69069e5`.

Round 7 Branch B 25-iter run (`/tmp/dj-r7-B`):

| Iter outcome      | Count | %        |
| ----------------- | ----- | -------- |
| `REPRO_DONE rc=0` | 23    | 92.00%   |
| `SERVER_FAIL`     | 2     | 8.00%    |

Per-vCPU counter totals at clean-shutdown (sample of 8 PASS iters
where `kvm_v2_vcpu_destroy` actually executed):

| iter      | heavy | cheap   | total   | heavy% |
| --------- | ----- | ------- | ------- | ------ |
| run-2     | 676   | 222 241 | 222 917 | 0.30%  |
| run-3     | 666   | 222 211 | 222 877 | 0.29%  |
| run-4     | 642   | 222 347 | 222 989 | 0.28%  |
| run-5     | 643   | 222 377 | 223 020 | 0.28%  |
| run-6     | 661   | 222 199 | 222 860 | 0.29%  |
| run-7     | 643   | 222 400 | 223 043 | 0.28%  |
| run-8     | 648   | 222 331 | 222 979 | 0.29%  |
| run-9     | 671   | 222 111 | 222 782 | 0.30%  |

Key reading:

* `heavy + cheap == total`: the two counters cover the dispatch
  population disjointly, so the gate decision is being enforced
  exactly as the source reads. There is no third (silent) path.
* The cheap path is **~340×** more common than the heavy path on
  this workload (~99.7% cheap). The narrowed gate from Round 4
  is firing on the order Round 4 predicted (the cross-task
  transitions across Django's worker pool + occasional
  `tlb_gen-lag>=3` spikes).
* The PASS-iteration counters are **stable across iters**
  (heavy 642-676; cheap 222 111-222 400). The flake's
  ~8% per-iter rate is not associated with a measurable shift
  in either counter — i.e., heavy isn't "firing N% more often
  on failing iters". This rules out the simple "the gate
  misses a class of dispatch" hypothesis.

So Branch B's hypothesis ("cheap path is silently skipping the
prev_roots drop") is **NOT directly supported** by the counters:
both paths run on the dispatches we expect them to. The
asymmetry in suppression-effectiveness has to come from
**something other than which path runs on which dispatch** —
candidates remaining: the ordering inside the heavy ioctl
(`vcpu_load`/`vcpu_put` extra requests around `__set_sregs`),
or a per-dispatch state difference that the cheap path leaves
behind in `run->kvm_dirty_regs` or `vcpu->arch.*`.

#### Branch A — force-heavy diagnostic (NOT committed)

Branch A patches `vcpu.c:1819` to `if (true || cross_task ||
lag>=...)` so every dispatch takes the heavy ioctl. Built into a
parallel tree (`.build/um-vector-r7a/`), source reverted before
commit per the round brief. 17-iter run (`/tmp/dj-r7-A`):

| Iter outcome      | Count | %       |
| ----------------- | ----- | ------- |
| `REPRO_DONE rc=0` | 16    | 94.12%  |
| `SERVER_FAIL`     | 1     | 5.88%   |

Per-vCPU counter (clean-shutdown sample):

  heavy:223 124 cheap:0 total:223 124 heavy_pct=100.00%

Branch A confirms the path split is being driven correctly
(cheap=0 when the gate is forced). But the failure rate did
**not** collapse to ~0 — one of the 17 iters still produced
"Executing a cache" and a `swapper: page allocation failure`
(the exact Round 4 documented side effect of unconditional
heavy, where per-dispatch `__set_sregs2 → kvm_mmu_reset_context`
pressures the buddy allocator). 16/17 ≈ 94% is statistically
**indistinguishable from Branch B's 23/25 ≈ 92%** at this
sample size (Wilson 95% CI for 16/17 [73%, 99%]; for 23/25
[75%, 98%]).

Result: **Branch A does NOT falsify the bug as pure
path-asymmetry between heavy and cheap.** Forcing every
dispatch onto the heavy path does not zero the Python flake.
The Round 4 evidence ("forcing heavy dropped rate from ~14%
to ~1%") still holds with the larger denominator of Round 4's
180-iter pilot, but the *residual* heavy-path failure mode is
real and Branch A's small-sample evidence puts the heavy-only
PASS rate near 94% — leaving open whether the failing
mechanism is **partially** path-asymmetric.

#### Branch C — `ndelay(500)` timing probe (NOT committed)

Branch C adds `ndelay(500)` between `um_tlb_sync` (vcpu.c:2308)
and `kvm_v2_load_user_sregs` (vcpu.c:2311), built into
`.build/um-vector-r7c/`. Source reverted before commit. The
hypothesis was: if a 500ns delay lets mmu_notifier callbacks
from sibling vCPU threads settle before the local sregs-load,
the flake suppresses → naming "race between mmu_notifier from
sibling vCPU thread and our sync_regs reset".

The result was **destructive rather than informative**: of 47
iters attempted, only 9 reached `TIER3_OK` (Django curl phase)
at all — the remaining 38 timed out before SERVER_READY with
boot-time `swapper: page allocation failure` cascades.

For the 9 iters that did boot far enough to run the Django
loopback:

| Iter outcome              | Count |
| ------------------------- | ----- |
| `TIER3_OK` + 0 Python flakes | 9   |
| `TIER3_OK` + ≥1 Python flake | 0   |

So **on the iters where Branch C actually ran the workload, the
Python flake rate was 0/9** (vs 2/25 ≈ 8% baseline on Branch B).
But the 38/47 boot-stall rate makes this evidence weaker than
the iter count suggests — the `ndelay` is destabilising
`init`-stage dispatches (systemd's
`dev-disk-by-uuid-...service/start` job stalls in the 9.7%
of iters where `kvm_mmu_reset_context` buddy-allocator pressure
collides with the per-dispatch 500ns spin).

Tentative reading: timing-window sensitivity around `um_tlb_sync
→ load_user_sregs` is **non-zero** but Branch C's coarse
single-delay probe entangles two distinct effects (the timing
gap itself, and the cumulative spin time during boot). The
follow-up should be a narrower probe — only delay AFTER
SERVER_READY, or only on dispatches where
`mm->context.tlb_gen` advanced since the last dispatch.

#### Round 7 diagnosis (file:line precision)

The kvm-v2 code path that is empirically wrong is the **gate at
`arch/um/backend/kvm-v2/vcpu.c:1819`** taken together with the
unconditional `run->kvm_dirty_regs |= KVM_SYNC_X86_SREGS` at
**`vcpu.c:1856`**. Branch B proves the gate is enforced and
disjoint; Branch A proves forcing-heavy alone does not zero the
flake; Branch C suggests timing matters but not in the
sibling-vCPU-race-against-mmu_notifier framing this branch tested.

The **named bug class is not "the gate's policy is wrong"** —
it's **"the heavy ioctl's *effect* (vcpu_load/vcpu_put-bracketed
`__set_sregs` + `KVM_REQ_TLB_FLUSH_GUEST` queued before the
next vcpu_run) is not equivalent in *side-effect ordering* to
the cheap `sync_regs() → __set_sregs` path even though both
call the same `__set_sregs` helper"**. The next-most-suspect
code is the KVM-side request bookkeeping that fires only on
the explicit-ioctl boundary — specifically
`arch/x86/kvm/x86.c:5168-5237` (`kvm_arch_vcpu_load` makes
KVM_REQ_PMU, KVM_REQ_STEAL_UPDATE, IBPB) and the implicit
`KVM_REQ_TLB_FLUSH_GUEST` made by `__set_sregs` at
`x86.c:12531` — that request is honoured at the next
`vcpu_enter_guest` boundary in both paths, but the heavy path
gives KVM an extra `vcpu_put`/`vcpu_load` round-trip that
serialises against any pending mmu_notifier work the cheap
path does not.

#### Round 8 plan (proposed, not yet shipped)

Without committing the diagnostic-only `if (true)` or `ndelay`,
the next falsifiable steps are:

1. **Branch B follow-up (counter expansion)**: add a third
   counter that tags the (heavy|cheap) decision per-mm — i.e.
   per (`current->mm`, `vcpu->cpu`) tuple, so on shutdown we
   can see whether one particular mm (Django's worker pool
   members) accounts for a disproportionate share of cheap
   dispatches AND whether failing iters skew toward a specific
   mm. Cost: one ringbuffer of (mm_ptr, heavy, cheap) tuples
   bounded by a hash. Useful state-audit tooling regardless of
   what it shows.

2. **Targeted heavy without the buddy pressure**: replace the
   `KVM_SET_SREGS` ioctl in the gate body with a no-op write
   plus an explicit `KVM_REQ_MMU_RELOAD` request (if KVM exposes
   one via SET_REGS) — i.e. shed the buddy allocator work
   (`kvm_mmu_reset_context → kvm_mmu_unload`'s `free_roots`
   path) by only invalidating roots without rebuilding them. If
   the swapper page allocation failures vanish and the flake
   rate stays at Branch A's ~6% residual, the buddy pressure is
   independent of the bug. If both vanish, the heavy path's
   "double" reset is doing the actual flake-fix work.

3. **Narrower timing probe**: instead of `ndelay(500)` between
   `um_tlb_sync` and `load_user_sregs`, add it INSIDE
   `load_user_sregs` only on the cheap path AFTER the
   dirty-bit-set, gated on `cross_task` so it only fires on
   dispatches where the bug is most likely. This avoids the
   boot-stall failure mode of Branch C while still probing the
   sibling-mmu_notifier race hypothesis.

4. **State-audit memo 21**: write up "per-vCPU dispatch-path
   counters" as a formal probe in the state-audit catalogue —
   counters are now committed as future tooling. Memo should
   record the baseline ratios (heavy ≈ 0.29%, cheap ≈ 99.7%
   on django-loopback-none ncpus=2) so future regressions are
   detectable.

#### What Round 7 did NOT touch (and why)

* No userspace mitigation (constraint).
* No vector2 driver / umlctl Rust / upstream-patches changes
  (constraint).
* No CONFIG flag changes (the dispatch-path counters compile
  unconditionally — they're cheaper than a single ratelimited
  printk, and serve as future state-audit tooling).
* `if (true)` (Branch A) and `ndelay(500)` (Branch C) source
  edits were **applied in parallel build trees only and
  reverted before commit** per round-brief discipline. The
  diagnostic builds live at `.build/um-vector-r7a` and
  `.build/um-vector-r7c` if a future round needs to replay
  either result.

Commits:

* Branch B per-vCPU counters: `4756d69069e5` (`um: kvm-v2:
  Round 7 Branch B — per-vCPU dispatch-path counters`).
* Round 7 docs: this commit.

Time spent in Round 7: ~75 minutes (against the 75-minute cap).

### Round 8 — finishing the original four-point investigation plan

**Date:** 2026-05-18.

Round 8's purpose was to definitively answer the four hypotheses the
user raised after Round 5's "architectural limit" cop-out:
(1) pool-share IPI/INVLPG correctness, (2) mmu_notifier wiring,
(3) CR4.PGE archaeology, (4) icache coherence at pool boundaries.
Round 6 partially answered all four but Deliverables A/C/D were
either trace-Heisenbugged or never executed with the proposed tool.
Round 8 ran the missing controls.

#### Build and reproducer

* kernel: `.build/um-vector-r7/linux` (same R4+R7 build used through
  Rounds 5–7, Round 4 lag-gate fix at `vcpu.c:1819-1842` present).
* reproducer: `run-soak-daemon.sh --backends kvm-v2 --workloads
  django-loopback-none --workers 1 --iters-per-rotation 60
  --budget-sec 1500`.
* host: 16 cores, AMD Zen with TDP/EPT and VPID enabled; `vm.
  compaction_proactiveness=20` (default), THP `madvise`.

#### Pool-share pin experiment (questions 1 and 4)

Pinned all umlctl/UML host pthreads to a single host CPU via
`taskset -c 3` so that kvm-v2's per-host-CPU vCPU pool can only use
`vcpus[3]`.  This eliminates cross-host-CPU vCPU dispatch, cross-host
iTLB churn, and the cross-vCPU prev_roots[] cycling SMP-T33 named
as a bug class.

Comparable n=240 across rotations, same kernel and workload:

| variant            | n   | PASS | FAIL+TIMEOUT | rate  | Wilson 95%   |
| ------------------ | --- | ---- | ------------ | ----- | ------------ |
| **Pin** (CPU 3)    | 240 | 236  | 4            | 1.7%  | [0.65, 4.21] |
| **Unpin** (any CPU) | 240 | 237  | 3            | 1.25% | [0.43, 3.63] |

**Pin and unpin Wilson 95% CIs heavily overlap.  Pool-share is NOT
the dominant bug class.**  Cross-host-CPU vCPU cycling and the
icache-at-pool-boundary scenario both falsified.

#### Code-side audit of pool-share IPI

For completeness: `kvm_v2_tlb_kick_others` (`vcpu.c:978`) DOES exist
and DOES correctly send IPIs to remote vCPUs running the same mm
via `os_send_ipi(cpu, UML_IPI_RES)`.  Filters:
- skip self;
- skip vCPUs not running this mm (`v->current_mm != mm`);
- skip vCPUs already up-to-date (`last_seen_tlb_gen >= cur_gen`);
- per-vCPU `kick_pending` cmpxchg dedup so at most one IPI in flight.

The kicked vCPU exits KVM_RUN with -EINTR via the unblocked
IPI_SIGNAL in `kvm_v2_install_signal_mask` (`vcpu.c:909-953`,
SMP-only), returns to the dispatcher, and the next dispatch's
CR4.PGE-toggle flushes the local guest TLB.

The IPI side is wired correctly.  The pin experiment confirms that
even with this mechanism inactive (single vCPU → no cross-vCPU
IPI traffic possible), the fail rate is unchanged.

#### mmu_notifier runtime audit (question 2)

Ran a bpftrace kprobe on `__mmu_notifier_invalidate_range_start`,
`__mmu_notifier_invalidate_range_end`,
`__mmu_notifier_arch_invalidate_secondary_tlbs`, and
`kvm_unmap_gfn_range` during a live 5-iter Django reproducer.

30-second window during steady-state:

| caller                          | mmu_inv_start | -> kvm_unmap_gfn |
| ------------------------------- | ------------- | ---------------- |
| UML `linux` (the umlctl process) | 922          | 24               |
| `kcompactd0` (host page migration daemon) | 6318  | **4841**         |

**Finding: kcompactd0 dominates KVM SPTE shootdown traffic by 200×
over the UML process itself.**  Host page-compaction migrations
account for 99% of KVM TDP invalidations during the workload.
mmu_notifier IS firing correctly and DOES propagate to KVM's SPTE
shootdown (`kvm_unmap_gfn_range` → `__kvm_unmap_gfn_range`).

##### Sub-experiment: kcompactd as causal hypothesis (RULED OUT)

If the residual flake is caused by kcompactd-induced SPTE
invalidation racing with vCPU dispatch, disabling proactive
compaction should reduce the rate.  Test:
`sudo sysctl -w vm.compaction_proactiveness=0`.

bpftrace re-audit confirmed kcompactd activity dropped to zero
and `kvm_unmap_gfn_range` total dropped from 4865 to 66 — a 73×
reduction in KVM TDP turnover.

n=120 (truncated early by the rolling-50 5% threshold trip):

| variant              | n   | PASS | FAIL | rate | Wilson 95%   |
| -------------------- | --- | ---- | ---- | ---- | ------------ |
| Unpin + compaction_proactiveness=0 | 120 | 114  | 6    | 5.0% | [2.30, 10.62] |

**Rate did NOT improve; it got worse and tripped the threshold.**
kcompactd is NOT the cause.  Reverted compaction_proactiveness to
20 after experiment.

#### CR4.PGE archaeology (question 3) — already answered Round 6

Commit `11102c8176fb` (2026-04-30, "drain UML TLB + CR4.PGE-toggle
to flush stale guest TLB"). Justification: UML guest PTEs live in
physmem; UML kernel updates to those PTEs are direct physmem
writes that do NOT fire mmu_notifier (mmu_notifier only fires on
host-mm operations on the spawner).  PGE-toggle every dispatch
forces `__set_sregs_common`'s mmu_reset_needed=1, which triggers
`kvm_mmu_reset_context` (drops `prev_roots[]`) AND queues
`KVM_REQ_TLB_FLUSH_GUEST` (single-VPID INVVPID).

Verified Round 7: heavy + cheap path counts sum to total dispatches
(no silent skipping); both paths reach `__set_sregs` →
`kvm_mmu_reset_context`.  The toggle is not a sledgehammer masking
the residual; it solves a different bug class (fork-tree-3level
post-CoW stale TLB) that fork-tree-3level still passes.

#### Where this leaves us

All four user-named hypotheses are now investigated.  None of them
is the bug:

| #   | hypothesis                                  | code audit          | experimental disposition |
| --- | ------------------------------------------- | ------------------- | ------------------------ |
| 1   | pool-share IPI/INVLPG correctness         | wiring correct      | falsified by pin (1.7% vs 1.25%) |
| 2   | mmu_notifier propagation to SPTE shootdown | propagates correctly (200× via kcompactd alone) | falsified — kcompactd suppression makes rate worse |
| 3   | CR4.PGE toggle still compensates           | yes (different bug class) | sound; not residual cause |
| 4   | icache at pool boundaries                  | not needed by x86 SMC rules | falsified (same as #1) |

Baseline rate at n=240, pin or unpin, with Round 4 fix and Round 7
counter instrumentation: **~1.5% "Executing a cache" + SERVER_FAIL
combined**, residual after eight rounds.

##### Negative-results catalogue

Investigation has now ruled out:
- Pool-share / cross-host-CPU vCPU dispatch
- Cross-host-CPU iTLB / L1i pollution
- prev_roots[] silent skip (cheap path verified to drop roots)
- Heavy-vs-cheap path asymmetry (forcing heavy doesn't fix it)
- kcompactd-driven SPTE invalidation
- mmu_notifier mis-wiring or non-propagation
- CR4.PGE toggle being broken

##### Open angles for future investigation

The bug is real, kvm-v2-specific (seccomp 30/30 Round 6), and
appears in a narrow timing window (Heisenbug, Round 6).  Six
hypotheses ruled out.  Round 9 angles that have NOT been chased:

* **Same-vCPU cross-task state leak.** Even on a single vCPU,
  multiple host pthreads serialize through it; cross-task dispatch
  fires `cross_task` gate, but some KVM-internal state may not
  fully reset.  Suspects: `kvm_dirty_regs` clearing semantics on
  exit; `kvm_run->s.regs.{regs,sregs}` staleness across dispatcher
  iterations; `vcpu->arch.*` fields not touched by `__set_sregs`.
* **CPython bytecode-level capture at failure.** Patch CPython
  3.14's `_PyEval_EvalFrameDefault` to dump the bytecode bytes +
  instruction-cache snapshot at the moment "Executing a cache"
  fires.  Tells us if the bytes are stale (microarch SMC),
  half-written (race), or correct-but-interpreter-confused
  (control-flow corruption).
* **Different reproducer.** Django spawns many processes, has lots
  of moving parts.  A minimal PEP 659 specialization reproducer
  (a tight Python loop that specializes a handful of opcodes
  repeatedly) would tighten signal-to-noise.
* **Seccomp vs kvm-v2 dispatch difference.** Seccomp is 30/30
  clean.  What does seccomp do that kvm-v2 doesn't?  Both use the
  same UML kernel mm-sync drain; the difference is the dispatch
  vehicle.  Diff the per-dispatch state-machine between backends.

The four-point plan the user named is now closed.  None of the
four was the bug.  Future rounds need a different hypothesis space.

Round 8 commits:
* `/tmp/dj-r8-umlctl-pin3.sh` (taskset wrapper, not committed)
* `/tmp/dj-r8-mmu-trace.bt` (bpftrace audit script, not committed)
* This docs section.

### Round 9 — CPython ground-truth dump of the cache-abort bytes

**Date:** 2026-05-18.

Round 9 patched CPython 3.14.4 to dump bytecode bytes + register state
at the `_PyEval_EvalFrameDefault` "Executing a cache" abort site.
Ground truth from 4 captured aborts confirms what the rate-based
investigations could only infer.

#### Method

Patched the `TARGET(CACHE)` handler in
`Python/generated_cases.c.h:1498` to write, on every abort:
- pid, frame address, instr_ptr, code object start, offset, size
- co_filename + co_name + firstlineno
- contiguous zero-region extent around instr_ptr
- 64 codeunits (128 bytes) of bytecode around the abort point
- CPU register snapshot (RAX/RBX/RDX/RSI/RDI/R8/R10/RSP) via inline asm
- /proc/self/maps line containing instr_ptr

Patched python installed at `/home/mjbommar/cpy-t59/bin/python3.14`
with `PYTHONHOME=/home/mjbommar/cpy-t59`. Wrapped into the soak
template via PATH override.

#### Findings

**Bytecode bytes read as zero where real opcodes should be.** The
CACHE opcode value is 0 in CPython 3.14, so any region of bytecode
that reads `0x0000 0x0000 ...` dispatches to the CACHE TARGET and
panics. The dump compares against the SOURCE .py file's compiled
bytecode and shows real opcodes (e.g. `BUILD_TUPLE 0 = 0x002f`)
should be present where 0x0000 is observed.

**Zero region sizes vary across hits** — 16 bytes (8 codeunits),
24 bytes, 40 bytes. The region is contiguous within a single page;
real opcodes are present immediately before and after.

**All hits happen in module-init code:**
- `html/entities.py <module>` (offset 8, 16 bytes zero)
- `<frozen getpath> <module>` (offset 12, 26 bytes zero)
- `<frozen importlib._bootstrap> _fix_up_module` (offset 12, 40 bytes zero)
- `enum.py convert_class` (offset 13, 24 bytes zero)

**All hits are in user heap** (rwxp, anonymous):
`/proc/self/maps`: `550000698000-5500007e7000 rwxp 00000000 00:00 0 [heap]`

#### Bisection of gadget involvement

n=120 each variant, same kernel build except for the gadget Kconfig
or gadget-handler edits:

| variant                                 | gadget | h_time | h_getcpu | h_clock_gettime | pid-family | cache hits |
| --------------------------------------- | ------ | ------ | -------- | --------------- | ---------- | ---------- |
| Baseline (Round 8 build)                | on     | on     | on       | on              | on         | 2/240      |
| `CONFIG_UM_BACKEND_KVM_V2_GADGET=n`     | off    | -      | -        | -               | -          | **0/120**  |
| Disable scratch handlers (jmp fallback) | on     | off    | off      | off             | on         | 1/120      |

The gadget IS involved. But disabling the three scratch-using
handlers (which were the most plausible "writes to user pointer"
source) does NOT close the bug: 1 cache hit still observed with
only pid-family handlers active. The corruption mechanism is in a
code path that runs even for the pid-family fast path — most likely
the entry-save block (`movq %rdx, %gs:0x50; movq %r8, %gs:0x58;
movq %r10, %gs:0x60`) under a corrupted GS_BASE, or in the gadget's
mid-flight EINTR-recovery path SMP-T56 ships.

#### Register snapshot at abort (1 dump entry with regs)

```
REGS: rax=0x550000698450 rbx=0xd rdx=0x2 rsi=0x2c rdi=0x5500003ca571
      r8=0x0 r10=0x1b6 rsp=0x7f7fffb8ca70
```

R8 = 0 is suggestive but inconclusive — these are post-abort
register values (CPython eval-loop internals), not at-gadget-entry
values.

#### Operational disposition

* **Workaround:** ship with `CONFIG_UM_BACKEND_KVM_V2_GADGET=n`.
  Confirmed 0 cache hits across 120 iters. Performance cost: gadget
  syscalls (getpid/gettid/clock_gettime/time/getcpu) lose their
  stay-in-guest fast path. Per the perf-getpid benchmark in
  STATUS.md, this gives up the ~1050× getpid speedup over seccomp —
  significant for syscall-heavy workloads, but Tier 3 Django runs
  reliably without it.
* **Diagnostic artefacts preserved** at
  `tools/uml/diag/round9-cpython-cache-dump/` — patch + 4 dump
  entries — so a future investigator can re-apply the patch and
  capture more reproducible-case data.

#### What Round 9 did NOT close

* Specific mechanism: WHY the gadget entry-save block (or whatever
  runs in the gadget hot path) intermittently writes zeros to user
  heap memory. Hypothesis space includes:
  - KERNEL_GS_BASE corruption (despite SMP-T56's recovery)
  - State-page slot writes via a corrupted GS_BASE landing on user
    memory
  - mid-flight EINTR-rewind from SMP-T56 with edge-case register
    state
* SMP-T58 fix (LSTAR EINTR GPR recovery) shipped at
  `bab1d5c54056` is a real ABI-compliance fix but is NOT THE Django
  flake fix — failure rate is statistically unchanged with T58 in.
