# Invariants

Properties that hold across every profile, every backend, every
combination of static keys. If a change to the architecture would
break an invariant, the invariant wins or the change is rejected.

## I1: Same source tree, every profile

There is exactly one `arch/um/` source tree. No profile-specific
forks, no shadow trees, no preprocessor walls that mean "this code
only exists in profile X". Everything that varies by profile varies
through Kconfig, static keys, or backend ops.

**Why:** Forks diverge. Preprocessor walls accumulate. The host
kernel doesn't fork per use case; we don't either.

**Enforcement:** CI builds every defconfig from the same checkout.
Patches that touch `arch/um/` must build all defconfigs cleanly.

## I2: prod-fast performance never regresses

The `prod-fast` profile is the speed of light. No commit may make
prod-fast slower. Period.

**Why:** Every other property of this architecture is a tax. If we
let prod-fast drift, we lose the credibility to charge taxes
elsewhere. Performance regressions are easier to prevent than to
recover from.

**Enforcement:** CI runs a syscall-cycle benchmark on prod-fast
for every patch. Regression >5% blocks merge. Regression 2-5%
requires explicit reviewer ack.

## I3: Layer 2 gates cost ~1 ns when off

Every static-key gate must be JITted to a NOP when the key is in
its off state. No gate may have a "soft" off mode where it still
does any work.

**Why:** The whole point of Layer 2 is that observability is free
when not used. Soft-off would defeat the architecture.

**Enforcement:** Every new gate must include a microbenchmark
showing off-state cost <2 ns. Test runs in CI on prod-fast build.

## I4: Backends are interchangeable for kernel correctness

Any kernel test that passes on one backend must pass on all backends
(modulo backend-specific features the test exercises). A bug in
the kernel surfaces the same way regardless of trap mechanism.

**Why:** If backends produce different kernel behavior, then UML
isn't a faithful Linux port — it's three separate things wearing
a UML mask. We are studying Linux, not "Linux plus seccomp quirks".

**Enforcement:** LTP and kselftest run against every backend in
CI. A test passing on ptrace but failing on seccomp is a backend
bug, not a test bug.

## I5: Compile-time sanitizers compose with every backend

KASAN, KMSAN, KCSAN, KFENCE all work regardless of which backend
is active. Sanitizer behavior is identical across backends.

**Why:** Sanitizers are about *kernel* memory safety, not the
trap mechanism. If KASAN reports a bug under one backend and not
another, the difference is a UML bug, not a kernel bug.

**Enforcement:** sanitizer-positive crash reproducers (from
syzbot or our own corpus) reproduce identically across backends.

## I6: Profiles differ only in defaults, not in available code

A binary built for `sandbox` profile contains less code than
`research` profile (sanitizers compiled out, mconsole compiled
out). But within a given binary, the source code that *is*
present behaves identically — the binary differences are
subtraction, not divergence.

**Why:** "Sandbox UML behaves differently from research UML"
is the same trap as "WSL1 syscalls behaved differently from
Linux syscalls". We avoid it.

**Enforcement:** Where two profiles include the same Kconfig
symbol, the symbol's behavior is identical. Profiles set
defaults, not behaviors.

## I7: Library mode shares the kernel implementation

The library-mode artifact (`liblinux.a`) and the ring-split
artifact (`linux` ELF) share the kernel implementation —
schedulers, allocators, VFS, networking, etc. The only
difference is the entry path.

**Why:** Same reason as I6. If library mode forks, it becomes
LKL again — useful but not Linux.

**Enforcement:** Library-mode build links the same `.o` files
as ring-split build, except for `arch/um/library_main.o` vs
`arch/um/as_user.o`.

## I8: Time-travel mode does not require its own kernel

Enabling `CONFIG_UML_TIME_TRAVEL_SUPPORT` changes which clock
the kernel reads (Layer 1 op + Layer 2 gate) and the scheduling
discipline at the boundary. It does not require a separate
kernel build, separate scheduler, or separate `arch/um/`
subdirectory.

**Why:** Time-travel is the killer feature; making it cheap
to enable means we use it more.

**Enforcement:** `time-travel` profile and `research` profile
share most code; the diff is bounded to `arch/um/time-travel/`.

## I9: Snapshot/restore is profile-orthogonal

Snapshot/restore (the fuzz-profile feature) works with any
backend, any sanitizer set, any sanitizer state. A snapshot
taken on ptrace can be restored on ptrace; a snapshot on
KVM restored on KVM.

**Why:** If snapshot is backend-specific, fuzzing pipelines
have to commit to a backend, which weakens our story.

**Enforcement:** snapshot/restore is implemented at Layer 2
(checkpoint hooks gated by static key); the actual state-
serialization logic delegates to the backend's `read_guest_regs`
and `write_guest_regs` ops.

**Caveat:** Cross-backend restore (snapshot on ptrace, restore
on KVM) is explicitly NOT promised. Each backend's restore
state is opaque to the others.

## I10: Host kernel is not modified

UML must run on stock host kernels. We do not require kernel
patches, custom modules, or root.

**Why:** If we require host changes, we lose every CI environment,
every container deployment, every cloud VM. UML's reach is "any
Linux".

**Enforcement:** CI runs against multiple host kernel versions
(latest mainline, latest stable, latest LTS, oldest supported).

**Caveat:** KVM backend requires host KVM (which is in every
mainline kernel). Seccomp backend requires `CONFIG_SECCOMP_FILTER`
(which is in every relevant kernel). These are not "host
modifications", they are existing host features we use.

## I11: Profiles are buildable in isolation

A user who wants only `prod-fast` should be able to build it
without compiling `research` features. Disk usage and build time
of one profile must not depend on another.

**Why:** If prod-fast users are forced to compile in research
features they don't want, the sandbox-profile argument falls
apart (you can't have a minimized binary if you can't build
without optional features).

**Enforcement:** `make ARCH=um uml/prod-fast` builds in <2 min
on a developer machine and produces a binary <50 MB.

## I12: Decisions are reversible

The architecture commits to layered abstractions, not to specific
choices within each layer. We can replace ptrace with seccomp
(done). We can add KVM backend (planned). We can replace KASAN
shadow with hardware MTE (someday). The architecture survives
each change.

**Why:** A 24-month plan that bakes in 2026 implementation choices
will be obsolete by 2028. Layer boundaries are the durable
commitment; what's behind each boundary can move.

**Enforcement:** Every decision in `04-risks/decisions-log.md`
notes its expected lifetime and triggers for revisitation.
