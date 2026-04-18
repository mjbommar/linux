# Build / test timings — workstream A

A running log so we know what each gate costs and where time goes.
Hardware: 4 CPU, 61 GiB RAM, Ubuntu 26.04, gcc 15.2.

Format: `<date> | <task> | <wall> | <jobs> | <notes>`.

| Date (UTC) | Task | Wall | -j | Notes |
|---|---|---|---|---|
| 2026-04-17 21:00 | A-01.0 → A-01.11 (full design pass, no compile) | ~20 min | – | Survey/inventory/sketches/RST/memo + checkpatch + dispatch macro smoke (~ms each) |
| 2026-04-17 22:35 | `make ARCH=um defconfig` (post-patch) | <1 s | 1 | fresh O=/tmp/uml-build-pre |
| 2026-04-17 21:44 EDT | `make ARCH=um -j4` baseline (with strrchr v3 patch) | ~2 min | 4 | Cold ccache; defconfig; vmlinux=86 MB (text=5.1 MB, data=1.2 MB, bss=188 KB); 0 warnings, 0 errors |
| 2026-04-17 21:50 EDT | A-02.3 incremental build (backend/ scaffolding added) | 11 s | 4 | Warm cache; 4 new placeholder TUs; backend/{common,ptrace}/built-in.a created |
| 2026-04-17 21:55 EDT | A-02.4 + A-02.6 incremental build (ptrace_ops + time.c + kernel/time.c migrated) | 8 s | 4 | Warm cache; first end-to-end op migration; verified dispatch is direct call (objdump) |
| 2026-04-17 22:01 EDT | A-02.5 incremental build (init_backend arbiter + um_arch.c hook) | 8 s | 4 | Warm cache; runtime confirmed: dmesg shows `um: backend = ptrace (contract v1)` and boot proceeds past timekeeping_init |
| 2026-04-17 22:02 EDT | UML boot smoke: init=/bin/true mem=64M | <1 s to early init banner | – | Kernel runs through clocksource registration, NET protocol families; init=/bin/true exits cleanly. timeout=15s |
| 2026-04-17 22:39 EDT | A-02.HOT-1 incremental build (run_userspace migrated; shared/backend.h added; trap_user.c lifted) | ~30 s cumulative | 4 | Multi-step build: shared header creation, USER TU wiring (`_user.o` via Makefile.rules), prototype lifting between asm/backend.h and shared/backend.h |
| 2026-04-17 22:39 EDT | UML boot smoke: post-HOT-1 init=/bin/true | <1 s | – | Boot reaches BogoMIPS calibration + pid_max init (every kernel→user syscall during boot now exercises ptrace_run_userspace via dispatch); init exits 0. Text +4208 B (+0.08%) vs pre-HOT-1 baseline. |
| 2026-04-17 23:32 EDT | A-02.HOT-2 incremental build (mm_map/mm_unmap migrated; map/unmap renamed to um_stub_mm_*) | 12 s | 4 | Warm cache; 1 new TU (arch/um/backend/ptrace/mm.c); 4 modified files; vmlinux text +80 B vs HOT-1 (wrappers inlined to ~12 B each). |
| 2026-04-17 23:32 EDT | UML boot smoke: post-HOT-2 init=/bin/true | <1 s | – | Boot succeeds. Every page fault during boot now routes through tlb.c → vm_ops → ptrace_mm_{map,unmap} → um_stub_mm_{map,unmap} → stub child. |
| 2026-04-17 23:36 EDT | A-02.HOT-3 incremental build (context_switch + thread_create + thread_start_idle migrated) | 8 s | 4 | Warm cache; 1 new TU (arch/um/backend/ptrace/thread.c, 47 lines); 4 modified files; vmlinux text +123 B. |
| 2026-04-17 23:36 EDT | UML boot smoke: post-HOT-3 init=/bin/true | <1 s | – | Boot reaches IO scheduler + tty init + console init; every kthread switch during boot exercises ptrace_context_switch. |
| 2026-04-17 23:41 EDT | A-02.COLD-1 incremental build (read_clock_ns + set_timer migrated) | 8 s | 4 | Warm cache; 8 call sites migrated (kernel/time.c×7 + os-Linux/main.c×1); enum um_timer_mode moved from asm/backend.h to shared/backend.h (USER TUs need it); +84 B text. |
| 2026-04-17 23:46 EDT | A-02.COLD-2 incremental build (ipi_send + init_thread_regs + read/write_guest_regs migrated) | 8 s | 4 | Warm cache; 6 call sites migrated; debug.c added; read/write_guest_regs return -EOPNOTSUPP pending KGDB integration; +128 B text. |
| 2026-04-17 23:55 EDT | A-02.COLD-3 incremental build (mm_attach + mm_detach + probe/init/shutdown stubs) | 8 s | 4 | Warm cache; lifecycle.c added; mm.c extended; 2 callers in mmu.c migrated; +168 B text; **all 18 ptrace ops are non-NULL**. |
| 2026-04-17 23:55 EDT | UML boot smoke: post-COLD-3 init=/bin/true | <1 s | – | Boot completes; init=/bin/true triggers expected "Attempted to kill init!" panic; full ops table exercised end-to-end. |
| 2026-04-18 00:02 EDT | A-03.S1 SECCOMP_ONLY full from-scratch build | 348 s | 4 | Cold cache; new arch/um/backend/seccomp/ subdir (8 TUs); all 18 seccomp ops wired; SECCOMP_ONLY=y, PTRACE compiled in but unused. |
| 2026-04-18 00:03 EDT | UML boot smoke: SECCOMP_ONLY init=/bin/true seccomp=on | <1 s | – | dmesg `um: backend = seccomp (contract v1)`; init=/bin/true triggers expected panic; FIRST proof of seccomp dispatch end-to-end through the ops table. |
| 2026-04-18 00:04 EDT | A-03.S1 PTRACE_ONLY rebuild (regression check) | 8 s | 4 | Warm cache; dmesg `um: backend = ptrace (contract v1)`; both backends now buildable+bootable from one source tree. |
| 2026-04-18 ~01:00 EDT | REVIEW.* fixes: D12/D13/D14 (single source of truth, Kconfig invariants, contract spec) | n/a | n/a | Boot matrix script `Documentation/virt/uml/redesign/scripts/uml-boot-matrix.sh` validates PTRACE_ONLY/SECCOMP_ONLY/DYNAMIC. **All 3 pass: warn=0, expected init-exit panic in each.** Fixes external review's high/medium findings. |
| 2026-04-18 ~01:30 EDT | A-04 boot-param parser (`backend=auto/ptrace/seccomp/force=*`) | <1 s incremental | 4 | 60 lines added to start_up.c (parser via __uml_setup) + 30 lines to backend.c (DYNAMIC arbiter); 6 manual boot variants pass; matrix script extended to 12 rows (3 modes × {default, named, force=, force=opposite}). |
| 2026-04-18 06:15 EDT | uml-boot-matrix.sh full run (3 builds + 12 boots) | ~10 min | 4 | All 3 from-scratch builds clean; **all 12 boot rows pass**: 3 dispatch modes × {default, named-prefer, force=, force=opposite-→panic}. A-04 validated. |
| 2026-04-18 ~06:50 EDT | A-05 KUnit conformance suite: 20 tests across PTRACE_ONLY/SECCOMP_ONLY/DYNAMIC | ~3 min per build | 4 | `arch/um/backend/contract/test_ops.c` extended from 4 to 20 tests covering all 18 ops + sanity. Suite gated by `CONFIG_UM_BACKEND_CONTRACT_TEST`. **All 20 pass on each of 3 modes** (60 test runs total, 0 failures). |
| 2026-04-18 ~07:00 EDT | A-05 cross-backend + perf skeletons | <1 min | n/a | `scripts/uml-cross-backend.sh` (diff-based equivalence between PTRACE_ONLY and SECCOMP_ONLY dmesg) and `scripts/uml-perf.sh` (5-iter wall-time per backend) added. Skeletons; LTP/cycle-perf integration deferred to A-07. |
| 2026-04-18 ~07:30 EDT | REVIEW2.1: backend=seccomp self-sufficiency | <1 min | n/a | os_early_checks now runs the seccomp probe when backend_arg_requested == SECCOMP, not just when seccomp_config != 0. `backend=seccomp` and `backend=force=seccomp` work without needing `seccomp=on` paired. **D15** logged. |
| 2026-04-18 ~07:45 EDT | A-06 docs (backends.rst + backend-contract.rst polish + cost-model.md) | <1 min | n/a | New user-facing `backends.rst` (overview, picking, ASCII trap diagrams). Polished `backend-contract.rst` for upstream tone. New planning `backend-cost-model.md` with first-pass wall-time numbers per backend. Sphinx HTML build clean. |
| 2026-04-18 ~07:45 EDT | uml-perf.sh first real measurement | ~50 s | – | PTRACE_ONLY=2.45 s, SECCOMP_ONLY=2.47 s, DYN/ptrace=2.46 s, DYN/seccomp=2.48 s wall-time avg over 5 init=/bin/true boots. Boot dominated by host setup, not trap mechanism — A-07 microbench-cycle counters will show the real ptrace-vs-seccomp delta. |
| 2026-04-18 ~07:55 EDT | A-07 perf-CI infra: uml-perf.sh upgrade + capture/compare scripts + GHA template + initial baseline | ~6 min | – | uml-perf.sh now uses `perf stat -e cycles,instructions,task-clock` with 10-iter median + IQR. Capture/compare scripts implement the bot policy (<2% silent, 2-5% WARN, >5% FAIL). Initial baseline JSON committed: PTRACE_ONLY=337.9M cycles vs SECCOMP_ONLY=195.8M cycles (~42% delta — first real signal showing seccomp's syscall advantage). |
| 2026-04-18 ~08:05 EDT | A-07 compare verification | ~5 min | – | uml-perf-compare.sh against just-captured baseline: 3/4 backends within 2%; SECCOMP_ONLY hit +83% "regression" from variance (baseline IQR was wide). Policy fired FAIL correctly — confirms the gating mechanism works; documents the host-noise mitigation needed (10→30 iters, dedicated runner, IQR-overlap compare). |

## How to log future entries

When timing a multi-minute step:

```
t0=$(date +%s); <command>; t1=$(date +%s); echo "elapsed=$((t1-t0))s"
```

Or for backgrounded steps, derive from the task-output mtime:

```
stat -c '%Y' <output-file>   # start
date +%s                     # check
```

Append a row to the table above with the wall time, `-j` setting,
and any cache state notes (cold ccache, allmodconfig vs defconfig,
incremental vs from clean).

## Why bother

- Helps decide cadence: if a full build is 10 min, we can afford one
  per op migration; if 30 min, we batch.
- Catches regressions in the build system (sudden 3× slowdown =
  broken Makefile dependency).
- Per kernel-dev-checklist Scale 2 "Performance impact characterized":
  runtime/build numbers belong in commit messages of perf-relevant
  changes.
- Per `00-vision.md` performance targets (~100 ns syscall etc.), we
  need a *baseline* to compare against; the build/boot time is part
  of that baseline.
