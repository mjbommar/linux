# A-04: Kconfig — single vs multi-backend builds

**Status:** **complete (2026-04-18)** — Kconfig invariants enforced
(D13), boot-param parser (`backend=auto|ptrace|seccomp|force=…`)
landed, init_backend() honors them with `force=` panic semantics,
build matrix exercises 12 boot variants (3 dispatch modes × 4
boot-param combinations).
**Effort:** 2 weeks (most of the work folded into REVIEW.* fixes
and HOT-1.3; this slice only added the boot-param parser)
**Dependencies:** A-02, A-03 (need impls to select between)
**Blocks:** C-01 (defconfig design)

## Status detail

| Aspect | Status |
|---|---|
| Kconfig choice block | landed (D13); _ONLY=exactly one backend, DYNAMIC=both |
| Single-source-of-truth selection | landed (D12); init_backend in linux_main, validates HOT ops, sets using_seccomp |
| `backend=` boot param parser | landed in `arch/um/os-Linux/start_up.c::uml_backend_config` via `__uml_setup` |
| `force=` panic semantics | landed in `arch/um/kernel/backend.c::pick_dynamic_backend` + `*_ONLY` mismatch check |
| Boot matrix coverage | 12 rows: 3 dispatch modes × {default, named, force=, force=opposite} |
| Boot-param documentation | `Documentation/virt/uml/backend-contract.rst` selection section |
| `seccomp=on/auto/off` legacy | preserved one release; aliases documented; consumed by init_seccomp probe which feeds into `using_seccomp` (read by init_backend in `auto` mode) |
| objdump verification of *_ONLY direct calls | done in HOT-1.3 (read_persistent_clock_ns), HOT-1.4 (run_userspace via register-hoist) |

## Goal

Express "compile in N backends, select at runtime" vs "compile
in 1 backend, inline its calls" as Kconfig choices. Single-backend
inline gives sandbox profile its small TCB and zero
indirect-call overhead.

## Approach

```
config UM_BACKEND_PTRACE
    bool "Include ptrace backend"
    default y if !UM_SECCOMP
    help
      Traditional skas mode. Most-portable backend; works on any
      Linux host with ptrace.

config UM_BACKEND_SECCOMP
    bool "Include seccomp backend"
    default y
    depends on SECCOMP_FILTER
    help
      Faster than ptrace (~2-3×). Requires host SECCOMP_FILTER.

config UM_BACKEND_KVM
    bool "Include KVM backend"
    default n
    depends on KVM
    help
      Fastest backend (~10× faster than ptrace). Requires host
      KVM access.

choice
    prompt "Backend dispatch mode"
    default UM_BACKEND_DYNAMIC

    config UM_BACKEND_DYNAMIC
        bool "Multi-backend dynamic dispatch"
        depends on (UM_BACKEND_PTRACE && UM_BACKEND_SECCOMP) || \
                   (UM_BACKEND_PTRACE && UM_BACKEND_KVM) || \
                   (UM_BACKEND_SECCOMP && UM_BACKEND_KVM)
        help
          Compile in multiple backends; select at boot via backend=.
          Adds one indirect call per dispatch (~5-10 cycles).

    config UM_BACKEND_PTRACE_ONLY
        bool "Inline ptrace backend (no other backends)"
        depends on UM_BACKEND_PTRACE && \
                   !UM_BACKEND_SECCOMP && !UM_BACKEND_KVM

    config UM_BACKEND_SECCOMP_ONLY
        bool "Inline seccomp backend (no other backends)"
        depends on UM_BACKEND_SECCOMP && \
                   !UM_BACKEND_PTRACE && !UM_BACKEND_KVM

    config UM_BACKEND_KVM_ONLY
        bool "Inline KVM backend (no other backends)"
        depends on UM_BACKEND_KVM && \
                   !UM_BACKEND_PTRACE && !UM_BACKEND_SECCOMP
endchoice
```

## Deliverable

- `arch/um/Kconfig.backends` with the above
- Build system support for both modes
- `arch/um/include/asm/backend.h` macros that expand differently
  per dispatch mode (see `01-architecture/three-layers.md`)

## Validation

- All four Kconfig combinations build cleanly
- Boot test passes for each
- For single-backend builds: confirm via `objdump -d` that the
  hot path has no indirect call to backend ops
- For multi-backend: confirm boot param `backend=` selects the
  named backend

## Open questions

- **Q1**: Should we ship a "no backends compiled in" config for
  library-only builds? (Plan: yes; library mode doesn't need a
  backend at all. Add `UM_BACKEND_NONE` for library profile.)
- **Q2**: How do we handle `make olddefconfig` with breaking
  Kconfig changes? (Plan: ship a transition note in each release.)

## Risk

Low. Kconfig is well-understood; the patterns above are standard.
