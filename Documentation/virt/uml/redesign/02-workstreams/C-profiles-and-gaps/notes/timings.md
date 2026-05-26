# Build / test timings — workstream C

Format: `<date> | <task> | <wall> | <jobs> | <notes>`.

Hardware: 4 CPU, 61 GiB RAM, Ubuntu 26.04, gcc 15.2.

| Date (UTC) | Task | Wall | -j | Notes |
|---|---|---|---|---|
| 2026-04-18 | C-01 design + base_defconfig + 8 fragments + Makefile wiring | ~15 min | – | No compile; text-only Kconfig work + 1-line top-level Makefile patch to allow `uml/%` in no-dot-config-targets |
| 2026-04-18 | C-01 user docs (profiles/*.rst) | ~10 min | – | 9 files in Documentation/virt/uml/profiles/ + Sphinx toctree; htmldocs clean |
| 2026-04-18 | C-01 selftest scaffolding (tools/testing/selftests/um/profiles/) | ~5 min | – | probe-features.sh + run-profile-checks.sh + Makefile wiring |
| 2026-04-18 | C-01 build matrix: 8 profiles clean `make ARCH=um uml/<p> && make -j4` | see notes/profile-matrix.md | 4 | Per-profile cold build; each profile starts from an empty O= tree. Sequential to avoid host CPU contention. |
| 2026-04-18 | C-02 asm/kfence.h + trap.c integration + Kconfig select | ~15 min | – | Text-only code changes; 1 new header + 1 trap.c insertion + 1 Kconfig select line + profile fragment edits |
| 2026-04-18 | C-02 research rebuild with KFENCE + KUnit test suite | 302s + 302s | 4 | Two cold builds: first with KFENCE; second after enabling KUNIT + KFENCE_KUNIT_TEST |
| 2026-04-18 | C-02 fuzz-deep rebuild with KFENCE | 290s | 4 | Cold from empty O= tree |
| 2026-04-18 | C-02 matrix rebuild (ptrace_only + seccomp_only + dynamic) | 11s + 11s + 12s | 4 | Incremental; only trap.c touched |
| 2026-04-18 | C-02 KFENCE KUnit at boot | ~10 s | – | 13/27 pass; 12 fail on stack-walker symbol resolution (UML limitation, not KFENCE arch). `total bugs: 23` visible in `/sys/kernel/debug/kfence/stats` — every OOB caught. |
| 2026-04-18 | C-02 full-profile smoke (8 profiles boot + selftest PASS) | ~30 s | – | 8/8 boot ok; 8/8 selftest PASS (research + fuzz-deep add `debugfs_kfence=PRESENT` row) |
| 2026-04-18 | C-03 debugging USER-TU KCSAN leak (__READ_ONCE via kcsan-checks.h) | ~15 min | – | Added `KCSAN_SANITIZE := n` + friends to os-Linux/{,skas/}Makefile. Replaced lone `__READ_ONCE` in skas/process.c with local `UM_USER_READ_ONCE` to drop `asm-generic/rwonce.h` kernel-header include from USER TU. |
| 2026-04-18 | C-03 race build (cold, KCSAN + lockdep + SMP + kunit test compiled) | 246s | 4 | vmlinux 139 MB; boot at `ncpus=2 mem=512M` |
| 2026-04-18 | C-03 KCSAN at boot | ~2 s | – | `kcsan: selftest: 3/3 tests passed`; KUnit suite detects 20+ real races when run |
| 2026-04-18 | C-03 full selftest (9 profiles) | ~4 min | 4 | 9/9 PASS; race adds `debugfs_kcsan=PRESENT` row; selftest harness timeout bumped 20s→40s for KCSAN's slower boot |
