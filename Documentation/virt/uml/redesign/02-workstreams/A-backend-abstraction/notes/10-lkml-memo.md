# A-01.10 — LKML design memo (draft)

**Status:** Draft for review. Do not send to LKML yet — this is an
internal artifact pending project-owner sign-off and the wider review
described in 04-risks/political-lkml-acceptance.md.

**Format:** Plain-text, formatted for git-send-email. No HTML. ~2
printed pages.

---

```
Subject: [RFC] um: introduce typed backend ops table (struct um_backend_ops)

Hi Richard, Johannes, Benjamin, Anton, Tiwei -

This is a design RFC, not a patch series. I'd like feedback on the
shape of a small abstraction layer in arch/um/ before writing the
refactor that follows it.

Today, arch/um/ supports two trap mechanisms (ptrace and the seccomp
mode merged in 6.16) selected at runtime via a single global flag,
`int using_seccomp`. There are 17 grep hits for that flag scattered
across 7 files in arch/um/{os-Linux,kernel}/skas/. The mechanism
works, but adding a third backend (KVM, gVisor-style) means adding a
third runtime branch at every site and broadening the flag to an
enum. That doesn't scale.

The proposal: lift the existing branches into a typed function-
pointer struct, struct um_backend_ops, with 18 ops in 5 categories.
Existing ptrace and seccomp code becomes the first two implementers;
KVM becomes the third without further refactoring.

Full draft header: arch/um/include/asm/backend.h
Full contract spec: Documentation/virt/uml/backend-contract.rst
Design notes:       Documentation/virt/uml/redesign/02-workstreams/
                    A-backend-abstraction/notes/

The 18 ops, abridged:

  Lifecycle/trap (4): probe, init, shutdown, run_userspace
  Memory      (4):    mm_attach, mm_detach, mm_map, mm_unmap
  Scheduling  (4):    thread_create, thread_start_idle,
                      context_switch, ipi_send
  Time        (3):    read_clock_ns, set_timer, read_persistent_clock_ns
  Debug       (3):    init_thread_regs, read_guest_regs, write_guest_regs

Five of these are HOT (per syscall / per fault / per task switch /
per clock read): run_userspace, mm_map, mm_unmap, context_switch,
read_clock_ns. The other 13 are cold (per fork, per init, per
debug-stop).

Selection mechanism, drawn from the gVisor pattern but simpler:

  - Single-backend Kconfig builds (CONFIG_UM_BACKEND_PTRACE_ONLY etc)
    expand a um_backend_dispatch() macro to a direct call. Zero
    indirect-dispatch cost; smaller binary. Used by sandbox and
    embedded profiles where the compile-time exclusion is valuable.

  - Multi-backend builds (CONFIG_UM_BACKEND_DYNAMIC) call through
    a global struct um_backend_ops *um_backend pointer set at boot
    by init_backend(). One indirect call per dispatch site;
    well-predicted target since um_backend doesn't change after
    init.

Boot-time selection: a backend= command line param with values
auto/ptrace/seccomp/kvm (and a force= variant that panics if the
requested backend's probe fails). Existing seccomp= aliases to the
new param for one transitional release.

Why this and not just keep extending using_seccomp:

  1. The branches today are scattered across the trap loop, the
     mm setup path, and the debug-error dump. New backend = ~50
     more branches. With a typed table the cost is O(1) per
     backend implementation.

  2. The KVM backend's "trap" is a KVM_RUN ioctl, not a SIGSYS
     handler. Forcing it into the using_seccomp shape would mean
     a 3-way enum at every site and a backend-specific side
     channel for KVM-only state (kvm_run mmap pointer, etc.).
     A typed table absorbs the per-backend state cleanly via
     mm_id and task->thread without a side channel.

  3. ptrace+seccomp share more than they differ. With the table,
     `arch/um/backend/common/` will hold the shared jmp_buf
     thread machinery and the timer dispatch, called from both
     backends' ops tables. ~10 of the 18 ops are literally
     identical between ptrace and seccomp today.

  4. The contract is versioned (UM_BACKEND_CONTRACT_VERSION u32).
     Adding new ops doesn't bump the version (old backends report
     -ENOSYS); only signature changes do. This avoids the
     "every backend must change in lockstep" trap.

What I'm asking for:

  (a) Critique of the op set. Are 18 too many? Too few? Is the
      hot/cold split right? Is anything missing that workstream
      D (KVM) will hit a wall on?

  (b) Critique of mm_id field ownership. Today mm_id has a few
      seccomp-only fields the ptrace backend ignores. The
      proposal keeps that pattern (with documentation) rather
      than per-backend mm_id allocation. ~24 bytes per mm in the
      worst case.

  (c) Anything in the seccomp work (Benjamin, hello) that this
      table doesn't accommodate cleanly? The notes/05-seccomp-
      sketch.md walks every seccomp branch and maps it to an op;
      I'd like a second pair of eyes.

  (d) The proposed transition path: A-01 is the design (this
      RFC). A-02 (~6 weeks) refactors the existing ptrace path
      onto the table without behavior change. A-03 (~4 weeks)
      does the same for seccomp. Conformance suite + perf CI
      land before any code is removed. Workstream A is ~24
      engineer-weeks total.

What this RFC does NOT propose:

  - Reimplementing any kernel logic. UML stays the upstream
    kernel; only the host interface (arch/um/) changes.
  - Adding KVM support. That is a separate workstream (D, ~6
    EM) that consumes this contract once stable.
  - Changing observed behavior. The first-pass refactor (A-02,
    A-03) is byte-equivalent to today's behavior on every test;
    the conformance suite enforces it.
  - Time-travel mode changes. Time-travel logic moves to a Layer 2
    static-key gate in a separate workstream (B), not into the
    backend table.

Full background on the broader plan (24 EM total, 4 workstreams):
Documentation/virt/uml/redesign/00-vision.md and the README in
the same directory. This RFC is workstream A's first task only.

Happy to discuss on-list, on netdev, or at the next UML BoF.

Thanks,
- Michael (with claude-code as co-author)

[1] arch/um/include/asm/backend.h         — the header
[2] Documentation/virt/uml/backend-contract.rst — contract spec
[3] Documentation/virt/uml/redesign/00-vision.md — broader plan
```

---

## Notes on the memo's tone (for project owner)

- **Addressed to maintainers by first name** as is LKML custom for
  RFCs that follow prior interaction. If this is a cold introduction,
  switch to "Hi linux-um, Richard, …" and link a one-paragraph
  introduction.
- **Asks for specific feedback** (4 numbered items) rather than a
  generic "thoughts?" — per Tazaki RFC post-mortem in
  `political-lkml-acceptance.md`, vague RFCs receive vague responses.
- **Explicit non-goals** — pre-empts the "you're trying to rewrite
  UML" objection that would otherwise dominate.
- **Length:** 95 lines / ~2 printed pages. Short enough to be read
  cold; long enough to be technically grounded.
- **Co-authored disclosure** — per
  `Documentation/process/coding-assistants.rst` (April 2026), AI
  contribution must be acknowledged. The "(with claude-code as
  co-author)" line is the lightweight form; full Co-developed-by:
  trailers belong on patches, not RFCs.

## Pre-send checklist

Before this memo is actually sent:

1. [ ] Project owner reviews and edits.
2. [ ] Header (`arch/um/include/asm/backend.h`) compiles standalone
   in the kernel build (currently smoke-tested standalone with
   `gcc -I/tmp/stubs/`; needs full kernel-build test).
3. [ ] `backend-contract.rst` is reachable from `Documentation/virt/uml/`
   (already true; verify no broken cross-refs).
4. [ ] Decisions log entry D8 is committed.
5. [ ] Run `scripts/checkpatch.pl --no-tree --strict` on any
   patch artifacts (none in this RFC, but the follow-up A-02 patches
   will need it).
6. [ ] Run `scripts/get_maintainer.pl arch/um/include/asm/backend.h
   Documentation/virt/uml/backend-contract.rst` to confirm the
   recipient list.
7. [ ] Start with `[RFC]` prefix to signal "design discussion, not
   merge candidate."

## Out of scope for this memo

- A-04 (Kconfig design) details. Mentioned briefly; full proposal
  in a separate later RFC.
- D (KVM backend) implementation. Mentioned as a downstream
  consumer; full design in workstream D.
- Performance numbers. The 100 ns / 300 ns / 1 µs figures are
  cited from `00-vision.md`; the actual perf-CI numbers (A-07)
  come later.
