# UML — clang / LTO / PGO build feasibility (measured)

**Date:** 2026-06-19
**Toolchain:** clang 21.1.8, ld.lld, llvm-profdata present. Configs from
uml-clean. All measurements 1-CPU pinned.

Question: can we build an LTO/PGO UML with clang, and does it help? Short answer:
**clang builds fine and is perf-equivalent; ThinLTO is advertised but broken;
codegen optimization does not move UML perf** (the cost is the host trap/handoff,
not guest-kernel instruction quality).

## Results

| build | status | getpid floor | boot (init=/bin/true) | cpython | binary |
|---|---|---|---|---|---|
| gcc -O2 (baseline) | ok | ~32,150 cyc | 2.49 s | 32.9 s | 91 MB |
| **clang -O2** (`LLVM=1`) | **ok** | ~31,910 cyc | 2.49 s | 32.5 s | **77.5 MB** |
| clang ThinLTO | **build FAILS** | — | — | — | — |

clang -O2 ≈ gcc -O2 on every metric (within noise); 15% smaller binary (code
size, not speed). cpython SUCCESS on both.

## ThinLTO is advertised but broken on UML

`arch/um/Kconfig` selects `ARCH_SUPPORTS_LTO_CLANG` and `_THIN`, but the build
fails at the kallsyms relink:

```
LD .tmp_vmlinux1
.vmlinux.export.o: file not recognized: file format not recognized
clang: error: linker command failed
```

Under LTO the objects are LLVM bitcode, but UML links vmlinux as a **hosted ELF
executable** (against host libc) via a path whose intermediate kallsyms relink
(`scripts/link-vmlinux.sh`'s `.tmp_vmlinux1`) invokes a linker that can't consume
bitcode `.vmlinux.export.o`. So UML's two-stage hosted link doesn't propagate
LTO/bitcode handling to the kallsyms stage. There were also target-triple
mismatch warnings (`x86_64-pc-linux-gnu` for arch/x86/um objects vs
`x86_64-unknown-linux-gnu` for generic). **This is a real UML+LTO integration gap
worth an upstream report** (UML claims the Kconfig support but it doesn't link).

## PGO / AutoFDO / Propeller

`AUTOFDO_CLANG` and `PROPELLER_CLANG` exist in `arch/Kconfig` but
`ARCH_SUPPORTS_AUTOFDO_CLANG` / `_PROPELLER_CLANG` are selected only by **x86**,
not `um` — so they're not offered for `ARCH=um` without adding the select +
testing. Raw userspace PGO (`-fprofile-generate`/`-use`, treating the kernel as
a normal ELF — UML's unique trick) is possible but needs the **stub excluded**
from instrumentation (it runs seccomp'd; profile-runtime syscalls there would be
killed). The exclusion infra already exists (the stub is `KCOV_INSTRUMENT := n`
/ `KASAN_SANITIZE := n`).

## Why none of this is worth chasing for perf

Codegen quality is a small slice of UML's cost. getpid floor and boot are
**identical** across gcc/clang, and cpython is within noise — because the
bottleneck is the host-side **trap/handoff** (futex + scheduler + signal frame),
which no amount of guest-kernel codegen optimization touches. This is the same
lesson as O1: the structural boundary-crossing dominates; instruction-level wins
on the guest kernel don't show at the workload level.

**Disposition:** clang -O2 is a fine, perf-equivalent, smaller alternative build
(could default to it). LTO needs a UML-hosted-link fix before it even builds, and
even then would only help boot/codegen — which we measured as insensitive. PGO is
not wired for um and has low expected value for the same reason. The performance
levers remain where the cost is: the gadget (remove clock crossings) and the
handoff (O4, out-of-tree).
