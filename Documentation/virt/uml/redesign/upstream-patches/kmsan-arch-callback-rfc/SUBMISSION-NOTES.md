# KMSAN arch-init callback — submission-readiness notes

Series location: this directory. **The `.patch` files are not
yet written** — the RFC patch is queued behind user sign-off
on the approach. This file captures the framing and routing
decided in D51 so that when the patch is written, the cover
letter and cc list are already settled.

## One-sentence summary

Add a weak `kmsan_arch_init_early_shadow()` hook called from
`kmsan_init_shadow()` and un-static
`kmsan_record_future_shadow_range()` so architectures that need
to mmap their shadow/origin VA regions at early boot can do so
and register the ranges through the existing generic sweep.

## Why this is a small, easy-to-accept patch

The patch is deliberately shaped to be the minimum semantic
surface addition:

- **No changes to existing function behavior.** Every current
  call site in `mm/kmsan/init.c`, every architecture's current
  init path, and every `mm_core_init` sequence runs identically
  to today after this patch.
- **One weak hook + one visibility change.** The hook adds a
  single call site in `kmsan_init_shadow()` whose default is
  empty; the visibility change (`static` → not) on
  `kmsan_record_future_shadow_range()` lets arch overrides use
  the same helper the generic code already uses, rather than
  reimplementing it.
- **x86 and s390 are byte-identical.** They don't define the
  weak override; their `vmlinux` output from the same `.config`
  is unchanged. Verify with `size(1)` + section dump.
- **~20 lines of generic code.** Easy review; fits in one
  patch.

The "sale" for Potapenko (KMSAN maintainer): it unlocks KMSAN
on an additional architecture (UML/x86_64) at zero maintenance
cost to the KMSAN core. UML is re-emerging as a credible
testing platform — ftrace, kprobes, kretprobes, function_graph,
KASAN/KFENCE/KCSAN/UBSAN, BPF JIT, AFL forkserver, and
`uml-launcher` Rust host tool all landed recently on
`uml-redesign-plan`. KMSAN-on-UML becomes a fast,
host-process-native regression-test target for the sanitizer
itself.

Cover letter text must make this argument without leaning on
UML partisanship — frame it as "enable KMSAN on additional
architectures", with UML as an existence proof rather than the
justification.

## Files that will exist here when the patch is written

- `0000-cover-letter.patch` — the cover letter. Draft sections:
  1. What the patch does (one paragraph, minimum-diff framing).
  2. Why the shape is right (no behavior change, new entrypoint
     only, weak-symbol pattern).
  3. Payoff (additional arch target for KMSAN testing; UML
     redesign context with links to
     `Documentation/virt/uml/redesign/` for reviewers who want
     to see why UML is worth extending for).
  4. RFC-level ask (shape approval before committing arch-side
     work that depends on it).
- `0001-mm-kmsan-add-kmsan_arch_init_early_shadow-weak-hook.patch`
  — the generic patch. Un-statics
  `kmsan_record_future_shadow_range`, declares the weak hook,
  calls it from `kmsan_init_shadow()`.
- Optionally `0002-*.patch` if we split the un-static into its
  own patch (debatable — likely clearer as one patch since
  neither change makes sense alone).

## LKML routing

Per `scripts/get_maintainer.pl` on `mm/kmsan/init.c` +
`include/linux/kmsan.h`:

| Role | Person | Comment |
|---|---|---|
| KMSAN maintainer | Alexander Potapenko `<glider@google.com>` | To: line. Decision-maker for the patch shape. |
| KMSAN reviewer / co-author | Dmitry Vyukov `<dvyukov@google.com>` | Cc:. Deep KASAN/KMSAN context. |
| UML maintainers | Johannes Berg `<johannes@sipsolutions.net>`, Anton Ivanov `<anton.ivanov@cambridgegreys.com>` | Cc:. Their ack strengthens the pitch — "UML side wants this and will land the arch override." |
| mm | Andrew Morton `<akpm@linux-foundation.org>` | Cc:. Generic-mm path. |
| lists | `linux-kernel@vger.kernel.org`, `linux-mm@kvack.org`, `linux-um@lists.infradead.org` | Cc:. |

Subject line for the cover letter:
```
[RFC PATCH] mm/kmsan: add kmsan_arch_init_early_shadow() weak hook for arch shadow VA init
```

The `[RFC]` prefix is deliberate — gives Potapenko an exit
valve to propose a different seam (e.g., a registration API
instead of a weak symbol) without making it feel like a
rushed merge request.

## Pre-send checklist (when the patch is written)

1. `scripts/get_maintainer.pl` on the actual diff — confirm
   the cc list above matches current reality.
2. `scripts/checkpatch.pl --strict` on each patch — expect
   clean (the generic part is small and formulaic).
3. Build-verify on x86_64 defconfig + `CONFIG_KMSAN=y`:
   - Baseline (no patch): record `vmlinux` size + `size` per
     section + `kmsan_*` symbol visibility.
   - With patch: verify identical `.text` / `.data` for
     existing archs. The weak hook costs exactly one
     `call __weak_stub` or a zero-sized nop in the generic
     path.
4. Build-verify on UML with the companion arch-side series
   (`uml-redesign-plan` branch). `make ARCH=um LLVM=1
   uml/research` + `CONFIG_KMSAN=y` should boot and run the
   KMSAN KUnit suite.
5. Cover letter:
   - States the payoff argument (additional arch for KMSAN).
   - Links to `Documentation/virt/uml/redesign/README.md`
     for reviewers who want platform context.
   - Includes KMSAN KUnit pass count from UML as concrete
     evidence.
6. **Do not** include any UML-redesign decisions-log
   references in the patch commit bodies — those stay in this
   file for lineage.

## Decisions log lineage (internal only)

- D44: four empirical probes + resolution establishing that
  the arch callback is the right shape. Probes 2+3+4 contain
  the specific failure modes for alternative approaches.
- D51: this submission strategy + LKML framing (load-bearing
  reference for anyone picking this up after the author).
- D45: in-fork scope policy — explains why UML-side work lives
  on `uml-redesign-plan` while the generic patch ships
  independently through LKML.

## Status

- [ ] Generic patch written.
- [ ] Cover letter written.
- [ ] `get_maintainer.pl` re-run against final diff.
- [ ] checkpatch --strict clean.
- [ ] Build-verify on x86_64 (identical behavior).
- [ ] Build-verify on UML (functional KMSAN).
- [ ] KMSAN KUnit results captured.
- [ ] User sign-off to send.
- [ ] `git send-email` to the routing table above.
- [ ] Upstream review round (expect feedback; bump to v1
      after any requested reshape).
- [ ] Merged into `mm-next`.
- [ ] Arch-side UML series (5-6 commits) lands on
      `uml-redesign-plan`; C-07 task closes.
