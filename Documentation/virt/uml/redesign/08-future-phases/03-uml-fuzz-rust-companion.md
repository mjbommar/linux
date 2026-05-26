# 03 — `tools/fuzz/uml-fuzz/` — a Rust, in-tree, syzlang-compatible
# companion fuzzer

**Status:** future phase. Not committed. Opens only after C-08
(`vm/uml` syzkaller backend) has landed and run long enough to
produce measurable data on its limits. Strict per-the-parking-lot
rule: begins where C-08 ends.

## The one-paragraph pitch

Once `pkg/vm/uml/` ships upstream in syzkaller, every kernel
developer running `syz-manager` can target UML and syzbot gets
UML in rotation. That covers distributed / cluster-scale
fuzzing with the full kernel-community triage pipeline
(`vision-success-criterion-#6`). It does **not** cover two
UML-specific capabilities that nobody is exploiting today:

1. **UML's native snapshot/forkserver** (workstream C-09) can
   run 1000+ iter/s on one CPU. Syzkaller's generic VM
   abstraction + its QEMU-default comparator means it leaves
   some of that speed on the table when the VM is UML. A
   fuzzer that talks to fd-198/199 directly, without going
   through `pkg/vm/vmimpl`, should do 5–10× iter/s on the same
   host.

2. **UML's time-travel / record-replay** (`time-travel`
   profile) supports rewinding kernel state. A fuzzer that
   snapshots, crashes, rewinds to the pre-crash snapshot, and
   mutates from there is an active-research capability
   syzkaller doesn't have on any target. UML is one of the
   only kernel-research targets where this is even possible.

`tools/fuzz/uml-fuzz/` would be the home for both. Position it
as a **companion** to syzkaller (not a replacement), targeting
research workflows that the community infrastructure doesn't
serve.

## Why this isn't "just roll our own syzkaller"

The analysis is in `04-risks/decisions-log.md` D48. Short
version:

- **The grammar is the 90% that would take a decade to
  rewrite.** Answer: we consume syzkaller's own `sys/linux/
  *.txt` descriptions (Apache 2.0 → GPL 2.0 compatible),
  either by vendoring them under `tools/fuzz/uml-fuzz/
  descriptions/` with attribution, or by pointing at a
  system-installed syzkaller tree at runtime.
- **syzbot is the distribution channel.** Answer: we don't
  replace it. C-08 stays as the upstream play for syzbot
  adoption. `uml-fuzz` is the local-CI / research-workflow
  companion.
- **Corpus compounding.** Answer: syzkaller's corpus files
  are syzlang-serialized. A Rust parser for syzlang can read
  them directly, so we inherit the corpus on day one.

Accepting these constraints, the tool is no longer a fuzzer
re-implementation — it's a UML-specific executor + mutator
loop that reuses 90% of syzkaller's ecosystem (grammar +
corpus + serialization format) and differs only in the two
specific capabilities above.

## What an end user gets

After this lands, a kernel developer on their workstation can:

```
# Local CI-style fuzz run, no syz-manager cluster needed.
make ARCH=um uml/fuzz
cargo run --release -p uml-fuzz -- \
    --kernel /tmp/uml-fuzz/linux \
    --duration 30m \
    --descriptions $SYZKALLER/sys/linux/
# → "uml-fuzz: 1.8M testcases, 3 crashes, 2 unique, report at
#    /tmp/uml-fuzz-report.html"
```

For reproducer minimization on a syzbot-reported crash:

```
uml-fuzz reproduce --input syzbot-crash.syz \
    --kernel /tmp/uml-fuzz/linux --minimize
# → "minimized from 847 calls to 3 calls:
#    1. openat(AT_FDCWD, "./loop", O_RDWR|O_CREAT, 0)
#    2. ioctl(r0, LOOP_SET_FD, r1)
#    3. ioctl(r0, LOOP_CTL_REMOVE, 0)
#    crash still reproduces in 0.4s."
```

For time-travel-aware research fuzzing (the genuine wedge):

```
uml-fuzz timetravel \
    --kernel /tmp/uml-fuzz/linux \
    --snapshot-at "mount_bdev" \
    --descriptions $SYZKALLER/sys/linux/
# → "snapshotting at kernel function mount_bdev;
#    10k variants per snapshot; rewinding on panic;
#    first panic at variant 82 in 'loop: invalid_block_device'."
```

None of these are things a syzkaller user can do today. The
first one is faster; the second one is simpler (no
syz-manager, no cluster); the third one is a research
capability that needs UML-level execution-model control.

## Rough shape

```
tools/fuzz/uml-fuzz/
├── Cargo.toml
├── README.md
├── descriptions/
│   └── README.md          # explains the vendor-or-point
│                          # choice and licensing
├── src/
│   ├── main.rs            # CLI via clap (matches uml-launcher style)
│   ├── syzlang/
│   │   ├── parser.rs      # nom-based parser for .txt grammar
│   │   ├── compiler.rs    # type resolver + IR
│   │   └── program.rs     # syzlang-serialized program format
│   ├── corpus.rs          # load/store compatible with syz-db
│   ├── mutator.rs         # grammar-aware mutation
│   ├── executor/
│   │   ├── forkserver.rs  # direct fd-198/199 integration
│   │   └── launcher.rs    # wraps uml-launcher run --forkserver
│   ├── coverage.rs        # KCOV bitmap collection
│   ├── minimize.rs        # crash-input minimizer
│   └── timetravel.rs      # snapshot/rewind/retry (the wedge)
└── tests/
    └── selftests.rs       # parse syzkaller's own corpus, assert
                           # round-trip; exec against a known
                           # binary, assert contract
```

Size estimate: **~10–15 kLOC of Rust**, **~3–4 months of
focused work** for one engineer. Comparable to
`tools/uml/uml-launcher` in code-organization style, larger in
total volume.

## What has to be true before we start

- **C-08 landed upstream.** Without a `vm/uml` driver in
  syzkaller first, this project doesn't have a credible
  "companion to what, exactly?" answer. C-08 establishes UML
  as a legitimate syzkaller target; `uml-fuzz` then serves
  niches that target-via-syzkaller doesn't.

- **C-09 v2 ceiling removed or well-understood.** The current
  forkserver v1 caps at one iteration per guest-init (D41/D42).
  A companion fuzzer that saturates at 1 iter per fork isn't
  compelling. Either v2 has to land or we need an explicit
  pattern for "keep parent alive indefinitely" that works
  around the ceiling. Right now both are open questions.

- **Measured evidence that syzkaller-on-UML is slower than it
  could be.** C-08 should establish a number. If
  `pkg/vm/uml/` saturates UML's forkserver anyway, the
  speed-wedge evaporates and only the time-travel wedge
  remains — which is a smaller project and might fit under a
  different shape (e.g. a Python + syzkaller plugin).

## When to revisit

Revisit triggers this note:

1. C-08 has been landed in syzkaller for >6 months and syzbot
   is running UML in rotation.
2. Measured data shows syzkaller-on-UML stuck below 1000
   iter/s on workstation hardware, **and** the C-09 v2
   freezer-cgroup redesign has shipped (so the ceiling
   explanation isn't just "v1 was always going to be slow").
3. An operator has written a one-pager making the
   time-travel-fuzzing case concretely (a specific class of
   bugs that requires rewind-and-mutate), not abstractly.

Until those three, the `08-future-phases` placement is
correct: it's a good idea, but its leverage depends on prior
pieces landing and producing evidence.

## Relation to the rest of the plan

- **C-07 (KMSAN).** Once KMSAN runtime-debug works on UML, any
  fuzzer (syzkaller or `uml-fuzz`) benefits from the
  uninit-use detection it provides. Not a dependency on
  `uml-fuzz` specifically; same statement holds for the
  in-kernel KASAN we already have.

- **`D` (KVM backend).** Irrelevant to `uml-fuzz`. The
  forkserver doesn't care which backend UML uses; KVM would
  just make everything faster proportionally.

- **vision success criteria** (`00-vision.md`):
  - #2 "syzkaller at >1000 iter/s" — **met by C-08**, not by
    this.
  - #6 "syzbot has UML in rotation" — **met by C-08**, not by
    this.
  - **No criterion currently names this companion tool.** If a
    future vision refresh adds one ("time-travel-aware fuzzing
    as a research primitive"), this is where to scope it.

## See also

- `02-workstreams/C-profiles-and-gaps/08-syzkaller-vm-uml.md` —
  the C-08 workstream that must land first.
- `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md` —
  the forkserver wire protocol this tool would bind to
  directly.
- `04-risks/decisions-log.md` D48 — the "Rust companion vs
  pure-C-08" evaluation.
- `08-future-phases/02-snapshot-to-disk.md` — v2 snapshot work
  that would enable an ELF-core-based variant of the
  time-travel wedge.
