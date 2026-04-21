# Workstream C: Profiles + gap-fill

**Effort:** ~8 engineer-months (the widest workstream)
**Owner role:** Multiple porters working in parallel
**Critical path:** Yes for profiles; gap-fill ports parallelize freely
**Bookend:** All defconfigs build cleanly from one tree;
            profile-appropriate test passes for each

## What this workstream produces

The seven defconfigs (`prod-fast`, `prod-with-hooks`, `research`,
`fuzz`, `fuzz-deep`, `sandbox`, `library`, `embedded`,
`time-travel`) plus the upstream ports that profiles need:
KFENCE, KCSAN, kprobes, ftrace, BPF JIT, KMSAN. Plus syzkaller's
`vm/uml` backend, snapshot/forkserver host launcher, and
crosvm-style process-per-device sandbox launcher.

This workstream is wide because most ports are independent.
Sequencing is mostly first-come-first-merged; no single port blocks
the others.

## Tasks

| # | Task | Effort | Status | Parallelizable |
|---|---|---|---|---|
| 01 | [Defconfig design + skeleton](01-defconfig-design.md) | 3 wk | **landed (2026-04-18)** — base + 8 fragments + `uml/<profile>` Makefile targets + 8 user docs + selftest | No (gates rest) |
| 02 | [Port KFENCE](02-port-kfence.md) | 2 wk | **landed (2026-04-18)** — HAVE_ARCH_KFENCE; research + fuzz-deep enabled; 13/27 KUnit pass (12 fail on UML stack-walker limit, KFENCE itself fully functional) | Yes |
| 03 | [Port KCSAN](03-port-kcsan.md) | 4 wk | **landed (2026-04-18)** — HAVE_ARCH_KCSAN; `race` profile (per D26); selftest 3/3 PASS; KUnit 20+ races | Yes |
| 04 | [Port kprobes](04-port-kprobes.md) | 4 wk | **partially landed (2026-04-20)** — HAVE_KPROBES (commits 1a-1d) + HAVE_RETHOOK/KRETPROBES (commit 2) in tree; commit 3 HAVE_FUNCTION_GRAPH_TRACER **deferred per D34** (upstream generic-fgraph conflict with UML's longjmp-based task entry) | Yes |
| 05 | [Port ftrace](05-port-ftrace.md) | 4 wk | **landed (2026-04-19)** — HAVE_FUNCTION_TRACER + HAVE_DYNAMIC_FTRACE; research + fuzz profiles enabled; selftest ftrace-smoke PASS | Yes |
| 06 | [Port BPF JIT](06-port-bpf-jit.md) | 3 wk | **deferred (2026-04-20)** — per D43. Two arch-generic hygiene commits landed on-branch (`bpf, x86: explicitly include <asm/cpufeature.h>` + `bpf, x86: use instruction_pointer helpers`), prepared as a submission series at `upstream-patches/bpf-hygiene-v1/` for LKML. Full UML port blocked on option B2 portable-emitter refactor of `arch/x86/net/bpf_jit_comp.c` — needs upstream coordination with BPF maintainers. | Yes |
| 07 | [Port KMSAN](07-port-kmsan.md) | 6 wk | **design-locked (2026-04-21)** — four probes documented in D44; phase-1-mmap pattern doesn't fit KMSAN's per-page metadata contract. Implementation-queued. Revised approach (recorded in D44 fourth probe): defer shadow setup to generic `kmsan_init_shadow`/`_runtime` at `mm_core_init` timing + map-on-demand arch callback hooking `kmsan_record_future_shadow_range`. Needs ~1 week focused work + upstream `mm/kmsan/init.c` arch-extension point (Alexander Potapenko). | Last (heaviest) |
| 08 | [Syzkaller `vm/uml` backend](08-syzkaller-vm-uml.md) | 4 wk | planned (external to linux.git — lives in the syzkaller repo) | Yes (early!) |
| 09 | [Snapshot/forkserver host launcher](09-snapshot-forkserver.md) | 6 wk | **landed v1 (2026-04-20)** — kernel-side plumbing + AFL-compatible 12-byte wire protocol on fds 198/199 + `/sys/kernel/um/state_version` + per-FD disposition taxonomy + snapshot-smoke selftest + fuzz/fuzz-deep defconfig wire-up. v1 ceiling: short non-blocking guest programs (single-iteration forkserver). Sustained fuzz + multi-iteration gated on v2 freezer-cgroup redesign per D41/D42. | Yes |
| 10 | [Crosvm-style host launcher](10-host-launcher-crosvm.md) | 6 wk | **landed v1 (2026-04-20)** — `tools/uml/uml-launcher/` Rust binary (clap v4 + figment + shared_child + signal-hook + tracing + anyhow + nix). Supports `uml-launcher run --kernel --init --mem --console --forkserver` end-to-end + TOML config merge + SIGINT/SIGTERM forwarding + exit-code passthrough. User doc at `Documentation/virt/uml/launcher.rst`; launcher-smoke Part A+B+C selftest. v2 (per-device vhost-user + seccomp + AppArmor) explicitly deferred. | Yes |

Total: ~42 weeks ≈ ~10 EM if serialized; ~8 EM with reasonable
parallelism (3 porters working in parallel after C-01).

## Sequencing

```
C-01 (defconfigs)    ─── must merge first; everything else needs Kconfig wiring
   ├── C-02 KFENCE   ─── 2 wk, easy
   ├── C-03 KCSAN    ─── 4 wk, hard (needs SMP)
   ├── C-04 kprobes  ─── 4 wk, hard (needs section split B-04)
   ├── C-05 ftrace   ─── 4 wk, hard (needs section split B-04)
   ├── C-06 BPF JIT  ─── 3 wk, medium
   ├── C-07 KMSAN    ─── 6 wk, hardest (do last)
   ├── C-08 syzkaller ── 4 wk, can start anytime after A complete
   ├── C-09 snapshot ── 6 wk, parallel
   └── C-10 crosvm   ── 6 wk, parallel (host-side)
```

C-04 and C-05 depend on workstream B's `.text` section split
(B-04). Other ports are mostly independent.

## Milestones

- **Month 1**: Defconfigs skeleton; one CI build per profile
  (may not boot useful payloads yet)
- **Month 3**: KFENCE, KCSAN, syzkaller-backend merged;
  research profile boots fully featured
- **Month 5**: kprobes, ftrace, BPF JIT merged; tracing parity
  with QEMU-KVM achieved
- **Month 7**: snapshot/forkserver and crosvm launchers
  shipping; fuzz and sandbox profiles validated
- **Month 8**: KMSAN merged; all 9 defconfigs deliver their
  intended profile

## Open questions

- **Q1**: Do we ship `prod-fast` before D (KVM backend) lands?
  (Plan: yes — `prod-fast` falls back to seccomp until KVM
  available. Document that "prod-fast" is currently
  "best-available-fast" in v1.)
- **Q2**: Library profile (C-01) — can it land without LKL
  upstream blessing? (Plan: yes; we ship as a sibling artifact
  under `arch/um/library/`. Talk to LKL maintainers about
  eventual unification.)
- **Q3**: What's the minimum host-userspace target for the
  crosvm launcher? (Plan: just-published distros — bookworm,
  Ubuntu 24.04, Alpine 3.19. Older systems use `embedded`
  profile.)

## What success looks like

After this workstream:

- `make ARCH=um uml/research` produces a functional debug-friendly
  UML.
- `make ARCH=um uml/fuzz` plus syzkaller's `vm/uml` backend runs a
  syzkaller corpus at >1000 iter/s with KCOV recording.
- `make ARCH=um uml/sandbox` produces a minimized-TCB UML
  suitable for hostile workloads.
- `make ARCH=um uml/library` produces `liblinux.a` callable from
  fuzzer harnesses.
- The matrix in `03-profiles/README.md` is fully validated.

## What failure looks like

- KMSAN proves intractable; we ship without it. Acceptable; KMSAN
  is the heaviest port and not all profiles need it.
- syzkaller backend stalls on lifecycle-management complexity
  (panic capture, console handling). Mitigation: incremental
  shipping; even 50% of QEMU-KVM's syzkaller features is a
  meaningful improvement.
- One profile defconfig doesn't actually deliver its promise
  (e.g., sandbox isn't actually small). Iterate on Kconfig
  audit.
