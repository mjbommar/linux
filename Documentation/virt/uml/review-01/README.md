# Review 01: Design vs Implementation Through A-05

This review compares the UML backend redesign as documented with the
implementation currently in tree, scoped **only through workstream
A-05**. It does **not** treat missing B/C/D work as a defect unless the
current A docs already claim that work landed.

## Overall judgment

The implementation is in a good place for architectural review. The ops
surface exists, backend selection is coherent enough to build on, and
A-05 added a real first-pass conformance layer.

The biggest risk is not that the architecture is wrong. The biggest risk
is that reviewers will read **different sources of truth** and come away
with different pictures of what has landed:

- The most current sources are `Documentation/virt/uml/backend-contract.rst`,
  `arch/um/include/shared/backend.h`, `arch/um/kernel/backend.c`, and the
  A-04/A-05 task docs.
- `Documentation/virt/uml/redesign/01-architecture/three-layers.md`
  still describes an older contract shape and older `backend=auto`
  semantics.
- Some comments in code still speak as if A-04 is future work even
  though selection and boot-param parsing have already moved.
- The contract doc's conformance section describes a deeper suite than
  A-05 currently implements; the current reality is "KUnit first pass +
  cross-backend/perf skeletons".

That means most of the recommendations below are about **freezing the
current design carefully and describing it honestly**, not redesigning it
from scratch.

## 1. Is the 18-op contract the right granularity?

### Ideas and thoughts

- The current split in `arch/um/include/shared/backend.h` is broadly the
  right size: 18 ops in 5 buckets, with the hot path narrowed to 5 ops.
- The most important design choice is not the absolute count. It is that
  the table separates:
  - backend-global lifecycle,
  - per-mm lifecycle,
  - per-thread scheduling,
  - hot trap/time primitives,
  - cold debug/introspection hooks.
- `mm_attach` and `mm_detach` should stay separate from `init` and
  `shutdown`. They model a different lifetime. Folding them into
  backend-global lifecycle would blur per-mm ownership and make teardown
  less explicit.
- `set_timer(cpu, deadline_ns, mode)` is a good example of the current
  granularity rule working. It is a cold op, so a mode tag is cheaper
  than exploding the table into three low-value ops.
- The main contract risk is not that 18 is obviously too many or too
  few. The risk is semantic drift in naming and meaning if docs and code
  keep disagreeing.
- The older architecture doc still shows a materially different surface
  (`syscall_dispatch`, `page_fault`, `host_io_submit`, different args
  block). That stale doc creates more review risk than the current op
  count itself.

### Assessment through A-05

- The 18-op contract is good enough to freeze for RFC discussion.
- I would not reopen folds or splits before maintainer feedback unless a
  concrete implementation pain appears.
- If maintainers push on anything, the likely pressure points are op
  naming and lifecycle semantics, not "18 vs 17 vs 20".

## 2. Is `init_backend()` in `linux_main` the right boot-order fix?

### Ideas and thoughts

- Yes, this is the right direction.
- The code now has real early dispatch points:
  - `start_uml()` dispatches `thread_start_idle`.
  - `read_persistent_clock64()` dispatches
    `read_persistent_clock_ns()`.
- Because those happen before the old `setup_arch()` point, backend
  installation has to happen earlier if the contract is going to cover
  those call sites.
- Installing the backend once, early, is cleaner than inventing a second
  bootstrap path that bypasses the ops table and later hands off to the
  "real" backend.
- A bootstrap direct-call routine would add another transition boundary,
  another place for selection bugs, and another thing KVM/ptrace/seccomp
  would all have to agree on.

### Caveats

- `init_backend()` currently selects and validates the backend, but it
  does **not** yet call the backend's `probe` or `init` ops.
- The actual seccomp probe still runs in `os_early_checks()`, and the
  dynamic arbiter still consumes `using_seccomp`.
- That means the boot-order fix is real, but full lifecycle unification
  is not done yet.
- Several comments still describe this as future work:
  - `arch/um/include/shared/backend.h`
  - `arch/um/include/asm/backend.h`
  - `arch/um/kernel/um_arch.c`
  - `arch/um/backend/{ptrace,seccomp}/lifecycle.c`

### Assessment through A-05

- Keep the early `init_backend()` placement.
- Do not redesign the contract to avoid early-boot dispatch.
- Document the current truth more sharply: the backend must be
  installed before early thread/time dispatch, even though lifecycle ops
  are not yet fully authoritative.

## 3. Are stubs for `probe/init/shutdown` and `read/write_guest_regs` acceptable?

### Ideas and thoughts

- Through A-05, yes, but only if they are described as staged
  placeholders rather than finished semantics.
- The key question is whether any current in-tree caller depends on
  their full behavior.
- Right now:
  - `probe/init/shutdown` exist in the contract and are populated.
  - KUnit verifies they are non-NULL.
  - Runtime does not yet route boot/halt logic through them.
  - `read_guest_regs` and `write_guest_regs` are explicit
    `-EOPNOTSUPP` stubs awaiting debugger work.
- That is defensible as an abstraction-first landing because it proves
  the contract shape and keeps later work from changing every call site
  again.
- It is **not** defensible if the cover letter or docs imply that
  backend lifecycle is fully centralized or that debugger support is
  already abstracted end to end.

### Assessment through A-05

- Acceptable in stages.
- Not a blocker for RFC if framed as:
  - the contract surface is frozen enough to migrate call sites now,
  - some cold ops are reserved and intentionally stubbed until later
    workstreams,
  - no current behavior depends on them being richer.
- If a maintainer objects, the most likely acceptable refinement is not
  "throw away the abstraction"; it is "make `probe` real sooner" or
  "say more clearly that these are placeholders".

## 4. Per-mm state: one `struct mm_id` with mixed ownership, or split?

### Ideas and thoughts

- The current choice is pragmatic and correct for this stage.
- A single `struct mm_id` keeps the refactor simpler:
  - no per-backend allocation path,
  - no extra failure/unwind logic,
  - no `void *backend_private`,
  - no accessor-only ceremony forced across shared code.
- The byte waste is small, and the current code still has shared helper
  paths that inspect or rely on backend-specific fields.
- A split `backend_private` design would clean up ownership, but it
  would also force accessor discipline everywhere before the codebase is
  actually ready for it.
- Today that cost would be paid immediately while much of the value
  would only appear later, mainly if KVM introduces substantially larger
  per-mm state or if ownership bugs start recurring.

### Caveats

- The ownership model is still easy to misread.
- The older notes are slightly optimistic about how much backend-local
  separation has already happened; shared host code still contains
  `using_seccomp` branches and shared helpers that know about the mixed
  layout.

### Assessment through A-05

- Keep the unified `mm_id`.
- Revisit only if one of these becomes true:
  - KVM adds materially larger per-mm state,
  - field ownership causes bugs or repeated reviewer pushback,
  - shared helpers cannot be cleaned up without an accessor layer.

## 5. Header split: is `shared/backend.h` acceptable?

### Ideas and thoughts

- Yes. It is unusual, but it solves a real UML-specific problem.
- USER-side TUs in `arch/um/os-Linux/` need:
  - the full struct definition,
  - the op prototypes,
  - the dispatch macro,
  - the `um_backend` declaration.
- A thinner adapter would likely just duplicate the same surface in a
  less honest way:
  - either mirror the struct,
  - or hide it behind wrappers and multiply the number of forwarding
    functions.
- Putting the shared contract in `arch/um/include/shared/backend.h` is
  therefore a reasonable exception to the normal "put it in asm/"
  instinct.

### Caveats

- The unusual split needs to be explained once and then left alone.
- Right now the contract doc does explain it, which is good.
- The remaining issue is consistency: some older docs still refer to the
  old `arch/um/include/asm/backend.h` location as if it contained the
  whole contract.

### Assessment through A-05

- Keep the split.
- Do not add a second adapter layer just to restore a more familiar
  header shape.
- Make the rationale explicit in the RFC so reviewers do not spend time
  rediscovering why USER TUs forced this.

## 6. `backend=auto`: keep "ptrace unless asked" or flip to "try seccomp first"?

### Ideas and thoughts

- This is mainly a product/default-policy question, not a contract-shape
  question.
- The current implementation keeps historical behavior:
  - `backend=auto` defers to the legacy seccomp selection path.
  - default DYNAMIC behavior remains effectively "ptrace unless seccomp
    was requested/probed".
- That is conservative and reviewer-friendly for an initial RFC because
  it avoids mixing architecture work with a user-visible default change.
- There is also a real case for seccomp-first:
  - it matches the long-term performance story,
  - it aligns with the earlier design direction,
  - it is closer to the "prefer better mechanism if available" model.
- But changing defaults before the abstraction is externally reviewed
  makes it harder to separate "is the interface sound?" from "do we want
  this behavior change now?"

### Assessment through A-05

- Keep legacy-compatible `backend=auto` for the first external send.
- Ask maintainers explicitly whether they want seccomp-first as a
  follow-up once the abstraction itself is accepted.
- Do not let this default-policy decision block workstream A review.

## 7. Upstream cadence: RFC now, or wait for workstream B?

### Ideas and thoughts

- RFC now.
- The branch already contains enough substance to justify
  architectural feedback:
  - the contract exists,
  - dispatch exists,
  - the arbiter exists,
  - selection invariants exist,
  - first-pass conformance exists.
- Waiting for B would make the first send larger, noisier, and harder to
  review.
- It would also mix two questions that should be separated:
  - "Is this backend abstraction the right shape?"
  - "Do the runtime gates and perf payoffs justify later work?"
- The existing upstream-strategy doc already argues for early RFC of the
  architecture before heavy code motion.

### Assessment through A-05

- Send the architecture RFC before B.
- The right order is:
  1. reconcile the docs,
  2. split the mega-commit into a small first series,
  3. send the RFC and ask the architectural questions explicitly.

## 8. Conformance test depth: is wired-only enough?

### Ideas and thoughts

- For A-05 first pass, yes.
- The current KUnit suite does something important and specific:
  - it makes missing-op regressions hard,
  - it checks single-backend dispatch wiring,
  - it gives safe functional coverage to the cold ops that can be
    exercised from a KUnit thread.
- That is exactly the right first layer for a contract migration.
- It is not the same as full behavioral equivalence.
- The hot ops are still only structurally validated at the KUnit layer
  because invoking them directly in KUnit would destabilize the running
  kernel.
- That means the next validation layer genuinely does need guest-driven
  cross-backend equivalence, but that is a **next layer**, not a reason
  to dismiss the current suite.

### Assessment through A-05

- Wired-only is enough for maintainers to review A-05 honestly.
- It is not enough to claim "the conformance suite is done" in the
  stronger sense described by the long-form contract doc.
- The right framing is:
  - A-05 delivers structural conformance and safe cold-op checks.
  - Guest-visible equivalence is the next validation layer.

## 9. Performance baseline: is a 42% boot-cycles delta meaningful?

### Ideas and thoughts

- Not yet as a headline claim.
- It is interesting as an internal signal, especially because it moves
  in the direction the architecture expects.
- But boot is a noisy compound workload:
  - host setup dominates,
  - init path details matter,
  - early variance is high,
  - the current perf notes already show noise issues.
- That means a strong "seccomp is 42% faster" claim would be easy for a
  reviewer to dismiss.
- A syscall-focused microbenchmark guest is the right next step if you
  want a publishable number:
  - tight `getpid()` loop,
  - maybe page-fault and context-switch microbenches later,
  - same harness across PTRACE_ONLY, SECCOMP_ONLY, and DYNAMIC.

### Assessment through A-05

- Treat the current perf number as exploratory only.
- Do not make it part of the architectural argument for the first RFC.
- If included at all, put it in an appendix or "early signal" note with
  explicit caveats.

## Cross-cutting design vs implementation notes

- The abstraction is structurally real now. The kernel dispatches
  through the backend table in the intended places.
- The implementation is still partially a facade over legacy shared
  helpers. `using_seccomp` remains present in shared host code and some
  backend wrappers still rely on those branches. That is acceptable at
  A-05, but it should be described as implementation debt, not as fully
  completed backend-local factoring.
- Lifecycle is the least complete part of the abstraction. The contract
  has lifecycle ops, but runtime still uses the legacy boot/halt flow
  rather than routing those semantics through the ops table.
- Validation is also intentionally partial. A-05 meaningfully improved
  the situation, but the contract doc still reads more like the desired
  end state than the currently shipped validation depth.

## Prioritized recommendations

1. Reconcile the sources of truth before any external review.
   Update `redesign/01-architecture/three-layers.md`, the stale A-01
   deliverable wording, the lifecycle comments, `asm/backend.h`,
   `um_arch.c`, and the conformance wording in
   `Documentation/virt/uml/backend-contract.rst` so they all describe
   the same A-05 system.
2. Send an RFC on workstream A now; do not wait for B.
   The abstraction is mature enough for architectural review, and
   waiting only increases patch size and review scope.
3. Freeze the 18-op surface for the RFC.
   Invite feedback on semantics and naming, but do not churn the table
   preemptively by folding or splitting ops without a concrete
   maintainer objection.
4. Keep `init_backend()` as the early boot installation point.
   The early dispatch sites justify it. The cleaner follow-up is to make
   lifecycle semantics catch up to this placement, not to reintroduce a
   bootstrap bypass around the contract.
5. Be explicit that five cold ops are staged placeholders through A-05.
   Say that plainly in docs and the cover letter. Do not imply full
   lifecycle unification or debugger support already landed.
6. Keep the unified `struct mm_id` for now.
   The memory overhead is small, and the accessor/allocation churn of a
   split design is not justified before later workstreams force it.
7. Keep `shared/backend.h` as the shared contract header.
   It is unusual but justified by USER-side TUs. Explain the exception;
   do not hide it behind a second adapter layer.
8. Keep `backend=auto` legacy-compatible for the first send.
   Raise seccomp-first as an explicit policy question to maintainers,
   not as an implicit behavior change bundled with the abstraction.
9. Describe A-05 tests as "first-pass structural conformance".
   Present the KUnit suite as valuable and real, but not as complete
   behavioral equivalence across backends.
10. Do not publish the 42% boot-cycles delta as a strong performance
    claim yet.
    Build a microbenchmark guest first if you want performance data to
    carry review weight.

## Bottom line

If the goal is to decide whether workstream A is strong enough, through
A-05, to solicit maintainer feedback, the answer is yes.

If the goal is to claim the abstraction is already fully internally
clean and fully validated, the answer is no.

The right next move is not more architectural churn. It is a careful
doc reconciliation pass, followed by an RFC that presents the current
state honestly.
