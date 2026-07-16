# UML v2 post-Linus validation ledger — 2026-07-15

This is the authoritative validation ledger for the July 2026 publication
sprint.  It supersedes ambiguous current-status claims only for the exact tree,
binary, host, and toolchain recorded below.  Historical evidence remains useful
for the commits it tested; it is not silently carried forward as current proof.

## Provenance

- branch: `next`
- synchronized integration commit: `efd6ecf10792ff5550607443b9a35f28b3fefb5b`
- post-fix kernel source commit: `cce63a5a6606c`
- validation-harness source tip: `d402dcea1949`
- Linus base: `58717b2a1365d06c8c64b72aa948541b53fe31eb`
- merge base: `58717b2a1365d06c8c64b72aa948541b53fe31eb`
- host architecture: `x86_64`
- host `/dev/kvm`: usable
- noninteractive privilege for TAP tests: available
- raw logs: `/home/mjbommar/.cache/codex/uml-validation-20260715-efd6ecf/`

Source-affecting paths must remain clean while a gate is running; updates to
this ledger do not alter the tested binary.  If a correctness fix changes
`HEAD`, results below are retained as pre-fix evidence and affected gates are
rerun under a new tested-commit subsection before final publication claims are
made.

## Gate ledger

| Area | Gate | Result | Evidence |
| --- | --- | --- | --- |
| provenance | upstream ancestry and clean tree | PASS | Linus base is the merge base; tree clean at start |
| build | `prod-fast` GCC | PASS | post-merge build completed before this ledger |
| build | KVM-v2 + gadget + SMP GCC | PASS | rebuilt warning-free after limiting debugfs-only snapshot VFS exporters to `CONFIG_DEBUG_FS` |
| runtime | `kvm-smoke` | PASS | 5/5 markers after the Linus merge |
| runtime | `userspace-smoke`, forced seccomp | PASS | post-merge smoke |
| runtime | `userspace-smoke`, forced KVM-v2 | PASS | post-merge smoke |
| profiles | all ten profile builds | PASS | GCC profiles plus Clang `research-kmsan`; logs under `profiles/` |
| profiles | all ten runtime feature probes | PASS | `ran=10 skipped=0`; `profiles/runtime-probes.log` |
| KUnit | vector2 and KVM-v2 suites | PASS | focused total 144: vector2 98, marshal 9, byteshape 9, record 24, snapshot 4; 0 fail, 0 skip with real KVM memslots |
| KUnit | unfiltered composition | PASS | 14 suites passed after adjacent-fd setup fix; four snapshot cases skipped only in the no-KVM composition boot, then passed 4/4 in the focused real-KVM run |
| instrumentation | ftrace/kprobes/BPF/KASAN/KFENCE/KCSAN/KCOV | PASS | ftrace 51337 lines; kprobes 200/204; BPF JIT; KASAN 10/13; KFENCE; KCSAN; KCOV 3762 entries |
| instrumentation | `kmsan-smoke` | PASS | `runtime=y reproducer=n` on the fresh Clang profile |
| KVM | snapshot, bounds, MM, state and record/replay gates | PASS | live snapshot/restore/ELF/bench, bounds 9/9, MM 5/5, DF, state trace, record R/R-1 and clock 100/100 pass on purpose-built configs |
| dynamic userspace | loader, CPython tier-0 and userspace gates | PASS | loader, Python 3.14/C extension/fork, and CPython hashlib tier-0 pass on seccomp and KVM-v2 |
| dynamic userspace | broad CPython diagnostic | NON-GATING FAIL | 46,957 tests ran with 56 case failures across 17 modules; 12 modules outside the branch allowlist were dominated by child interpreters using `-E`/`-S` and therefore not seeing the separately unpacked testsuite; useful MM stress, but not publication proof |
| networking | vector2 functional and sandbox gates | PASS | fd, four-queue fd, in-process TAP, and forbidden-host-operation audit pass |
| networking | vector2 TCP publication benchmark | PASS | legacy 45157.5 Mbps, vector2 43457.1 Mbps, ratio 0.962 |
| Tier 3 | Django/FastAPI family, seccomp + KVM-v2 | PASS smoke | real FastAPI/Uvicorn and Django/Gunicorn each pass 51 HTTP checks on both backends; this is smoke evidence, not a long-duration framework claim |
| pool/fork | template, fork, pool and benchmark gates | PASS | fork/pivot/member/replication, daemon/exec/port/mconsole and benchmark 5/5; Vector2 pool TAP passes 3/3 after readiness fix |
| syzkaller | `syzkaller-shim-smoke` | PASS | take/exec/status/destroy wire path |
| RSS/MM | post-workaround stress | PASS | KVM MM 5/5; fork stress 522/522 with 406 child PIDs and 0.00% drift; final mt-mini KVM-v2 p50 305 ms and seccomp p50 1038 ms with zero strict/verify failures; no RSS warning or repair marker |
| post-fix | focused rerun | PASS | warning-free lean build, KVM smoke 5/5, CPython tier-0 on seccomp and the `kvm` alias, Vector2 KUnit 98/98, snapshot real-KVM 4/4, KVM MM 5/5, pool TAP, cargo tests, and unfiltered KUnit all pass |
| soak | bounded mission soak | PENDING | superseded by focused gates for this fix set; retain as an optional CI-duration check |
| soak | first natural 24-hour KVM-v2 attempt | INVALID / STOPPED | stopped after 31 minutes and 460 rows: memcheck 100/100 PASS, iocheck 100/100 PASS, while stress-ng 0/100, CPython 0/80, and kbuild 0/80 never started their intended workload because hostfs prerequisites were absent or hidden; controlled init exit was also misclassified as PANIC |
| soak | all-workload KVM-v2 preflight | PASS | corrected harness at `d402dcea1949`; one fresh iteration each of memcheck, iocheck, stress-ng, CPython, and 64-bit tiny kbuild passed 5/5 with `panic=false`; `soak-preflight-kvm-v2-20260715-r7/` |
| soak | corrected bounded KVM-v2 soak | PASS | corrected pushed tree `48a94005bf02`; gracefully stopped at the agreed three-rotation boundary after 9,075 seconds (2h 31m 15s); 300/300 passed, comprising 60 each of memcheck, iocheck, stress-ng, CPython, and 64-bit tiny kbuild, with zero panic, timeout, host error, threshold, or thermal events; `soak-kvm-v2-24h-48a94005bf02-r8/` |

Three complete corrected rotations are the bounded exit criterion for this
publication sprint.  A full-day soak remains useful as optional CI evidence
for rarer intermittency, leak, or heat effects, but is not a blocker for the
next upstream-preparation work.

## Correctness and publication blockers

| Item | State | Publication condition |
| --- | --- | --- |
| unused snapshot ELF-export helper | RESOLVED | debugfs-only VFS exporters are compiled only with debugfs; warning-free rebuild passed |
| `exit_mmap()` UML RSS counter correction | RESOLVED | removed; the old symptom came from cross-mm KVM TLB-generation state, while new-mm setup now resets the sync range, generation, and every per-CPU seen slot; MM/fork/CPython stress shows no counter drift |
| generic `mm/mmu_gather.c` UML deferral | REVIEWING | isolate and document the required MM contract and focused tests |
| generic scheduler fork-worker detach helper | REVIEWING | keep outside the initial KVM core or justify independently with focused tests |

## Defects found during the matrix

- `umlctl` mconsole readiness used 100 ms one-shot client sockets and could
  manufacture a queue of requests whose reply paths had already been removed
  while a forked Vector2 member initialized.  The Vector2 pool-TAP gate failed
  twice at 23 seconds before the fix.  Readiness probes now allow a bounded
  one-second reply window under the same overall deadline; the exact gate then
  passed three consecutive runs.
- `cpython-tier0` treats requested backend `kvm` and observed canonical name
  `kvm-v2` as different backends.  The runner now canonicalizes the alias and
  the exact `CPYTHON_TIER0_BACKEND=kvm` gate passes.
- `cpython-full` accepted `UML_BACKEND` but always left the generated instance
  on seccomp.  The runner now substitutes the requested backend, and its
  template can consume a separately packaged matching CPython testsuite.  An
  optional `CPYTHON_FULL_LOG` path now preserves the guest log before `umlctl
  rm`, so a failed gate does not delete its own primary diagnostic artifact.
- the mmap benchmark consumed `mt-mini`, but that workload was not part of the
  directory's default build.  The Makefile now builds and cleans both stress
  binaries; the final two-backend stress gate passes with zero data failures.
- pipe descriptors are usually adjacent, but UML host helper threads can win an
  fd allocation between the two allocations inside `pipe(2)`.  Multiqueue
  Vector2 KUnit setup now retries until it obtains the adjacent inherited-fd
  layout it intends to test; focused and unfiltered suite compositions pass.
- the checked-in Tier-3 soak templates are framework-shaped stdlib shims, so
  they cannot substantiate claims about real Django or FastAPI.  The existing
  real FastAPI example and the new real Django example are the authoritative
  framework smoke paths for this ledger.
- the first natural soak spent its failure budget discovering missing
  `stress-ng` and CPython testsuite packages, then lost `/usr/bin/awk` when the
  guest's private `/etc` overlay hid `/etc/alternatives`.  Selected workloads
  now fail fast on host and guest prerequisites, the overlay preserves
  alternatives targets, and tiny kbuild explicitly selects the running
  kernel's 64-bit architecture rather than implicitly requiring a 32-bit SDK.
- a failed init phase terminates PID 1 and UML consequently prints
  `Attempted to kill init`; that controlled shutdown is now classified as the
  workload FAIL it is, not as a spontaneous kernel PANIC.  Final summaries
  also distinguish running, requested, threshold, and budget stop reasons.

## Publication boundary

`next` remains the complete integration branch.  Upstream series are rebuilt
from the current Linus base, not mechanically emitted from consolidated
integration commits.  The intended boundaries are:

1. independent generic hygiene fixes;
2. backend abstraction with unchanged seccomp behavior;
3. minimal KVM-v2 execution core;
4. snapshot support as a follow-up;
5. bounded experimental record/replay as a later follow-up;
6. vector2 as an independent networking RFC;
7. `umlctl`, pool, and fork-server operational work documented separately from
   the initial kernel backend series.

The current, size-checked extraction map is recorded in
`2026-07-15-upstream-emission-boundaries.md`.  It supersedes the stale assumption
that the now 14.6-kLoC KVM-v2 directory can be represented honestly by the old
single 17--19 patch plan.
