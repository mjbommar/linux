# R1: LKML acceptance — the Tazaki redux risk

## The risk

Hajime Tazaki's "Unify LKL into UML" RFC reached v8 in 2019 and
stalled. Not because the idea was wrong; because no one drove
it through 8 review cycles and the architecture story wasn't
crisp enough to compel a decision.

Our plan is bigger than Tazaki's. It can fail the same way.

## What stalled Tazaki

From reviewing the linux-um and LKML threads:

- **No clear architectural commitment.** Each rev added or
  changed structural pieces; reviewers couldn't predict where
  v9 would land.
- **No upstream maintainer champion.** Tazaki was a research
  outsider; existing UML maintainers didn't actively push it
  through.
- **Compelling-but-uncosted ambition.** Reviewers couldn't
  evaluate "is this 2 weeks of polish or 2 years?".
- **Overlap with other in-flight work.** Anton Ivanov, Johannes
  Berg, and Benjamin Berg had their own roadmaps; Tazaki's RFC
  collided with rather than complemented them.

## What our plan does differently

1. **Architecture commitment up front.** Layer 1 + Layer 2 +
   Layer 3 is the architecture; everything else is mechanism.
   We don't iterate on the architecture itself across versions.

2. **Modular landing.** Workstreams A and B can land
   independently of C and D. Each workstream's first task is a
   small, separately-mergeable change. We're not asking for
   "merge this 80-patch series"; we're asking for "merge this
   3-patch contract".

3. **Maintainer engagement at design time.** Not after.
   Workstream A's first deliverable (the ops table) gets
   reviewed by Berg, Ivanov, Bie, Weinberger BEFORE any
   implementation. If they reject the design, we restart with
   their input — not after we've written 20kLOC.

4. **Costed plan.** This document is honest about the 24-EM
   total cost. Reviewers can decide if it's worth it; they
   don't have to guess.

5. **Complement existing work.** Berg's seccomp port is the
   foundation for Backend Abstraction. Bie's SMP work is a
   precondition for KCSAN. Ivanov's vector networking is
   profile-orthogonal; we don't disturb it.

## Mitigation strategy

### Pre-RFC engagement (months 1-2)

- Email each maintainer individually with a 1-page summary.
- Ask: does this make sense? What's missing?
- Iterate the architecture document until they ack the shape.
- Only then send the first patch series.

### Incremental upstream cadence

- A-01 (ops table): 3-patch series; wide review.
- A-02 (ptrace refactor): N small series, one per op refactored.
- B (gates): each gate is its own patch.
- C (ports): each port is its own series, weeks apart.
- D (KVM): biggest single series; needs the most pre-work.

Never submit a 50-patch monster.

### Maintainer recruitment

- Identify a champion among existing UML maintainers. Berg seems
  most aligned (he did seccomp; understands the platform-
  abstraction value).
- Offer co-author or co-maintainer status if they take on a
  workstream.
- Publish progress regularly (monthly status to linux-um).

### Bail-out criteria

If after 6 months of work, no significant series has landed in
mainline, stop and reassess. Maybe the maintainers genuinely
don't want this and we're better off forking. Better to know
at 6 months than at 24.

## Indicators of trouble

Watch for:

- Multiple v3+ revisions of the same series with no maintainer
  ack
- "I don't see why we'd want this" replies that don't engage
  the value prop
- Silence on architecture-level posts (means no one cares
  enough to engage)
- Other maintainers proposing alternatives to our approach
  (fine; merge them or argue, but don't be silent)

## Indicators of success

- An UML maintainer adds `Reviewed-by` to a workstream A patch
- A workstream B gate lands in 6.NN-rc1
- syzbot adopts UML as a target after C-08 lands
- A second contributor (outside our team) submits a patch
  building on the architecture
