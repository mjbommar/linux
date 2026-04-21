# C-06: Port BPF JIT to UML

**Status (2026-04-21 addendum):** option A (the smaller path)
landed on-branch in commits `1a2c96a94008` (shim infrastructure)
and `611d94ef6184` (JIT working end-to-end). A same-day review
pass then caught that the `arch/um/include/asm/segment.h` shim
that option A added silently shadowed `arch/x86/um/asm/segment.h`
and broke the uml/fuzz build. Fixed in commit `48aa69e2d4a7` by
deleting the arch/um shim and extending the x86-UML shim with
the missing selector macros instead. See D46 in
`04-risks/decisions-log.md` for the fix-discipline consequences.
Option B2 (portable-emitter refactor) is still the long-term
shape for upstream; D45 clarifies that fork-first is acceptable.

**Original status (2026-04-20):** deferred; **two arch-generic
hygiene fixes landed on-branch and queued for independent
upstream submission** (`bpf, x86: explicitly include <asm/cpufeature.h>`
and `bpf, x86: use instruction_pointer helpers in ex_handler_bpf`).
The full UML port is blocked on a bigger refactor — see D43
addendum: after the three initial D43 divergences (which the
two landed hygiene patches + a UML-side `<asm/vsyscall.h>`
together resolve) the JIT compile reaches a deeper iceberg of
`arch/x86/kernel/`-internal symbols (`x86_nops[]`,
`DISABLED_MASK_BIT_SET`, probably more: text_poke / unwind /
cfi). Option B1 (3-fix patch) does not work alone as initially
scoped in D43; option B2 (portable-emitter refactor of
`arch/x86/net/bpf_jit_comp.c` splitting byte-emission from
x86-host mitigation glue) is the correct long-term shape —
4-6 weeks, LKML coordination with BPF maintainers. Until B2 is
in flight or landed, C-06 v1 is explicitly deferred. The two
hygiene fixes benefit bare-metal x86 independently and are
ready for LKML submission as standalone patches.
**Effort:** 3 weeks (budget); see "Reality-check" below — actual
scope after investigation is closer to **~3 days of disciplined
work** if the optimistic code-reuse path holds (option B +
upstream review lead time ignored), or **~1 week** if it
unravels and we need arch-specific glue (option A path). Kept at
the 3-week line-item in `00-vision.md` only because Q (unknowns)
is not zero and we do not compress the plan preemptively.
**Dependencies:** B-04 (`.text` section + JIT allocator seam).
                  All landed.
**Blocks:** research profile getting realistic BPF perf; C-08
            syzkaller backend's BPF program coverage; bpftrace and
            `perf probe` working against UML guests; C-09 fuzz
            work that wants to fuzz BPF verifier.

## Goal

`select HAVE_EBPF_JIT if X86_64` for UML. Reuse the host's
`arch/x86/net/bpf_jit_comp.c` x86_64 BPF JIT verbatim — UML emits
x86_64 machine code and executes x86_64 machine code, so the JIT
backend is arch-correct already. Ship a working `bpftool prog
show | grep jit_enabled` on research and fuzz profiles, plus a
selftest that loads a trivial BPF program and confirms it runs
through the JIT, not the interpreter.

## Prior-art survey

### What we reuse unchanged

- **`arch/x86/net/bpf_jit_comp.c`** (~3800 LOC, the JIT itself).
  Emits x86_64 machine code from eBPF bytecode. Arch-agnostic at
  the JIT-emission layer in the sense that the code it writes is
  the code UML runs. The emit pass calls a small number of
  allocator + text-patching hooks all of which are either generic
  (`kernel/bpf/core.c`'s `__weak bpf_jit_alloc_exec`) or have x86
  implementations we want (`<asm/text-patching.h>`'s `text_poke`,
  `<asm/cfi.h>`'s `gen_endbr()`).
- **`arch/x86/net/bpf_timed_may_goto.S`**. A tiny asm helper for
  `bpf_timed_may_goto`; the same binary works on UML.
- **`kernel/bpf/core.c`'s allocator weak defaults.** `__weak
  bpf_jit_alloc_exec(size)` calls `execmem_alloc(EXECMEM_BPF,
  size)`; `execmem_arch_setup()` is not overridden in UML, so
  `execmem_init_missing()` (mm/execmem.c) fills EXECMEM_BPF with
  the EXECMEM_DEFAULT range: `[VMALLOC_START, VMALLOC_END]` with
  `PAGE_KERNEL_EXEC`. That is the right answer: UML's VMALLOC
  area IS executable by construction (UML runs as a host
  userspace process; the VMA is PROT_READ|PROT_WRITE|PROT_EXEC
  for the regions that back kernel `.text` + vmalloc).

### What's different vs. the bare-metal x86 port

- **No W^X transitioning.** Bare-metal x86 uses
  `ARCH_HAS_EXECMEM_ROX` + `bpf_jit_binary_pack_finalize()` to
  allocate RW, copy the finalized code into an ROX alias, then
  flip the RW mapping out. UML does not currently set
  `ARCH_HAS_EXECMEM_ROX`; it keeps the JITed page RWX for the
  lifetime of the BPF program. This is the same posture UML uses
  for kprobes (`arch/um/kernel/kprobes/core.c`'s insn slot
  allocator already accepts EXECMEM_KPROBES as RWX) and for the
  module allocator. **This is a security trade-off we accept
  v1** — UML is a research/fuzz host, not a production kernel.
  If a v2 hardening pass wants ROX for UML, the path is
  `ARCH_HAS_EXECMEM_ROX=y` + the `arch_mprotect` shim over host
  `mprotect()`. Out of scope here.
- **No IBT / FineIBT / CET.** UML does not compile with CFI by
  default (research profile config; verify). `gen_endbr()` from
  `<asm/cfi.h>` expands to a 4-byte NOP when CFI is off; harmless
  in the emitted BPF prog. If CFI ever comes on for UML, the
  endbr sequence lives in the BPF prog prologue and the host's
  CPU ignores it on non-IBT CPUs, or honors it on IBT-on CPUs —
  same as bare metal.
- **Icache sync.** On x86 there is no explicit icache flush
  (self-modifying-code consistency is CPU-level per-core); the
  JIT doesn't emit one. UML inherits this correctness. On ARM64
  UML (future), the JIT would need to call
  `__flush_icache_range()` after each emit; that's a future
  workstream concern, not C-06's.

## Approach — minimal-viable shape

The investigation strongly suggests C-06 v1 needs **no new C
file in `arch/um/`** at all. The three touches are:

1. `arch/um/Kconfig` — `select HAVE_EBPF_JIT if X86_64`
   alongside the existing `HAVE_KPROBES if X86_64` and
   `HAVE_FUNCTION_TRACER if X86_64` selections.
2. `arch/um/Makefile` — pull `arch/x86/net/` into the UML build
   via `core-$(CONFIG_BPF_JIT) += $(HOST_DIR)/net/`. This is the
   same pattern the existing `core-y += $(HOST_DIR)/um/` line
   uses for `arch/x86/um/`. Gated on `CONFIG_BPF_JIT` so non-
   BPF-JIT builds are unchanged.
3. Selftest + user doc in a follow-up commit.

**If (2) doesn't work** — for example because
`arch/x86/net/bpf_jit_comp.c` includes an `<asm/*>` header that
UML doesn't forward, or because the `-mcmodel=large` CFLAG UML
sets interacts with an alignment assumption in the JIT — we fall
back to a small `arch/um/net/bpf_jit_glue.c` that forwards to
the x86 source via `#include`. See §"Unknowns" U3.

## Commit plan (bisectable)

All commits build clean on `ARCH=um` + `ARCH=um LLVM=1`, boot the
research profile to userspace, and don't regress existing
selftests. Per AGENT-PROMPT §2 bisectability rule.

1. **commit 1:** `select HAVE_EBPF_JIT if X86_64` in Kconfig +
   Makefile hookup to arch/x86/net/. Plus a one-paragraph
   comment near the select line referencing this design doc. No
   new C files if (2) works; one trivial `bpf_jit_glue.c` if not
   (U3 fallback). Verifiable: `make ARCH=um uml/research`
   followed by `nm vmlinux | grep bpf_int_jit_compile` shows the
   symbol; boot userspace; `cat /proc/sys/net/core/bpf_jit_enable
   == 1`.

2. **commit 2:** `tools/testing/selftests/um/bpf-smoke/` — load a
   trivial BPF program via bpftool, check it shows `jit_enabled
   1` in `bpftool prog show`. Host-driven; the guest boots with
   bpftool in /usr/sbin (hostfs). Follows the `snapshot-smoke`
   pattern from C-09 commit 5: `TEST_PROGS := run-bpf-smoke.sh`,
   `TEST_FILES := bpf-smoke.sh`. Registered in
   `tools/testing/selftests/um/Makefile` TARGETS list.

3. **commit 3:** `Documentation/virt/uml/bpf-jit.rst` user doc +
   `Documentation/virt/index.rst` toctree entry + verify
   `arch/um/configs/profiles/research.config` has
   `CONFIG_BPF_SYSCALL=y` and `CONFIG_BPF_JIT=y` (add if
   missing). Flip **Status:** in this file to `landed (YYYY-MM-
   DD)` with concrete selftest evidence.

## Validation

Per AGENT-PROMPT §3 Q1 bar on each commit:

- `uml-quality-q1.sh research` clean vs. committed baseline
  (gcc + clang + sparse + smatch).
- `scripts/checkpatch.pl --strict -g HEAD` on each commit.
- `uml-boot-matrix.sh` clean (PTRACE_ONLY / SECCOMP_ONLY /
  DYNAMIC backends all boot).

Per-task validation specific to C-06:

- **Compile symbol visibility:** `nm vmlinux | grep -E
  "bpf_(int_|)jit_compile|bpf_jit_alloc_exec"` — must show
  defined (T) symbols, not undefined (U).
- **Boot-time flag:**
  `/proc/sys/net/core/bpf_jit_enable` exists and defaults per
  `CONFIG_BPF_JIT_ALWAYS_ON`. Test default = 1 on research.
- **Functional proof:** Load a trivial BPF program via `bpftool
  prog loadall /sys/fs/bpf/test ...` or `iproute2`'s `tc filter
  add ... bpf ...`; check `bpftool prog show id N` reports
  `jited: yes` + a positive `jited_len`.
- **Selftest:** `test_progs -t core_reloc` from
  `tools/testing/selftests/bpf/` — a lightweight subset that
  doesn't require kprobes (deferred C-04 commit 3) or tracing
  (C-05 landed, ok to use). Run inside the UML guest.
- **Bpftrace one-liner** (stretch goal, not gating): `bpftrace
  -e 'kfunc:ksys_read { @[probe] = count(); }'` works. Requires
  C-04 commit 3 (HAVE_FUNCTION_GRAPH_TRACER) eventually but not
  for C-06 landing — just kprobe-based bpftrace probes.

## Unknowns

**U1: VMALLOC_START/VMALLOC_END layout on UML at JIT-alloc time.**

- Question: does `[VMALLOC_START, VMALLOC_END]` actually span
  executable memory in UML's post-boot address space, and does
  `execmem_alloc(EXECMEM_BPF, size)` return a page that
  userspace can jump to?
- Probe: commit 1 adds a `pr_info_once` at JIT allocator entry
  dumping the returned VA + the containing VMA's host protection.
  If protection is wrong, `mprotect(PROT_EXEC)` the page before
  returning — or mark this as a real blocker and open a follow-
  up.
- Evidence from existing landings: UML's kprobes port
  (`arch/um/kernel/kprobes/core.c:78` comment) already relies on
  `execmem_alloc(EXECMEM_KPROBES)` returning RWX memory and it
  works in production. EXECMEM_KPROBES and EXECMEM_BPF both
  default to EXECMEM_DEFAULT under `execmem_init_missing()` on
  UML, so if kprobes works, BPF JIT should too. **Low risk.**

**U2: `-mcmodel=large` vs. JIT emit.**

- Question: UML builds with `-mcmodel=large` (arch/um/Makefile
  line 34). Does this change how `arch/x86/net/bpf_jit_comp.c`
  compiles in a way that corrupts the emitted code?
- Evidence: `-mcmodel=large` affects how the C compiler
  generates calls/jumps to 64-bit addresses. The JIT source
  itself emits x86_64 opcodes as byte arrays (`EMIT1_off32`,
  `EMIT_mov`, etc.) — these are not affected by `-mcmodel` since
  they are literal bytes, not compiler-generated. The code that
  moves data around (C-level) compiles under large-model just as
  any other large-model kernel code does. **Very low risk.**

**U3: Header forwarding — do all `<asm/*>` headers used by
arch/x86/net/bpf_jit_comp.c resolve under UML?**

- Probe: `make ARCH=um -j$(nproc) arch/x86/net/bpf_jit_comp.o`
  once Kconfig is wired. If any header is missing, add a
  forwarder to `arch/um/include/asm/Kbuild` or create a thin
  stub in `arch/um/include/asm/`. The most likely suspects:
  `<asm/text-patching.h>` (used by JIT trampolines), `<asm/cfi.h>`
  (used for endbr emission), `<asm/nospec-branch.h>` (used for
  retpoline emission), `<asm/set_memory.h>` (used for RO
  transitioning — but UML path is RWX, so this header's presence
  matters but its set_memory_ro() call may be a no-op).
- Mitigation plan: start with the maximal reuse (no
  `bpf_jit_glue.c`). If any `<asm/*>` header fails to resolve,
  introduce a minimal `arch/um/include/asm/<foo>.h` stub that
  forwards to the x86 header but redefines the handful of host-
  specific functions as UML-safe no-ops. This is the same
  approach C-05 ftrace took for `<asm/patchable.h>`. **Medium
  risk** — identified as the dominant unknown; not a blocker,
  just work.

**U4: CONFIG_BPF_JIT_ALWAYS_ON vs. fuzz's expectations.**

- Question: Does fuzz want JIT unconditionally, or configurable
  via `/proc/sys/net/core/bpf_jit_enable`? The default is
  "enabled if CONFIG_BPF_JIT_ALWAYS_ON=y, else opt-in via
  sysctl". Research profile should probably default ALWAYS_ON;
  fuzz likewise. Verify in commit 3 when touching
  research.config.
- This is a policy call, not a technical one. Decision: mirror
  x86_64 defconfig which sets `CONFIG_BPF_JIT_ALWAYS_ON=y`. Add
  to research.config + fuzz.config if missing. **No risk** —
  just an explicit knob.

## Risk summary

- **v1 implementation risk: low.** The JIT itself is a known-
  good x86_64 C translation unit that writes byte arrays. The
  allocator fallback is already correct for UML by construction.
  U3 is the only concrete unknown and has a fallback plan.
- **Security risk: accepted.** UML v1 keeps BPF JIT pages RWX
  (same as kprobes, modules). Not a production kernel; research
  profile. Documented in Limitations of the user doc.
- **Maintenance risk: low.** No new UML-specific JIT code; the
  port rides the x86 JIT's maintenance.

## Reality-check

The original doc budgeted 3 weeks; investigation shows the
minimal-viable shape is probably under a week. Keep the 3-week
line-item in `00-vision.md` and `06-sequencing/critical-path.md`
as a soft ceiling, not a target. If the implementation lands in
under a week we win back schedule for C-07 (KMSAN) or C-10
(crosvm launcher). If U3 blows up and we need real glue, the
3-week budget covers it.

Success shape: `make ARCH=um uml/research && make ARCH=um -j$(nproc)`
→ `bpftool prog loadall /path/to/prog.bpf` → `bpftool prog show id N`
prints `jited: yes`. Selftest in `tools/testing/selftests/um/bpf-
smoke/` codifies that assertion.

## Blocker: what the empirical build found

The commit-1 attempt (2026-04-20; reverted, nothing pushed) set
`select HAVE_EBPF_JIT if X86_64` + `core-$(CONFIG_BPF_JIT) +=
$(HOST_DIR)/net/` and built the research profile with
`CONFIG_BPF_SYSCALL=y CONFIG_BPF_JIT=y`. The compile of
`arch/x86/net/bpf_jit_comp.c` surfaced three real divergences:

1. **`regs->ip` direct-field access** at
   `bpf_jit_comp.c:1493,1499` (function `ex_handler_bpf`). x86's
   bare-metal `struct pt_regs` has `ip` as a named field
   (`arch/x86/include/asm/ptrace.h`); UML's `struct pt_regs`
   wraps `struct uml_pt_regs` and exposes IP via the
   `PT_REGS_IP(regs)` / `instruction_pointer(regs)` macros.
   **Pt_regs layout divergence, not a header fix.**

2. **`boot_cpu_has(X86_FEATURE_BMI2)` implicit declaration** at
   `bpf_jit_comp.c:1997`. UML has a working `boot_cpu_has` macro
   at `arch/um/include/asm/cpufeature.h:54`; `bpf_jit_comp.c`
   doesn't explicitly `#include <asm/cpufeature.h>` and relies
   on transitive inclusion that holds on bare-metal x86 but not
   under UML. **Fix: add the explicit include.**

3. **`VSYSCALL_ADDR` undeclared** at `bpf_jit_comp.c:2255`. UML
   has no vsyscall page. **Fix: `#define VSYSCALL_ADDR 0UL` in a
   UML-only header.**

(2) and (3) fit the U3 mitigation plan (UML-only header
forwarders). (1) does not: `#define ip ...` at translation-unit
scope at the `#include <asm/x86_bpf_jit_comp.c>` site collides
with unrelated `.ip` field accesses in other kernel headers
pulled in transitively.

The full three-way A/B/C decision (UML-side shim, small upstream
patch, defer) is in `04-risks/decisions-log.md` D43. Per
AGENT-PROMPT §"When to stop and ask", option B (touches
`arch/x86/net/`) requires user sign-off; option A (all
`arch/um/`) does not but is fragile; option C defers C-06.

Until the decision arrives, C-06 remains `blocked`. Other
workstream leaves (C-07 KMSAN, C-09 v2 groundwork) are not
blocked and continue to advance.
