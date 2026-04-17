# Genuine conflicts and their resolutions

Three things in the design genuinely don't compose. We name them
explicitly and document the resolution so a future engineer
doesn't try to "fix" them.

## C1: RWX text vs RO text

**Conflict:** ftrace and kprobes patch executable code at runtime
to insert hooks. KVM-mode backend wants `.text` mapped read-only
inside the guest for security and KVM-internal optimizations
(makes the EPT/NPT pages cacheable read-only, less invalidation).

These are mutually exclusive on the same memory region.

**Resolution:** Split `.text` into two sections.

```
.text.frozen        RO after init        most of the kernel
.text.patchable     RW (or W^X toggled)  mcount stubs, static_key call sites
```

Linker script (`arch/um/kernel/vmlinux.lds.S`) places:

- All static-key call sites (5 bytes each, JITted in place) in
  `.text.patchable`.
- All mcount stubs (5 bytes each, function entry trampolines)
  in `.text.patchable`.
- Everything else in `.text.frozen`.

Result: KVM backend can mark `.text.frozen` RO; `.text.patchable`
stays RW (or W^X-toggled with explicit gates around patches).

**Cost:** Slightly larger binary (separate sections waste a few
pages per section), tiny TLB cost (two text sections vs one).
Acceptable.

**Reference:** The kernel already has the concept
(`__ro_after_init`, `__init` sections). x86_64 has `__ro_after_init`
sections that get mprotected RO after initcalls. We extend the
same idea.

**Profiles affected:** `prod-fast`, `prod-with-hooks`, `sandbox`
(all use KVM-style RO benefit). `research`, `fuzz`, `fuzz-deep`
(all use ftrace/kprobes patching).

## C2: SMP parallelism vs time-travel determinism

**Conflict:** True parallel kernel execution (SMP) and a
deterministic logical clock (time-travel) are mutually
exclusive. Parallel execution introduces nondeterministic
ordering of events; time-travel demands a total order.

**Resolution:** Time-travel mode forces UP at boot.

```c
/* arch/um/kernel/time-travel.c */
void __init time_travel_init(void)
{
    if (!time_travel_enabled) return;

    if (num_possible_cpus() > 1) {
        pr_warn("time-travel mode forces UP; disabling SMP\n");
        set_cpu_possible(1, false);
        /* ... mark only CPU 0 possible/online */
    }
}
```

This is enforced at runtime (via the static key on `read_clock`
plus a boot-time CPU-possible mask), not compile time. So a
single binary supports:

- SMP without time-travel (default research mode)
- UP with time-travel (time-travel-research profile)

You choose at boot.

**Open question:** Some research workloads want both (deterministic
parallel kernel execution). The literature has logical-clock
SMP schedulers (vector-clock-based event ordering). Out of
scope for v1; revisit if anyone asks.

**Profiles affected:** `time-travel` (UP-only). All others can
use SMP.

## C3: Library mode vs ring-split realism

**Conflict:** Library mode (LKL-style) calls `lkl_sys_open()`
directly from the host application. There is no `copy_from_user`
boundary, no signal delivery, no real syscall entry path. Bugs
that depend on the user/kernel boundary — e.g., TOCTOU on
`copy_from_user`, signal-handling races, unaligned-access
faults from userspace — are unreachable.

Ring-split mode preserves all of these. Library mode trades them
for ~60× speed.

**Resolution:** Library mode is a *sibling artifact*, not a
runtime choice.

- `make ARCH=um uml/library` produces `liblinux.a` and `liblinux.so`.
  Entry point: `lkl_start_kernel()` + `lkl_sys_*()`.
- `make ARCH=um uml/research` (or any other profile) produces the
  `linux` ELF binary. Entry point: `_start` → `main()` → boot.
- Both share kernel sources (~95% of `arch/um/`). The diff is the
  entry path: `arch/um/library_main.c` vs `arch/um/as_user.c`,
  plus a thin `arch/um/library/` for the `lkl_sys_*` shim layer.

Consumers know which they're calling. A fuzz harness using library
mode for speed knows it's not testing user/kernel boundary code;
a syzkaller setup using ring-split mode knows it's getting
realistic syscall paths.

**No runtime selection.** You can't flip from library to ring-split
mid-run; they're different binaries.

**Cost:** Maintaining two entry points. Bounded; small.

**Reference:** This resolution is the unify-LKL-into-UML idea
that Tazaki proposed (RFC v8, 2019). It died politically because
there was no unifying architecture story. With Layer 1 + Layer 2 +
Layer 3 in place, the unification has a coherent home: library
mode is just "skip Layer 1, call directly into the kernel".

**Profiles affected:** `library` (library mode). All others
(ring-split mode).

## C4 (sub-conflict, narrower): mconsole vs sandbox

**Conflict:** `CONFIG_MCONSOLE` exposes a Unix-socket control
interface that can add/remove devices and inspect guest state.
Powerful for research; a host-escape primitive for sandbox.

**Resolution:** Compile-time choice. `sandbox` profile sets
`# CONFIG_MCONSOLE is not set`. Other profiles default to `=y`.

This is not a hard architectural conflict, just a default that
must differ. Documented here for visibility.

**Profiles affected:** `sandbox` (off). All others (on).

## C5 (sub-conflict, narrower): KCOV buffer vs sandbox

**Conflict:** KCOV records guest PCs into a userspace-mapped
buffer. The buffer is mmap'd into the guest userspace process
that owns the coverage. In a sandbox profile, this is a
side-channel into kernel addresses (every PC is a kernel
text address).

**Resolution:** `sandbox` profile sets `# CONFIG_KCOV is not set`.
Coverage is research/fuzz-only. Documented.

**Profiles affected:** `sandbox` (off). `fuzz`, `fuzz-deep` (on).

## What is NOT a conflict (despite first appearances)

- **KASAN + KVM.** They compose. KASAN is in-guest compile-time
  wrap; KVM is the trap mechanism. KASAN-instrumented UML on
  KVM is much faster than KASAN-instrumented UML on ptrace
  because the trap is faster, but the wrap behavior is identical.

- **Tracing + KVM.** They compose. Tracing fires inside the guest
  kernel; KVM provides the trap. Tracing slow path costs the
  same regardless of how the trap arrived.

- **Snapshot + KASAN.** They compose. Snapshot serializes the
  shadow region the same as any other memory region.

- **Library mode + sanitizers.** They compose. KASAN-instrumented
  `liblinux.a` is fine; the host caller pays for the wrap on
  every call into the library.

- **eBPF JIT + KVM.** They compose. The JIT generates code into
  `.text.patchable`; KVM mode RWX management for that section
  handles it.

- **SMP + sanitizers.** They compose, but SMP makes KCSAN much
  more useful (real races now possible). KASAN/KMSAN/KFENCE
  are SMP-orthogonal.

The composition matrix is sparse: 3 conflicts (+ 2 sub-conflicts)
out of dozens of feature pairs. The architecture works.
