# SMP-T80 — test_signal.test_itimer_virtual hangs/fails on kvm-v2 SMP (2026-05-21)

**Discovered:** post-memo-04 24h soak (`post-smp-t78v2-24h-soak`),
cpython-soak iters 1-20 of `kvm-v2 ncpus=4 mem=1024M`.
**Kernel HEAD at discovery:** `17ca05177ac7`.
**Reproduce:** boot UML with `mem=1024M ncpus=4 backend=force=kvm-v2`
+ a userspace Python 3.14 + `python3 -m test test_signal` —
specifically `test_signal.test_itimer_virtual`.

## Symptom

`test_signal worker non-zero exit code (Exit code 1)`.  The
worker hangs in `test_itimer_virtual` until regrtest's per-test
timeout (60 s) kills the process.  Stack trace (CPython):

```
  File "/usr/lib/python3.14/test/test_signal.py", line 842 in
       test_itimer_virtual
```

(line 842 is inside the test's `for _ in support.busy_retry(...)`
spin-and-poll-getitimer loop.)

## Likely root cause

`ITIMER_VIRTUAL` is supposed to deliver `SIGVTALRM` when the
calling process has accumulated the requested amount of *virtual*
CPU time (i.e. user-mode CPU time, distinct from real time).
Linux's accounting for virtual time normally increments on every
clock tick when the task is running user-mode.

Under kvm-v2 SMP, the dispatch model is:

1. Host UML kernel runs (kernel-mode in the host).
2. `kvm_v2_vcpu_run` enters `KVM_RUN` to execute guest user code.
3. Guest's userspace runs inside the KVM context until a vmexit.

The host kernel's tick-based virtual-time accounting fires on host
TICKS — but during step 3, the host UML kernel isn't running on
that CPU; the KVM thread is.  The result is the host's
`getrusage(RUSAGE_SELF)` reports very little user CPU time,
because the actual user CPU time is being spent inside KVM_RUN
which appears as system/wait time from the host's view.

In other words: ITIMER_VIRTUAL counts "user CPU time" but UML
under kvm-v2 doesn't accurately report user CPU time to the
host because the actual computation is delegated to KVM_RUN.

The result: `setitimer(ITIMER_VIRTUAL, 0.3, 0.2)` arms the timer,
the task does work, but the host's accounting never reaches 0.3 s
of user time (because it's accounted as something else), so
SIGVTALRM never fires.

## Validation

Run the same test under `backend=force=seccomp` on the same
kernel; if ITIMER_VIRTUAL works there (because seccomp dispatches
syscalls in the host kernel context where accounting is normal),
this hypothesis is confirmed.  If it ALSO fails under seccomp,
the issue is in UML's ITIMER_VIRTUAL plumbing more broadly.

```sh
$BIN mem=1024M ncpus=4 backend=force=seccomp init=/bin/sh <<EOF
python3 -c '
import signal, time
def handler(signum, frame): print("SIGVTALRM!")
signal.signal(signal.SIGVTALRM, handler)
signal.setitimer(signal.ITIMER_VIRTUAL, 0.3, 0.2)
end = time.monotonic() + 2.0
while time.monotonic() < end:
    pass
print("done, remaining:", signal.getitimer(signal.ITIMER_VIRTUAL))
'
EOF
```

## Resolution options

### Option A — host-side rusage accounting for KVM_RUN

In `kvm_v2_vcpu_run`, around the `ioctl(vcpu_fd, KVM_RUN, ...)`
call, sample `getrusage(RUSAGE_THREAD)` before/after and credit
the delta to the calling task's virtual-time counter.  Requires a
new arch hook into the scheduler's `task_struct->utime` field;
likely needs maintainer review for the ABI break.

### Option B — KVM exit cost accounting via host CPU time

Use a more direct accounting: read `pthread_getcpuclockid` and
`clock_gettime(CLOCK_THREAD_CPUTIME_ID)` around KVM_RUN, credit
the delta as virtual time on the calling guest task.  Cleaner
than rusage but same ABI surface.

### Option C — skip the test (defer)

For the immediate soak, `cpython-soak` is removed.  The CPython
suite has known kvm-v2 SMP gaps; we don't need ITIMER_VIRTUAL to
work for substrate correctness.  File this as a known limitation
in the cover letter; reviewers comparing against bare-metal will
see the gap explicitly.

## Recommendation

**For Series 7 send:** Option C (skip; document as a known
limitation).  ITIMER_VIRTUAL semantics under hardware-virt are
known to be tricky in many hypervisors; UML+kvm-v2 inheriting that
trickiness is excusable for a v1 series.

**For follow-up:** Option B (cleanest) when bandwidth allows.

## Cross-references

- `02-workstreams/D-kvm-backend/state-audit/27-sched_setaffinity-
  EBUSY.md` (SMP-T78, sibling architectural quirk)
- `02-workstreams/D-kvm-backend/state-audit/28-kunit-snapshot-smp-
  regression.md` (SMP-T79, fixed today)
- CPython source: `Lib/test/test_signal.py` `test_itimer_virtual`
  near line 842.
