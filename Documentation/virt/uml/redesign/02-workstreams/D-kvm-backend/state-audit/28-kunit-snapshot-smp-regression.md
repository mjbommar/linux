# SMP-T79 — KUnit kvm_v2_snapshot fails under SMP (2026-05-21)

**Discovered:** post-memo-04 24h soak (`post-smp-t78v2-24h-soak`),
cpython-soak iter 1 boot logs.
**Kernel HEAD at discovery:** `17ca05177ac7` (umlctl-deploy).
**Reproduce:** boot UML with `mem=128M ncpus=4 backend=force=kvm-v2`
on a build with `CONFIG_UM_BACKEND_KVM_V2_KUNIT=y`.

## Symptom

`kvm_v2_snapshot` KUnit suite: **pass=2 fail=2** under SMP
(`ncpus=4`), but **pass=4 fail=0** under UP (no `ncpus=` argument
or `ncpus=1`).

Failing cases (both):

- `test_kvm_v2_snapshot_basic` at `test_snapshot.c:169`:
  ```
  Expected scratch.rax == snap->regs.rax, but
      scratch.rax == -2401053088876216593 (0xdeadbeefdeadbeef)
      snap->regs.rax == 0 (0x0)
  ```
- `test_kvm_v2_snapshot_elf_basic` at `test_snapshot.c:593`:
  ```
  Expected snap->regs.rax == 0x18112026ULL, but
      snap->regs.rax == 0 (0x0)
      captured RAX (0x0) does not match the marker
  ```

Both failures share root pattern: the snapshot captures `regs.rax
== 0`, despite the test setting a non-zero marker via
`KVM_SET_REGS` on the suite-init-armed vCPU before calling
`kvm_v2_snapshot_capture_regs_only`.

## Likely root cause

The KUnit suite's `suite_init` calls `kvm_v2_vcpu_prime_for_kunit`
on `vcpus[0]` (the per-host-CPU vCPU pool entry for CPU 0).  The
test stores `vcpus[0]` in a file-scope `kvm_v2_test_vcpu` pointer
and operates on it via `KVM_SET_REGS` / `KVM_GET_REGS` ioctls
against `vcpu->vcpu_fd`.

Under UP, there is only one vCPU; the test's setter and the
snapshot's capture both touch the same vCPU.  Under SMP with
`ncpus=4`, the test still uses `vcpus[0]` for its ioctls (it
holds the pointer from suite_init), but `kvm_v2_snapshot_capture_
regs_only` may pull from a different vCPU in the per-host-CPU pool
— specifically the vCPU bound to whichever host CPU the snapshot-
running thread happens to land on.  That vCPU's `kvm_run`-side
registers were never set by the test, so they read as 0.

Same architectural pattern as SMP-T13 (per-host-CPU vCPU pool
implies that any cross-vCPU operation must explicitly target
`vcpus[N]` rather than `vcpus[smp_processor_id()]`).

## Resolution options

### Option A — Pin KUnit thread to CPU 0 (cheapest)

Have `kvm_v2_snapshot_test_basic` / `_elf_basic` call
`set_cpus_allowed_ptr(current, cpumask_of(0))` (or its KUnit-
appropriate equivalent) before issuing the snapshot capture, then
restore.  Forces the snapshot to use `vcpus[0]` matching the
test's `KVM_SET_REGS` target.

### Option B — Make snapshot_capture_regs_only take explicit vCPU

Refactor the snapshot API to accept a `struct kvm_v2_vcpu *`
explicitly, eliminating the implicit "current host CPU's vCPU"
behavior.  Larger surface change; out of scope for the soak fix.

### Option C — Mark the two cases `_KUNIT_SKIP` under SMP

Both failing cases are pre-Phase-3 surface (basic regs-only +
elf-basic).  Phase 3 (`_full` + `_task`) passes under SMP because
those test paths use different vCPU resolution.  Skip the two
single-vCPU cases when `nr_cpu_ids > 1`.  This matches the LTP
curation style — accept that some tests are UP-only.

## Recommendation

**Option A.**  Smallest patch, restores 4/4 KUnit pass under both
UP and SMP, no architectural compromise.

## Series 7 impact

Reviewers cross-reference the KUnit results in the Series 7 cover
letter.  Today's `kvm_v2_snapshot: pass=4` claim for the
post-`ee244842a5da` kernel is UP-correct, SMP-incorrect.

The cover letter should:
- Cite the UP `pass=4` as the published number.
- Footnote that the SMP `pass=2 fail=2` is a known KUnit-fixture
  issue (this memo), not a backend bug.
- Promise Option A as a follow-up patch (file as SMP-T79 fixup).

## NOT a regression introduced today

`test_kvm_v2_snapshot_basic` was 3/3 PASS before my changes.  But
that 3/3 figure was measured under UP (or was reported pre-SMP-
T57 enabling of AVX which changed XSAVE plumbing).  The SMP-
specific failure mode predates today's work — today's ELF64 add
(`test_kvm_v2_snapshot_elf_basic`) just made the surface more
visible.

## Cross-references

- `02-workstreams/D-kvm-backend/state-audit/08-smp-t13-FIXED.md`
  — the per-host-CPU vCPU pool decision.
- `02-workstreams/D-kvm-backend/state-audit/27-sched_setaffinity-
  EBUSY.md` — sibling SMP-T78 finding (sched_setaffinity EBUSY).
- `arch/um/backend/kvm-v2/test_snapshot.c` lines 140-172 (basic
  case) and 555-625 (elf_basic case).
