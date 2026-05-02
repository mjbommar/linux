# UML KVM v2 — Layer 8: SMP T>=N FIXED (root cause + patch)

**Date:** 2026-05-02
**Tip with fix:** `95b3a85bd309`
**Status:** FIXED — root cause confirmed, patch shipped, all reproducers PASS.

## Root cause

`kvm_v2_vcpu_run` (arch/um/backend/kvm-v2/vcpu.c:1505 era) called
`preempt_disable()` to pin the running task to its current host CPU
so the local `cpu`, `vcpu`, and `run` ptrs stayed valid across the
entire dispatch. UML builds with `CONFIG_PREEMPT_VOLUNTARY` and NO
`CONFIG_PREEMPT_COUNT`, so **`preempt_disable()` is a NO-OP** — it
does not actually prevent task migration.

When `handle_syscall` calls `schedule()` (e.g. from `sched_yield`'s
path), the task can be migrated to a different host CPU. On
resumption it continues with **STALE** `vcpu` (and `run`) ptrs
pointing at the ORIGINAL CPU's vCPU. The subsequent
`kvm_v2_marshal_to_kvm_regs` + `KVM_RUN` ioctl operates on the
wrong vCPU, racing with the task that legitimately owns that CPU's
vCPU. Cross-task contamination follows: corrupted user RIP, RAX,
saved-return-address, etc.

## Empirical confirmation

`UM_CPU_MIGRATED` probe at the exit of `handle_syscall` in
syscall_trap.c — fires when `raw_smp_processor_id()` differs
before vs. after `handle_syscall`:

```
UM_CPU_MIGRATED pid=58 sysno=24 cpu_before=1 cpu_after=2
UM_CPU_MIGRATED pid=54 sysno=24 cpu_before=1 cpu_after=3
... (30+ migrations per run)
```

`sysno=24` = `__NR_sched_yield`. Confirmed: the task migrates
mid-`handle_syscall` and the dispatcher's local pointers are stale.

## Patch

`arch/um/backend/kvm-v2/vcpu.c`: replace `preempt_disable()` /
`preempt_enable()` with `migrate_disable()` / `migrate_enable()`
in the three call sites of `kvm_v2_vcpu_run`. `migrate_disable()`
pins the task to the current CPU regardless of preempt-count
semantics; voluntary `schedule()` is still allowed but the task
remains on the same host CPU.

```diff
-	preempt_disable();
+	migrate_disable();
... (and matching enables)
```

## Test results (T=8 ncpus=4)

|                  | Baseline | After fix |
|------------------|---------:|----------:|
| mt-mini × 30     |  ~45-50% |     100%  |
| mt-yieldonly × 15|     ~10% |     100%  |
| mt-rawmmap × 20  |     ~40% |     100%  |
| substrate gate   |  PASS=25 |  PASS=25  |

## Investigation timeline (Layers 1-8)

The state-audit framework systematically narrowed the suspect surface:

- **Layers 1-4** enumerated 149 state items, 50+ operations, ownership
  matrix, suspect register audits.
- **Layer 5** built tooling (cscope/ast-grep/bpftrace/ftrace recipes).
- **Layer 6** ranked 11 bug candidates; recommended C6 → C5 → C1.
- **Layer 7** (SMP-T12) built a state-snapshot trace ring, raw-syscall
  reproducer, kernel-side mmap diagnostics. Tested 5 single-fix
  hypotheses — none resolved. Conclusively isolated bug to
  "kernel→user RAX transport corruption under T>N stress."
- **Layer 8** (this) — built `mt-yieldonly`: minimal pthread test,
  NO mmap, NO memory writes, just `sched_yield()` in a loop. 90%
  failure rate at T=8 ncpus=4. Codex agent suggested the
  `schedule()`-from-`preempt_disable` hypothesis. Verified with
  `UM_CPU_MIGRATED` probe — confirmed cross-CPU migration during
  `handle_syscall`. Applied `migrate_disable()` fix. 100% PASS.

The state-trace + raw-syscall reproducer + kernel diagnostics
infrastructure was the critical machinery that converted "we know
mt-mini fails sometimes under T>=N" into "we know exactly what's
corrupting which register at which exact instruction" in 2 days
of focused work.

## Related cleanups (deferred)

The conditional CS/SS reset added in commit `f780bd5d54fa` was
also a correct fix (CPL=0 leak when CS=kernel + RIP=user) but is
now strictly secondary to the migrate_disable fix. It can stay or
be dropped — left in place for defense-in-depth. The error code
went from `2` (supervisor) to `6` (user) when CS reset was added,
proving the CS issue was real.

The `preempt_disable` → `migrate_disable` change should be audited
in the other phases of v2 too. The `kvm_v2_fpu_capture_for_fork`
and other helpers also use `preempt_disable` and may have similar
issues if they yield (they don't currently, but if a future patch
introduces a yield, they'll silently break).

## Cited file references

- arch/um/backend/kvm-v2/vcpu.c (the dispatcher; fixed in this commit)
- arch/um/backend/kvm-v1-archive/thread.c — v1's per-current vCPU
  lookup (kvm_vcpu_for_current) inherently correct because the per-
  task field moves with the task.
- include/linux/sched.h:2500-2510 — migrate_disable / migrate_enable
- kernel/sched/core.c — full migrate_disable implementation.
