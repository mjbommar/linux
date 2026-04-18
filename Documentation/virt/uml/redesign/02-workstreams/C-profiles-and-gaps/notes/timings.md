# Build / test timings — workstream C

Format: `<date> | <task> | <wall> | <jobs> | <notes>`.

Hardware: 4 CPU, 61 GiB RAM, Ubuntu 26.04, gcc 15.2.

| Date (UTC) | Task | Wall | -j | Notes |
|---|---|---|---|---|
| 2026-04-18 | C-01 design + base_defconfig + 8 fragments + Makefile wiring | ~15 min | – | No compile; text-only Kconfig work + 1-line top-level Makefile patch to allow `uml/%` in no-dot-config-targets |
| 2026-04-18 | C-01 user docs (profiles/*.rst) | ~10 min | – | 9 files in Documentation/virt/uml/profiles/ + Sphinx toctree; htmldocs clean |
| 2026-04-18 | C-01 selftest scaffolding (tools/testing/selftests/um/profiles/) | ~5 min | – | probe-features.sh + run-profile-checks.sh + Makefile wiring |
| 2026-04-18 | C-01 build matrix: 8 profiles clean `make ARCH=um uml/<p> && make -j4` | see notes/profile-matrix.md | 4 | Per-profile cold build; each profile starts from an empty O= tree. Sequential to avoid host CPU contention. |
