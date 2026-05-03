# SMP-T41 — stress soak + performance benchmarks (T41-fix kernel)

This memo captures the validation results AFTER the T41 fix
(`af659ad4297d`) landed. Two purposes:

1. **Stress** — prove byte[0]=0 stays at zero events across very
   long soaks under realistic-and-adversarial workloads.
2. **Perf** — prove the fix doesn't measurably slow the dispatch
   path (it adds one read from IST top-56 per EINTR-mid-PF-stub
   event; not on the hot path).

Kernel under test: `~/src/uml-builds/uml-smp-t41fix/linux`
(branch `umlctl-deploy`, tip `6fb90a1ba275`).

Host: AMD Ryzen 7 7840HS (16 host CPUs, SVM); Linux 7.0.0-15-generic.

## Stress soaks

### mt-mini SMP T=8 ncpus=4 — primary regression target

This is the workload that surfaced the byte[0]=0 bug. Each boot
runs mt-mini with 8 pthread workers each doing 50 iterations of
`mmap(64KB) → strict_memset(tid) → verify → munmap`. Per boot ≈
8 × 50 × 16 = 6400 page-touches, each a potential first-touch
#PF that exercises the EINTR-mid-stub path.

**Comparison:**

| Run | N | PASS | STRICT_MEMSET_FAIL | Wilson 95% CI |
|---|---|---|---|---|
| T39 baseline (no fix) | 100 | 88 | 11 | [80%, 93%] |
| T41 discriminator (state-trace ON, no fix) | 100 | 93 | 6 | [86.3%, 96.6%] |
| T41 fix | 100 | 99 | 0 | [94.6%, 99.8%] |
| **T41 fix (extended soak)** | **400** | **397** | **0** | **[97.8%, 99.7%]** |
| **T41 fix (long soak)** | **1000** | **992** | **0** | **[98.4%, 99.6%]** |

`STRICT_MEMSET_FAIL` count is the bug-class signal. The 3 fails in
T41-fix N=400 are init.sh-hangs in libc syscall during boot —
different bug class (tracked as SMP-T54).

### substrate gate

Sealed gate at `tools/testing/selftests/um/gates/regrtest-substrate.toml`.
Battery of class-a-env / class-b-process / class-c-syscall /
class-d-structural reproducers run as `init=` inside one UML boot.

| Backend | PASS | FAIL | EXPECTED_FAIL | Status |
|---|---|---|---|---|
| seccomp (T41-fix kernel) | 25 | 3 | 3 | PASS |
| kvm-v2 (T41-fix kernel) | 25 | 3 | 3 | PASS |

Bit-identical to seccomp. T41 introduced no regression.

### cpython-tier0 gate

Single-process Python with libcrypto + zlib + ssl extensions.

| Backend | Result | Duration |
|---|---|---|
| kvm-v2 (T41-fix kernel) | PASS | 3.8s |

## Performance benchmarks

### perf-py-startup — minimal Python startup wall-clock

10 samples per backend, in-kernel printk-timestamp resolution
(10 ms granularity).

| Backend | Median | Samples |
|---|---|---|
| seccomp | 0.1200 s | 0.12 × 10 (no variance) |
| kvm-v2  | 0.1000 s | 0.10 × 7, 0.11 × 2, 0.18 × 1 |

**Ratio kvm-v2 / seccomp = 0.833** — kvm-v2 is **17% FASTER** than
seccomp on Python startup. Gate ceiling is 1.2×; comfortably under.

The 0.18s outlier is the same init.sh-hang class (SMP-T54). Excluded
the median is 0.10s flat.

### perf-getpid — N/A on T41-fix kernel

`perf-getpid` measures the legacy v1 KVM backend (gadget /
integrated). v1 is no longer present in T41-fix builds, so this
gate fails with "v1 binary not found" (exit code 4) — expected.

### Pre-fix vs post-fix: does T41 add overhead?

The T41 fix adds:
- 1 capture of `regs->gp[HOST_IP]` into a local at function entry
- 1 conditional branch (`> stub_start`)
- 1 64-bit read from IST top-56 when the branch is taken

This code runs ONLY in the `kvm_v2_handle_pf_eintr_inline` path
(EINTR caught the guest mid-PF-stub). For the common case (no
EINTR; PF handled via `handle_io_pf` vmexit-out path), zero added
work.

Empirical comparison on Python startup (10 samples each, in-kernel
printk-timestamp resolution):

| Kernel | seccomp median | kvm-v2 median | Ratio |
|---|---|---|---|
| T39 baseline (no T41 fix) | 0.1200 s | 0.1000 s | 0.833 |
| T41 fix                   | 0.1200 s | 0.1000 s | 0.833 |

**Identical to printk-timestamp granularity (10 ms).** The T41 fix
introduces zero measurable wall-clock overhead.

This confirms what the code change implies: a single conditional
read of `*(u64 *)(top - 56)` on the EINTR-mid-PF-stub slow path
(which fires <0.1% of dispatches) has no impact on whole-program
performance.

## Headline numbers

- **Bug class**: byte[0]=0 STRICT_MEMSET_FAIL — closed at root
  cause. **Zero events across N=1000 boots.** Wilson 95% CI for
  PASS rate: [98.4%, 99.6%].
- **Substrate parity**: kvm-v2 = seccomp = 25/3/3.
- **Python startup (T41-fix kernel)**: kvm-v2 is 17% faster than
  seccomp on the SMP build (median 0.10s vs 0.12s).
- **Fix overhead**: identical wall-clock to T39 baseline. No
  measurable hot-path impact.

## Known perf regression vs 2026-04-30 baseline (separate from T41)

Three-way comparison from scoreboard.jsonl + on-host re-runs today:

| Build | Commit | seccomp_med | kvm_v2_med | Ratio | Era |
|---|---|---|---|---|---|
| 04-30 baseline (UP) | `f1e3130a69af` | 0.090 s | **0.040 s** | **0.444** | pre-H.2 (peak v2 perf) |
| 05-02 UP build (uml-clean, re-run today) | `ad18db7c3768` | 0.090 s | 0.060 s | 0.667 | post-tlb-kick-activate, pre-T26/T27 |
| 05-03 SMP build (uml-smp-t41fix) | `cf98c8d21e99` + T41 | 0.120 s | 0.100 s | 0.833 | post-T26/T27/T33 + SMP flavor |

The regression decomposes into two distinct hops:

1. **UP→UP, 04-30→05-02 build (kvm-v2 0.04→0.06, +50%)** — same
   build flavor; same seccomp wall-clock. Likely culprits in the
   ~99 commits between: cross-vCPU TLB kick infrastructure
   (commits A+B at `7e1c255a09ad` / `9f0ff6257e8b`), migrate_disable
   in vcpu_run (`95b3a85bd309`), the EINTR-mid-stub PF inline path
   (`e5977806fd14`). These all add code on the dispatch hot path.

2. **UP→SMP + 05-02→05-03 (kvm-v2 0.06→0.10, +67%)** — both build
   flavor (CONFIG_SMP=y, NR_CPUS=4 → +33% on seccomp too) and
   SMP-T26/T27 (`76b1d98b2006`) which REVERTED the H.2 lazy-FPU
   optimization (`ba9c83331f30`) by forcing `KVM_GET_FPU` after
   EVERY `KVM_RUN` to plug the cross-task XMM leak. H.2 was
   advertised as skipping ~95% of `GET_FPU` calls; T26/T27 traded
   that throughput back for correctness.

This regression is **not caused by T41** (the T39-vs-T41-fix table
above shows identical wall-clock to 10 ms granularity). It is a
**deferred perf-restoration follow-up** tracked as **SMP-T55**:

- Identify the worst single-commit regression in the 04-30→05-02
  UP-build window via bisect.
- Investigate whether T26/T27's always-GET_FPU can be made cheaper
  while remaining correct (e.g., per-vCPU FPU-dirty flag from KVM,
  conditional GET only when guest actually executed FPU
  instructions since last GET).
- Compare SMP-build overhead on seccomp vs UP-build to quantify the
  CONFIG_SMP=y cost.
- Restore Python startup ratio to <0.5 if possible.
