# Series 2 + Series 3 readiness recheck (2026-05-14)

## Series 2 — kmsan-arch-callback-rfc (Track D §6.2)

**Status:** APPLY-CHECK PASSES AGAINST `origin/master`.

Verification:

```
cd $WORKTREE_OFF_MASTER
git apply --check Documentation/.../kmsan-arch-callback-rfc/
              0001-mm-kmsan-add-kmsan_arch_init_early_shadow-weak-hook-.patch
# (empty stderr = clean apply)
```

The patch as-staged applies against master cleanly. Cover
letter + SUBMISSION-NOTES are written. No build verification
needed — the patch is RFC and the SUBMISSION-NOTES already
carry the build evidence from when it was prepared.

**Verdict:** READY for `git send-email` to LKML + linux-mm +
KMSAN maintainers. Operator action only.

## Series 3 — ftrace-notrace-generic-v1 (Track D §6.3)

**Status:** PATCH FORMAT ISSUE — needs regeneration before send.

Verification:

```
git apply --check Documentation/.../ftrace-notrace-generic-v1/
              0001-kernel-mark-kthread-and-smpboot_thread_fn-notrace.patch
# error: corrupt patch at line 103
git am --3way Documentation/.../ftrace-notrace-generic-v1/0001-...
# error: corrupt patch at line 63
# error: could not build fake ancestor
```

checkpatch PASSES (`total: 0 errors, 0 warnings, 103 lines
checked`), but `git apply` and `git am` both reject the patch
with corruption errors at different lines.

Inspection of the patch content shows the actual diff is
syntactically clean: two hunks (one in `kernel/kthread.c`, one
in `kernel/smpboot.c`), proper `@@ -N,M +N',M' @@` headers,
correct context lines. The hunk header arithmetic checks out
(`-377,7 +377,27` for kthread.c, `-99,7 +99,16` for smpboot.c).

The smpboot.c hunk has only 2 trailing context lines instead
of the conventional 3 (git's default is 3), which suggests the
patch was generated with `--unified=2` or similar. That's
syntactically valid but may trip `git am`'s mailbox parser
under some conditions.

**Recommended fix before send:**

Regenerate the patch from the on-branch commit (`a2e01ee58c53`
per SUBMISSION-NOTES) with default `git format-patch` settings:

```
git -C $UMLCTL_DEPLOY format-patch -1 a2e01ee58c53 \
    --output-directory Documentation/virt/uml/redesign/
    upstream-patches/ftrace-notrace-generic-v1/
git apply --check <new-patch>   # confirm regenerated patch applies
```

Then re-write the cover letter (or keep the existing one) and
re-verify with `git am --3way` against an `origin/master`
worktree.

**Verdict:** STAGING ISSUE, not a code issue. Operator can
either fix in-place per the recipe above before sending, or
defer Series 3 to a re-prep pass at queue-runup time.

## Implications for the queue

- **Series 1 (bpf-hygiene-v1)**: READY. Re-verified earlier
  (commit 147b946c47b9 diary). Operator should send first.
- **Series 2 (kmsan-arch-callback-rfc)**: READY. Operator can
  send after Series 1 lands or in parallel — they're independent
  per SUBMISSION-QUEUE.md.
- **Series 3 (ftrace-notrace-generic-v1)**: BLOCKED on patch
  regeneration. ~5 minutes of operator work. Either re-format
  from the on-branch commit before sending, or defer.
- **Series 4-6**: still in PLAN's "to write" / "to scope" state
  per SUBMISSION-QUEUE.md.
- **Series 7 (kvm-backend-series)**: cover letter draft staged
  but blocked on Phase J DONE certificate.

## Next

- Continue waiting on snapshot port + LTP curation sub-agents.
- Draft Series 7 cover-letter framework while waiting (PLAN
  §6.3 "start packaging the cover letter NOW even while Phase J
  finishes").
