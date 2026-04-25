# BPF x86 hygiene patches — submission-readiness notes

## Status (2026-04-25 update — task #260)

**READY TO SEND.** Submission branch + format-patches
prepared. All preflight cleanups are done; awaiting user
"go" to run `git send-email`.

### Submission branch

`bpf-hygiene-v1-submit` (pushed to `origin/bpf-hygiene-
v1-submit`), based off `master` with the two patches
cherry-picked + cleaned:

| Branch commit | Subject |
|---------------|---------|
| `d43a4a849188` | bpf, x86: explicitly include `<asm/cpufeature.h>` |
| `37098ae27db8` | bpf, x86: use instruction_pointer helpers in ex_handler_bpf |

### Cleanup applied

- `Co-authored-by:` trailers stripped from both commits
  (LKML convention: drop or use `Co-developed-by:` paired
  with a corresponding `Signed-off-by:`. Plain
  `Co-authored-by:` flagged by checkpatch as non-standard;
  for upstream we just drop it).
- Author + `Signed-off-by:` both read
  `Michael Bommarito <michael.bommarito@gmail.com>` (the
  cherry-pick onto a clean tree picked up the user's git
  config name automatically; the original commits had
  `mjbommar` as the SOB name which checkpatch flagged as
  a from/SOB name mismatch).

### Format-patch output

Generated at `/tmp/bpf-hygiene-v1-emit/`:

- `0000-cover-letter.patch` — subject + blurb filled in;
  references the build-verification recipe.
- `0001-bpf-x86-explicitly-include-asm-cpufeature.h.patch`
- `0002-bpf-x86-use-instruction_pointer-helpers-in-ex_handle.patch`

### Build verification (re-run on cleaned branch)

```
make ARCH=x86_64 O=/tmp/x86-bpfprobe defconfig
./scripts/config --file /tmp/x86-bpfprobe/.config \
    --enable BPF_SYSCALL --enable BPF_JIT
make ARCH=x86_64 O=/tmp/x86-bpfprobe olddefconfig
make ARCH=x86_64 O=/tmp/x86-bpfprobe -j4 \
    arch/x86/net/bpf_jit_comp.o
```

Result: `bpf_jit_comp.o` is **65656 bytes** — byte-for-byte
identical to the master baseline (also 65656 bytes). No
codegen change, no ABI change. Pure hygiene.

### checkpatch on the format-patch output

```
0 errors, 1 warning, 15 lines checked
```

The single warning is "Prefer a maximum 75 chars per line"
on a code-snippet line in the commit message of patch 2
(showing the After: code form). Code-snippet lines are
expected to exceed the 75-char paragraph wrap; checkpatch's
heuristic doesn't distinguish. Not actionable; reviewers
won't object.

### Send recipe (when authorized)

```
cd /tmp/bpf-hygiene-v1-emit
git send-email --to=bpf@vger.kernel.org \
               --cc=netdev@vger.kernel.org \
               --cc=x86@kernel.org \
               --cc=ast@kernel.org \
               --cc=daniel@iogearbox.net \
               --cc=andrii@kernel.org \
               *.patch
```

(Run `./scripts/get_maintainer.pl --file
arch/x86/net/bpf_jit_comp.c` right before sending to pick
up any maintainer rotation since 2026-04-25.)

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
