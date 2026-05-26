# UML vector driver v2 — cmdline migration policy

**Status:** decision recorded.  Migration is a one-time cmdline edit.
**Date:** 2026-05-17.
**Audit ref:** `46-uml-vector-driver-v2-code-audit-2026-05-17.md` §P4.1.

## Question

Should v2 accept legacy `vecN:...` / `vec=N,...` cmdline syntax
in addition to its own `vec2.N:...` / `vec2=N,...` syntax, so an
operator can swap v1 for v2 without editing kernel cmdlines?

## Decision

**No automatic aliasing.  Migration is a one-time cmdline edit:
`vec0:foo=bar` -> `vec2.0:foo=bar`.**

## Rationale

  1. **`__setup` prefix routing is order-dependent.**  Legacy
     `vector_kern.c` registers `__setup("vec", vector_setup)`, which
     matches every cmdline arg starting with `vec` — including
     `vec0:`, `vec.0:`, `vec=`, etc.  If v2 *also* registers
     `__setup("vec.", ...)`, both handlers can match `vec.0:...`.
     The kernel chooses based on `.init.data` section link order,
     which is fragile and easy to break in future refactors.

  2. **Combined-build ambiguity.**  When `CONFIG_UML_NET_VECTOR=y`
     AND `CONFIG_UML_NET_VECTOR_V2=y` (the realistic transition
     config — keep legacy running while v2 stabilises), an alias
     would have to pick a winner per-arg.  Either:
     - v1 always wins (alias is useless for migration);
     - v2 always wins (silently steals from a still-deployed
       legacy that may be the production path);
     - per-arg config knob (more cmdline complexity).
     None of those are operator-friendly.

  3. **The migration edit is small and explicit.**  Find/replace
     `vec0:` → `vec2.0:` (and analogous N) in the kernel cmdline.
     Operators can git-grep deployments for the change at a
     well-defined moment.  Silent aliasing hides the moment of
     migration and complicates rollback.

  4. **Long-term direction.**  Once `CONFIG_UML_NET_VECTOR_V2`
     reaches drop-in-replacement quality (post-audit P3 / P4
     gates), the eventual deprecation path is:
     - default `CONFIG_UML_NET_VECTOR=n` in distros;
     - mark legacy as deprecated in arch/um/drivers/Kconfig;
     - eventually remove legacy after a release window.
     At THAT point — when legacy is gone — v2 can take the `vec`
     prefix if reviewers want it.  Until then, distinct prefixes
     keep the two drivers cleanly distinguishable in dmesg, log
     parsers, and cmdline introspection.

## Operator-facing migration

Find / replace in the kernel cmdline:

```
Before:   vec0:transport=tap,ifname=tap0,mode=auto
After:    vec2.0:transport=tap,ifname=tap0,mode=auto

Before:   vec=0,transport=fd,fd=200
After:    vec2=0,transport=fd,fd=200
```

Per-key syntax (`transport=`, `ifname=`, `mode=`, etc.) is
unchanged between v1 and v2 for the supported transports (tap +
fd).  For the seven transports v2 has not implemented yet
(raw, gre, l2tpv3, hybrid, bess, vde, proxy — see audit P4.2),
keep using v1 until v2 grows the runtime backend.

## Future revisitation

If a distro reports the manual edit is a real adoption friction
point, consider:

  - a userspace shim that translates `vec` → `vec2` in the
    bootloader-staged cmdline before exec;
  - a Kconfig-gated `__setup("vec.", ...)` alias that only
    registers when `!CONFIG_UML_NET_VECTOR` (legacy not in tree),
    eliminating the order-dependency concern;
  - leaving v1 in tree forever (drop-in v2 not required).

This memo closes audit P4.1.  Reopen if any of the above three
paths becomes the better choice with new data.
