# Rebase Plan: `umlctl-deploy` → maintainer tree on `torvalds/master`

**Date**: 2026-05-26
**Author**: Michael Bommarito + Claude Opus 4.7

## Executive Summary

**Do NOT rebase the 1,236-commit branch.** Create a maintainer tree
from `torvalds/master` with standard kernel subsystem branch naming,
then replay curated patch series onto it.

---

## Phase -1: Establish Maintainer Tree

### Branch structure (mirrors Johannes Berg's `uml/linux.git`)

```
mjbommar/linux.git
├── master          ← tracks torvalds/master (never diverges)
├── next            ← development: all series queued for next merge window
├── fixes           ← bugfixes for current -rc cycle (cherry-picks from next)
└── for-linus       ← (created per merge window) signed tag for pull request
```

**Tag convention**: `uml-for-linux-<version>` (e.g., `uml-for-linux-7.2-rc1`)
matching Johannes's existing pattern (`uml-for-linux-6.19-rc1`, etc.).

### Branch semantics

| Branch | Base | Contents | When to update |
|--------|------|----------|----------------|
| `master` | `torvalds/master` | Identical to Linus HEAD | `git pull torvalds master` after each -rc / release |
| `next` | `master` (rebased each window) | All development patches in submission order | As patches are reviewed, revised, or new series added |
| `fixes` | Latest `-rc` tag | Only bugfixes for the current cycle | When a bug is found in code already in Linus's tree |
| `for-linus` | N/A (signed tag) | Points at `next` HEAD or `fixes` HEAD | Created when sending a pull request |

### Setup commands

```bash
# 1. Update master to current Linus
git checkout master
git pull torvalds master
git push origin master

# 2. Create next from master
git checkout -b next master
# (replay patch series here — Phase 2)
git push origin next

# 3. Create fixes from the current rc tag
git checkout -b fixes v7.1-rc5
git push origin fixes

# 4. For-linus tags (created at pull-request time)
git tag -s uml-for-linux-7.2-rc1 next -m "UML changes for 7.2-rc1"
git push origin uml-for-linux-7.2-rc1
```

### MAINTAINERS entry (target state)

When accepted as co-maintainer, the entry would add your tree:

```
USER-MODE LINUX (UML)
M:	Richard Weinberger <richard@nod.at>
M:	Anton Ivanov <anton.ivanov@cambridgegreys.com>
M:	Johannes Berg <johannes@sipsolutions.net>
M:	Michael Bommarito <michael.bommarito@gmail.com>
L:	linux-um@lists.infradead.org
S:	Maintained
T:	git git://git.kernel.org/pub/scm/linux/kernel/git/uml/linux.git next
T:	git git://git.kernel.org/pub/scm/linux/kernel/git/uml/linux.git fixes
T:	git https://github.com/mjbommar/linux.git next
T:	git https://github.com/mjbommar/linux.git fixes
F:	Documentation/virt/uml/
F:	arch/um/
F:	arch/x86/um/
F:	fs/hostfs/
```

Until then, the tree structure is ready for the conversation with
Johannes and the other maintainers — patches on `linux-um@lists.infradead.org`
demonstrate the work, and the tree structure shows you're ready to
maintain it long-term.

### Relationship to existing `umlctl-deploy` branch

```
umlctl-deploy                ← preserved forever (tag: umlctl-deploy-pre-rebase-20260526)
                                 1,236 commits of development archaeology

next                          ← the NEW upstream-facing branch
                                 clean patch series on torvalds/master
                                 this is what gets sent to LKML
```

The `umlctl-deploy` branch remains as the working branch for
fork-only infrastructure (umlctl, internal memos, experimental
configs).  `next` is the clean room for upstream patches.

---

- 347 of 1,236 commits are investigative/experimental (hypothesis
  testing, pool-replicate bisects, SMP-T## diagnostic cycles).
- The SQUASH-AUDIT-PLAN.md already identifies 6 introduce-then-revert
  pairs that would be bisect-breakers.
- The Series 7 audit derived 19 squash-target patches for kvm-v2.
  Replaying the raw 542 arch/um commits would undo that work.
- A raw `git rebase` of 1,236 commits onto a 5,502-commit-ahead base
  would produce hundreds of textual conflicts in files we never
  touched.

Strategy: **tag-and-rebuild**.

## Current State

| Metric | Value |
|--------|-------|
| Our branch | `umlctl-deploy`, 1,236 commits ahead of `master` |
| Our master | v7.1-rc1 (April 17 2026) |
| Linus mainline | v7.1-rc5+ (May 25 2026) |
| Commits behind Linus | 5,502 |
| Our arch/um/ commits | 542 |
| Our shared-code commits | 9 |
| Upstream arch/um/ changes since our fork | 9 |

---

## Phase 0: Preparation

### 0.1 Tag the current branch

```bash
git tag umlctl-deploy-pre-rebase-20260526 umlctl-deploy
git push origin umlctl-deploy-pre-rebase-20260526
```

Non-negotiable.  The 1,236-commit history is development archaeology.

### 0.2 Fetch latest torvalds/master

```bash
git fetch torvalds
```

If v7.1 final has dropped by execution time, use that instead of
rc5 — a release tag is a cleaner base.

### 0.3 Verify build environment

Build and boot from current `umlctl-deploy` HEAD as the known-good
baseline.  Record selftest numbers.  These are the "before" numbers
that validate the rebuild.

---

## Phase 1: Create the New Branch

```bash
git checkout -b umlctl-deploy-rebased torvalds/master
```

Verify the 9 upstream arch/um fixes are in the base:

```bash
git log --oneline --ancestry-path v7.1-rc1..HEAD -- arch/um/
```

---

## Phase 2: Replay Patches in Series Order

Instead of replaying 1,236 raw commits, replay **curated patch
groups** in dependency order matching the submission queue.

### Group 0: Shared-code patches (non-arch/um)

Files: `mm/mmap.c` (rss correction), `mm/mmu_gather.c` (deferred
batch), `kernel/kthread.c`, `kernel/smpboot.c`, `kernel/sched/core.c`,
`mm/kmsan/init.c`.

Method: `git diff` from old branch, apply with `--3way`.

Special handling:
- `arch/um/drivers/cow_user.c`: drop our fix — upstream `91e901c65b4d`
  is functionally identical and already in the base.

### Group 1: Series 4 — Backend ops abstraction

The `struct um_backend_ops` typed ops table.  ~12 patches.  Touches
~33 files under `arch/um/`.

**This is the hardest group** due to two manual merges:

#### Conflict 1: `arch/um/kernel/tlb.c`

Upstream `102331b66bca` adds `page_table_lock` guard in `um_tlb_sync`.
We completely restructured the function (deferred-free, sync_tlb_range,
backend ops dispatch).

Resolution:
1. Apply our full `tlb.c` diff (replaces upstream almost entirely)
2. Add back upstream `page_table_lock` guard before our `sync_tlb_lock`
3. Fix `kern_map` to use `region->prot & UM_PROT_EXEC` (upstream fix)
4. Build-test

#### Conflict 2: `arch/um/include/asm/pgtable.h`

Upstream `cd4126d48f7f` fixes `pte_read()`/`pte_exec()` by removing
`_PAGE_USER` check.  We remapped PTE bits, added NEEDSYNC, single-
store set_pte.

Resolution:
1. Apply our full `pgtable.h` diff
2. In `pte_read()`/`pte_exec()` conflict, adopt upstream pattern:
   `return !pte_get_bits(pte, _PAGE_PROTNONE)` with no `_PAGE_USER`

### Group 2: Series 5 — Static-key hot paths

~6 patches.  Mostly new files.  Low conflict risk.

### Group 3: Series 6 — Profile features (kprobes, ftrace, KFENCE, KCSAN, KMSAN)

~20 patches.  Mostly new files + Kconfig wiring.  Low conflict risk.

### Group 4: Series 7 — KVM v2 backend

**19 patches from SQUASH-AUDIT-PLAN.md.**  Do NOT replay the raw
175+ kvm-v2 commits.

`arch/um/backend/kvm-v2/` is entirely new — zero upstream conflicts.
Integration touches in existing files were resolved in Group 1.

This is the largest group by LOC but the safest from a conflict
perspective.

### Group 5: umlctl tooling + selftests + documentation

The umlctl Rust crate (`tools/uml/uml-launcher/`), kselftests
(`tools/testing/selftests/um/`), and documentation
(`Documentation/virt/uml/`).

All new paths with zero upstream collision.

- Upstream-bound content (selftests, operator docs): clean signed-off
  patches.
- Fork-only content (umlctl, planning memos): squashed commits.

### Group 6: Stragglers

Review `git log` for anything not covered by Groups 0-5.  Cherry-pick
individually.

---

## Phase 3: Conflict Resolution Summary

| File | Upstream change | Our change | Resolution |
|------|----------------|------------|------------|
| `tlb.c` | `page_table_lock` guard | Full rewrite | Manual merge: our rewrite + upstream lock |
| `pgtable.h` | `pte_read/pte_exec` fix | PTE bit remap + NEEDSYNC | Adopt upstream fix pattern |
| `stub.c` | CMSG fix | FS_BASE re-read | Different functions — auto-merge |
| `cow_user.c` | `kernel_strrchr` | Same fix | Drop ours |
| `x86_64_defconfig` | FRAME_WARN removal | UM_WORKER_PROCESS add | Non-overlapping |
| `Kconfig` | GCOV fix | 33 additions | Different config blocks — `--3way` |
| `mm/Kconfig` | split-lock disable | No changes | No conflict |

---

## Phase 4: Testing Gates

After each group:

| Gate | When | Command |
|------|------|---------|
| Build | Every group | `make ARCH=um O=$BUILD_DIR -j$(nproc)` |
| Boot | Groups 1, 4 | `$BUILD_DIR/linux init=/bin/echo hi` (both backends) |
| Selftests | Groups 4, 5 | cpython-parity, mt-mini, threaded-fork-malloc |
| CPython regrtest | Final | Full suite on both seccomp + kvm-v2 |
| checkpatch | Before send-email | `scripts/checkpatch.pl --strict *.patch` |

---

## Phase 5: Execution Order + Time Estimates

| # | Step | Estimate |
|---|------|----------|
| 1 | Tag + fetch | 2 min |
| 2 | Baseline build+boot | 15 min |
| 3 | Create branch | 1 min |
| 4 | Group 0: shared-code | 30 min |
| 5 | Build gate | 5 min |
| 6 | Group 1: Series 4 backend ops | **2-4 hours** (hardest) |
| 7 | Build + boot gate | 15 min |
| 8 | Group 2: Series 5 static keys | 1 hour |
| 9 | Build gate | 5 min |
| 10 | Group 3: Series 6 profiles | 1-2 hours |
| 11 | Build gate | 5 min |
| 12 | Group 4: Series 7 kvm-v2 | **4-8 hours** (19-patch construction) |
| 13 | Build + boot gate (both backends) | 30 min |
| 14 | Selftest gate | 1 hour |
| 15 | Group 5: umlctl + selftests + docs | 2 hours |
| 16 | Group 6: stragglers | 30 min |
| 17 | Full selftest + CPython regrtest | 2-4 hours |
| 18 | checkpatch on all patches | 30 min |

**Total: 2-3 days of focused work.**  Bottleneck is Group 4 (Series 7
squash construction).

---

## Phase 6: Rollback Plan

The original branch is never modified.  The new branch can be
abandoned at any point:

```bash
# Level 1: restart a group
git reset --hard <last-good-commit>

# Level 2: abandon the entire rebuild
git checkout umlctl-deploy
git branch -D umlctl-deploy-rebased

# Level 3: recover from original branch damage
git checkout umlctl-deploy-pre-rebase-20260526
git checkout -b umlctl-deploy-recovered
```

---

## Phase 7: Final Verification

### 7.1 Tree-level diff

```bash
git diff umlctl-deploy-pre-rebase-20260526..umlctl-deploy-rebased -- arch/um/
```

Should show ONLY: upstream fixes adopted, investigative code removed,
cow_user.c duplicate dropped.  No missing functionality.

### 7.2 File-level audit

```bash
git diff --name-only --diff-filter=A master..umlctl-deploy-pre-rebase-20260526 -- arch/um/ | sort > /tmp/old
git diff --name-only --diff-filter=A torvalds/master..umlctl-deploy-rebased -- arch/um/ | sort > /tmp/new
diff /tmp/old /tmp/new
```

### 7.3 Series extraction

```bash
git format-patch torvalds/master..HEAD -o /tmp/patches
scripts/checkpatch.pl /tmp/patches/*.patch
```

### 7.4 Boot + selftest parity

Compare against Phase 0.3 baseline.  Any regression = something lost.

---

## Key Reference Files

- `SQUASH-AUDIT-PLAN.md` — 19-patch blueprint for Series 7
- `SUBMISSION-QUEUE.md` — 7-series dependency order
- `arch/um/kernel/tlb.c` — primary conflict point
- `arch/um/include/asm/pgtable.h` — second conflict point
- `arch/um/Kconfig` — highest-touch-count shared file

---

## Execution Log (2026-05-26)

### Phase 0: Preparation — DONE
- Tag `umlctl-deploy-pre-rebase-20260526` created and pushed
- `torvalds/master` fetched at `e8c2f9fdadee` (v7.1-rc5+)

### Phase -1: Maintainer Tree — DONE
- `master` updated to `torvalds/master` (e8c2f9fdadee)
- `next` created from `master`
- `fixes` created from `v7.1-rc5`
- All pushed to `origin`

### Phase 1-2: Branch Creation + Replay — DONE

All 1,006 files replayed onto `torvalds/master` base.

Upstream conflict resolution:
- `pgtable.h`: adopted upstream `pte_read`/`pte_exec` fix (cd4126d48f7f)
- `tlb.c`: added upstream `page_table_lock` guard (102331b66bca)
- `stub.c`: adopted upstream CMSG_DATA rvalue fix (4076f7329832)
- `Kconfig`: adopted upstream GCOV Clang 20/21 fix (6522fe5c1b00)
- `x86_64_defconfig`: removed FRAME_WARN per upstream (92d5c5c04eaa)
- 7 shared-code files (sched.h, core.c, Makefile, etc.): patched
  on top of upstream with `git apply --3way`, all clean

### Commits on `next` (7 series-level commits)

```
71069f26ecc1 Documentation/virt/uml: add comprehensive UML documentation
dba578471251 um: add umlctl launcher tool + kselftests
12b19d012f9f um: kvm-v2: add KVM-based backend for User-Mode Linux
86b74b8993d0 um: backend ops abstraction + SMP + TLB sync + infrastructure
8301623cf952 kthread/smpboot: add notrace to signal-adjacent thread functions
be9dbd18e75e mm/kmsan: add arch early-shadow callback for UML physmem layout
c1084ea01e06 um: bpf: add JIT stub and x86 hygiene for UML
```

### Build Verification — DONE
- seccomp backend: builds clean, boots clean (v7.1.0-rc5-dirty)
- kvm-v2 backend: builds clean

### What Remains
- Each series-level commit can be further split into individual
  patches per the SUBMISSION-NOTES when preparing for LKML
- Series 1-3 are small enough to send as-is
- Series 4 needs ~12 patch split (per SUBMISSION-NOTES)
- Series 7 needs ~19 patch split (per SQUASH-AUDIT-PLAN.md)
- checkpatch pass on all final patches
- kvm-v2 boot test on the new base
- Full CPython regrtest on new base (both backends)
