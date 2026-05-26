# Layer-1 probe refactor — move os_early_checks() probing into backend lifecycle ops

**Status:** DESIGN (2026-04-24). Code deferred to a dedicated
session; this memo captures the exact plan so the work lands
with minimum discovery.

**Parent audit:** 2026-04-24 audit finding A3 (medium-high) —
the Layer-1 arbiter in `arch/um/kernel/backend.c::init_backend`
presents itself as authoritative, but
`os_early_checks()` in `arch/um/os-Linux/start_up.c:557` still
does seccomp probing, ptrace fallback, and ptrace-only fatal
checks BEFORE init_backend runs. The lifecycle hooks in
`arch/um/backend/seccomp/lifecycle.c:21` and
`arch/um/backend/ptrace/lifecycle.c:64` are stubs that
explicitly document the deferral.

**Round-4 audit re-confirmation (F1).** The 2026-04-24 round-4
review re-flagged this as still-outstanding, with the same
file:line pointers: `os_early_checks` in
`arch/um/os-Linux/start_up.c:557` + `:605`, stub lifecycle
ops at `arch/um/backend/ptrace/lifecycle.c:32` and
`arch/um/backend/seccomp/lifecycle.c:23`, and
`backend.c:135`'s arbiter. F1 asks for exactly the same
consolidation this memo scopes — no additional scope beyond
the three-change plan below. Task #219 (F1) is therefore a
renaming of task #215 (A3) from the round-2 audit; both are
satisfied by the same single-commit landing.

**Correctness-vs-architectural-purity:** A3 is architectural,
not correctness-breaking. `backend=force=kvm` boots correctly
today because the wasted seccomp probe in `os_early_checks`
just sets `using_seccomp = 1` as a side effect; `init_backend`
sees `UM_BACKEND_KIND_KVM` in `backend_arg_requested` and
picks the KVM ops table regardless. The refactor is worth
landing because it unwinds the "global `using_seccomp` side
channel" — making backend selection fully data-flow through
the arbiter — but it doesn't fix any user-visible bug.

## What the current code does

```c
// arch/um/kernel/um_arch.c
linux_main(...) {
    ...
    os_early_checks();          // (1) runs probes, sets using_seccomp
    init_backend(&backend_args); // (2) consumes using_seccomp via
                                 //     pick_dynamic_backend()
    ...
}

// arch/um/os-Linux/start_up.c
void __init os_early_checks(void) {
    check_coredump_limit();     // host-only, no backend coupling
    check_tmpexec();            // host-only, no backend coupling
    if (!SECCOMP && !PTRACE) return;    // KVM_ONLY short-circuit
    if (SECCOMP && arg != PTRACE) {
        if (init_seccomp()) {   // REAL seccomp probe
            using_seccomp = 1;
            return;
        }
        if (seccomp_config == 2) fatal(...);
    }
    if (!PTRACE) fatal(...);
    if (uml_ncpus > 1) fatal(...);
    using_seccomp = 0;
    check_ptrace();             // REAL ptrace probe
}

// arch/um/kernel/backend.c
static int __init init_backend(...) {
    if (arg == force=kvm)      pick_kvm_ops();
    else if (arg == force=seccomp) pick_seccomp_ops();
    else if (arg == force=ptrace)  pick_ptrace_ops();
    else                           pick_dynamic_backend();  // reads using_seccomp
}
```

`using_seccomp` is the global side channel; `init_seccomp` +
`check_ptrace` are large statics in start_up.c doing the real
probes.

## What the refactor looks like

Three changes land together:

### 1. Extract the host-only checks into their own helper

`os_early_host_checks()` keeps `check_coredump_limit()` and
`check_tmpexec()`. No backend coupling. Called from
`um_arch.c::linux_main` BEFORE `init_backend`, preserving the
invariant that these run before any mmap-using code.

`os_early_checks()` is removed.

### 2. Move the probes into each backend's lifecycle.probe()

`arch/um/backend/seccomp/lifecycle.c::seccomp_probe`:

```c
int seccomp_probe(void)
{
    /*
     * Lifted from arch/um/os-Linux/start_up.c::init_seccomp.
     * Spawns a stub child with CLONE_VFORK|VM, installs the
     * seccomp filter, reads back exec_regs + host_fp_size
     * via get_stub_state. Sets using_seccomp = 1 on success.
     * Returns 0 on probe-passed, -errno on probe-failed
     * (the arbiter then picks ptrace per pick_dynamic_backend
     * when using_seccomp==0).
     */
    if (!init_seccomp())
        return -ENODEV;
    return 0;
}
```

`arch/um/backend/ptrace/lifecycle.c::ptrace_probe`:

```c
int ptrace_probe(void)
{
    /*
     * Lifted from arch/um/os-Linux/start_up.c::check_ptrace.
     * Spawns a stub child, verifies PTRACE_SYSEMU is
     * available. Fatal on failure since ptrace is required
     * for the DYNAMIC fallback path.
     */
    check_ptrace();
    if (uml_ncpus > 1)
        fatal("SMP is not supported with PTRACE userspace.\n");
    return 0;
}
```

Both `init_seccomp` and `check_ptrace` are exported from
`os-Linux/start_up.c` (made non-static) so the backend
lifecycle.c TUs can call them. Keep them in start_up.c for
now — their bodies are substantial and touch
`seccomp_test_stub_data`, `exec_regs`, etc., which are also
static in start_up.c. Moving the full bodies would cascade
into moving `fatal_perror`, `get_stub_state`, and a handful
of other helpers. Worth doing in a subsequent cleanup; the
renaming + export is the minimum.

### 3. Arbiter drives the probes

`arch/um/kernel/backend.c::init_backend` calls probe() on
each compiled-in backend (matching the spec in
`Documentation/virt/uml/backend-contract.rst` §"Lifecycle
ordering"):

```c
static int __init init_backend(...) {
#ifdef CONFIG_UM_BACKEND_KVM
    (void)kvm_probe();  // already populated via
                        // os_early_checks path pre-refactor
#endif
#ifdef CONFIG_UM_BACKEND_SECCOMP
    if (arg != UM_BACKEND_KIND_PTRACE)
        seccomp_probe();
#endif
#ifdef CONFIG_UM_BACKEND_PTRACE
    if (!using_seccomp || arg == UM_BACKEND_KIND_PTRACE)
        ptrace_probe();
#endif
    // ... existing arg-dispatch logic unchanged ...
}
```

## What stays the same

- The `using_seccomp` global (for now; the Grand Removal is a
  separate follow-on).
- `pick_dynamic_backend()` in `backend.c`.
- Every `run_userspace` / mm op — unchanged.
- `um_backend` table layout — unchanged.

## Blockers

1. **Export ordering.** `init_seccomp` + `check_ptrace` in
   start_up.c reference `exec_regs`, `host_fp_size`,
   `seccomp_test_stub_data`. Those are static globals. Exporting
   the probe functions for cross-TU call is fine; moving the
   whole bodies would cascade.

2. **`os_early_host_checks` vs probe ordering.** check_tmpexec
   mmaps memory early, before mm_init. If the probe happens
   after (as "via init_backend"), the host memory assumptions
   need to stay satisfied. `os_early_host_checks` preserves
   that by keeping check_tmpexec + check_coredump_limit at the
   same call site.

3. **KVM_ONLY short-circuit.** Current
   `if (!SECCOMP && !PTRACE) return;` goes away naturally
   because the `#ifdef`s in init_backend gate the probe calls.

## Measurement

No performance target — this is a refactor. Regression matrix:

- 32/32 KUnit backend-contract tests must still pass.
- userspace-smoke + launcher-smoke + umlctl-smoke + ftrace-smoke
  + kprobes-stress: must still PASS under each single-backend
  build (PTRACE_ONLY, SECCOMP_ONLY, KVM_ONLY) + DYNAMIC default.
- Boot-time delta: probe runs at the same wall-clock moment,
  just from a different TU.

## Why this is deferred

Three reasons:

1. The existing `os_early_checks` code is extensively
   documented (see `start_up.c:557-609`) as "future cleanup;
   early-boot sequence in arch/um/os-Linux/ needs
   reorganization before the arbiter can safely drive probe()
   itself." That documentation is accurate; the refactor needs
   a dedicated session to do cleanly.

2. No user-visible bug hangs on it. The audit finding is
   architectural purity, not correctness.

3. Scope: touches 4 files across 3 subsystems with careful
   ordering. Better done as one focused commit than
   piecemealed alongside gadget work.

## Decisions-log cross-reference

- 2026-04-24 2nd audit round: identified A3 (this memo).
- D72 (this landing): records the design commitment; tracks
  implementation as a separate follow-on task.

## Status flip criteria

This memo becomes implementation when:

1. A session starts with "A3 Layer-1 probe refactor" as the
   sole scope.
2. The four files above are touched in one commit (or one
   stacked series).
3. The regression matrix above goes green for all four
   build flavors.
4. D72 gets an "implemented" follow-up entry pointing at the
   landing commit SHA.
