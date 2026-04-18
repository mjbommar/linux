# Build / test timings — workstream B

Format: `<date> | <task> | <wall> | <jobs> | <notes>`.

| Date (UTC) | Task | Wall | -j | Notes |
|---|---|---|---|---|
| 2026-04-18 | B-01 audit (inventory + hot-paths.md) | ~15 min | – | Static grep of um_backend_dispatch + kernel-side handler walk; no compile |
| 2026-04-18 | B-02 incremental build: hooks.h + hooks.c + 6 insertions on DYNAMIC | 9.1 s | 4 | Warm cache; 1 new TU (kernel/hooks.c) + 5 touched TUs; vmlinux still 87 MB (text delta within noise — C fallback adds ~3 insns per gate × 6 sites = ~18 insns) |
| 2026-04-18 | B-02 PTRACE_ONLY build | 9.9 s | 4 | Warm cache; clean |
| 2026-04-18 | B-02 SECCOMP_ONLY build | 10.1 s | 4 | Warm cache; clean |
| 2026-04-18 | B-02 boot smoke DYNAMIC + PTRACE_ONLY + SECCOMP_ONLY | <1 s each | – | init=/bin/true exits cleanly under each; 20/20 KUnit conformance tests pass under each |
| 2026-04-18 | B-03 incremental DYNAMIC with debugfs.o + kconfig DEBUG_FS=y | 2m7s | 4 | Bigger rebuild because enabling DEBUG_FS pulled in a lot of new objects (first-time enable on this tree) |
| 2026-04-18 | B-03 debugfs end-to-end flip test (guest init script) | <1 s | – | Baseline hits=0 → after 4421 hits → flip off → settled. Confirms debugfs toggles the static_key |
| 2026-04-18 | B-04 incremental DYNAMIC (section_split.o + patchable.h + lds edits) | 7.1 s | 4 | Warm cache; linker script change triggers full vmlinux relink |
| 2026-04-18 | B-04 PTRACE_ONLY + SECCOMP_ONLY rebuild | ~10s each | 4 | All three configs show `.um_patch_text` via readelf; boot prints the section split banner |
| 2026-04-18 | B-05 incremental DYNAMIC with hooks_bench.o | 7.0 s | 4 | Warm cache |
| 2026-04-18 | B-05 benchmark inside UML (7 hooks × 2 states × 10 batches × 100k iters) | ~5 s | – | Yields the checked-in baseline in notes/bench-baseline.json; worst per-gate off = 0.68 ns (I3 met) |
| 2026-04-18 | B-06 first-flip kselftest (host-driver) | <2 s | – | PASS: baseline=0 off_delta=0 on_delta=18340 settle_delta=0 |

