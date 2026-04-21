# BPF x86 hygiene patches — submission-readiness notes

Series location: `/tmp/bpf-hygiene-v1/`

## Files

- `0000-cover-letter.patch` — cover letter (edited with real subject + blurb; base-only version originally had ~60 prerequisite-patch-id lines from the UML work on the source branch, now cleaned up).
- `0001-bpf-x86-explicitly-include-asm-cpufeature.h.patch`
- `0002-bpf-x86-use-instruction_pointer-helpers-in-ex_handle.patch`

## Pre-submission cleanups

Two things to handle before sending to LKML:

1. **Strip the `Co-authored-by:` trailer from 0002.** checkpatch
   flags it as a non-standard signature. LKML convention is
   either `Co-developed-by:` (lowercase -by, title-case initial),
   or just drop it. Easiest: `git rebase -i` on the
   submission branch (not the main branch) and drop the trailer
   line from the commit message. Keep `Signed-off-by:`.

2. **Re-Sign-off-by.** The current author line reads
   `mjbommar <mjbommar@server3.(none)>` because `format-patch`
   picked up the committer from the env. Re-run `git commit
   --amend --reset-author` on a submission branch to use
   `michael.bommarito@gmail.com` cleanly, or edit the From:
   header in the .patch files manually before `git send-email`.

## checkpatch findings (false positives, no action needed)

- 0001: "Consider using `<linux/cpufeature.h>`". Explained in
  the commit message: `<linux/cpufeature.h>` is the
  `GENERIC_CPU_AUTOPROBE` device-module-probing wrapper and
  does not expose `boot_cpu_has` / `X86_FEATURE_*` macros; the
  arch-specific header is correct here, consistent with the
  surrounding `<asm/*>` headers.

## Build verification

Both patches tested against x86_64 defconfig with BPF_JIT=y and
BPF_SYSCALL=y.

```
make ARCH=x86_64 O=/tmp/x86-probe defconfig
./scripts/config --file /tmp/x86-probe/.config \
    --enable BPF_SYSCALL --enable BPF_JIT
make ARCH=x86_64 O=/tmp/x86-probe olddefconfig
make ARCH=x86_64 O=/tmp/x86-probe -j$(nproc) \
    arch/x86/net/bpf_jit_comp.o
```

Baseline (no changes): 65656-byte `bpf_jit_comp.o`.
After patch 1:          65656 bytes (identical).
After patch 2:          65656 bytes (identical).

No codegen change, no link change, no ABI change. Both patches
are pure hygiene that make the TU's dependencies explicit and
align with the kernel's `instruction_pointer()` helper idiom.

## Submission targets

- `bpf@vger.kernel.org` — primary list.
- `netdev@vger.kernel.org` — subsystem cc (`arch/x86/net/`).
- `x86@kernel.org` — cc because the file lives under `arch/x86/`.
- Alexei Starovoitov `<ast@kernel.org>` — BPF maintainer.
- Daniel Borkmann `<daniel@iogearbox.net>` — BPF maintainer.
- Linus is on the tree obviously — no direct CC needed for
  bpf-next.

Use `./scripts/get_maintainer.pl --file arch/x86/net/bpf_jit_comp.c`
for the current list right before `git send-email`.

## Why this series stands alone

The original context for these two patches is the UML kernel's
attempt to reuse `arch/x86/net/bpf_jit_comp.c` wholesale
(decisions-log D43 on branch `uml-redesign-plan`). That broader
port is deferred pending an upstream-collaborative refactor
(D43 option B2). These two hygiene fixes benefit bare-metal x86
independently — they make the TU's dependencies self-evident
and migrate direct pt_regs field access to the generic helper
idiom used elsewhere in the tree. They should land regardless
of whether UML ever reuses this file.

The commit messages deliberately do NOT mention UML; they stand
as x86 BPF JIT hygiene on their own merits. Reviewers who ask
"what motivated this?" get an honest answer (I was building it
in a context where the transitive-include chain was thinner),
without the UML context being load-bearing.

## v1 → v2 if review feedback comes in

Keep the local branch `uml-redesign-plan` as-is; cherry-pick
onto a submission-specific branch for each iteration:

```
git checkout -b bpf-hygiene-v1 bpf-next/master    # or linux master
git cherry-pick e2b686c96218 5b95b1bb3e6a
# apply review-feedback edits via git commit --amend / rebase -i
git format-patch -2 --cover-letter --subject-prefix='PATCH bpf-next v2'
```

## Status

- Patches: **ready** (minor pre-flight cleanups above).
- Not yet sent to LKML. Waiting on user decision to push.
- Branch-side tracking: decisions-log D43 Status note +
  06-port-bpf-jit.md Status header document the deferral of
  the broader UML port.
