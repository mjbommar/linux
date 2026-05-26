# Architecture

The plan rests on three layered abstractions. They are independent
in the sense that you can change one without rewriting the others;
they are layered in the sense that lower layers don't know about
higher layers.

## The layers

```
┌──────────────────────────────────────────────────────────────┐
│  Layer 3: Compile-time wraps (KASAN, KMSAN, KCSAN, KFENCE)   │
│  ─────────────────────────────────────────────────────────── │
│  Wraps every memory access at compile time. Per-build cost.  │
│  Composes with any backend, any static-key state.            │
└──────────────────────────────────────────────────────────────┘
                              ▲
                              │ instruments
                              │
┌──────────────────────────────────────────────────────────────┐
│  Layer 2: Static-key gates on hot paths                      │
│  ─────────────────────────────────────────────────────────── │
│  syscall entry, page fault, context switch, IPI, time read   │
│  Each gate is JIT-patched NOP when off, JMP when on.         │
│  Toggle at runtime via debugfs. Cost off: ~1 ns. Cost on:    │
│  varies by hook (~50 ns trace, ~10 ns kcov, ~200 ns replay). │
└──────────────────────────────────────────────────────────────┘
                              ▲
                              │ calls
                              │
┌──────────────────────────────────────────────────────────────┐
│  Layer 1: Backend ops table                                  │
│  ─────────────────────────────────────────────────────────── │
│  struct um_backend_ops { run_userspace, mm_{map,unmap},      │
│  context_switch, ipi_send, read_clock_ns, ... } — 18 ops    │
│  Implementations: ptrace, seccomp (both in tree); kvm        │
│  (workstream D). Selected at compile time (single-backend,   │
│  inlined) or runtime (multi-backend, indirect call).         │
└──────────────────────────────────────────────────────────────┘
                              ▲
                              │ traps to
                              │
┌──────────────────────────────────────────────────────────────┐
│  Host kernel + hardware                                      │
│  Not ours. We're the guest.                                  │
└──────────────────────────────────────────────────────────────┘
```

## Documents in this directory

- **[three-layers.md](three-layers.md)** — the load-bearing design;
  read this first
- **[invariants.md](invariants.md)** — properties that must hold
  across every profile, regardless of which features are on
- **[conflicts.md](conflicts.md)** — the genuine "you can't have both"
  cases and how we resolve them (RWX vs RO text, determinism vs
  parallelism, library vs ring-split)
- **[data-flow.md](data-flow.md)** — walk a syscall through every
  layer, every backend; concrete sequence diagrams

## Why this layering, not another

We considered three alternative architectures:

1. **One backend (just KVM), one set of features (everything
   compiled in, runtime-toggled).** Rejected because nested-virt
   environments (CI, cloud, containers) need a non-KVM fallback,
   and forcing every feature compiled in bloats sandbox-profile
   binaries with code that's a security liability when present.

2. **One source tree, separate forks per profile.** Rejected
   because it just relocates the problem to "now you have N forks
   diverging". The host kernel doesn't fork per use case; it ships
   one tree with extensive Kconfig + static_key + compile-time
   wraps. We do the same.

3. **Plugin architecture (loadable backend modules).** Rejected
   because the backend abstraction is a hot path (every syscall),
   and module-load overhead + indirect call layer is more
   expensive than necessary. Compile-time backend selection +
   static_branch_likely on the indirect call gives us the same
   flexibility cheaper.

The chosen layering is identical in shape to the host kernel's
own approach to the same problem space. That's intentional —
the host kernel works at scale, has been refined for 30+ years,
and its abstractions compose. Copying its pattern reduces our
risk dramatically.

## The compositional matrix

Every shipped binary is a point in (Layer 1) × (Layer 2 defaults) ×
(Layer 3 enabled set). The eight profiles in `03-profiles/` are the
points we choose to ship. Other points are buildable but unsupported.

| Profile | L1 backend | L2 default-on hooks | L3 wraps |
|---|---|---|---|
| prod-fast | KVM (or seccomp) | none | none |
| prod-with-hooks | KVM | none | none |
| research | seccomp | trace, kprobes | KASAN, UBSAN |
| fuzz | seccomp | KCOV, snapshot | KASAN |
| fuzz-deep | seccomp | KCOV, record-replay | KASAN, KCSAN |
| sandbox | seccomp-only | none compiled in | none |
| library | none (direct call) | none | KASAN optional |
| embedded | ptrace | none | none |
| time-travel | seccomp | trace, time-travel | KASAN |

The matrix tells you which combinations are validated (all rows
shipped) and which are theoretically buildable (any combination,
but not in CI).

## What this architecture is NOT

- It is not a microkernel design. There are no IPC boundaries
  between subsystems. UML kernel code looks like Linux kernel code.
- It is not a hypervisor. KVM (Linux's hypervisor) is what the
  KVM backend uses, not what we are.
- It is not a sandbox model in itself. The sandbox profile is
  one configuration of this architecture; it is not the whole
  architecture.
- It is not a verification effort. We are not formally verifying
  the backend abstraction. seL4 exists if you want that.
