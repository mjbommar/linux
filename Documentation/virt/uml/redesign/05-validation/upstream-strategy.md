# Upstream strategy

How patches land in mainline without becoming Tazaki v9.

## The mainline calculus

Linux mainline merges patches via subsystem maintainer trees.
For UML, the relevant trees are:

- Richard Weinberger's tree (nominal maintainer)
- Johannes Berg's wireless/UML tree (active driver)
- Whatever Anton Ivanov uses (vector networking)
- Tiwei Bie's SMP work landing path

Patches get into mainline when a maintainer applies them.
Maintainers apply patches when:

1. The change is well-justified (commit message explains *why*)
2. The code is correct (review by other developers)
3. It's small enough to evaluate (<200 lines is easy; >2000
   lines is hard)
4. It doesn't conflict with their other priorities
5. They have bandwidth

## Our strategy

### Make every series small

No 50-patch monsters. Workstream A's first deliverable is
3-5 patches: the ops table header, one trivial ptrace op
migration, and the conformance-suite skeleton. That's it.

Subsequent series each move 1-3 ops. Each is independently
justifiable and reviewable.

### Justify the architecture *before* sending code

Pre-send a `[RFC]` of the architecture overview to linux-um
+ a couple of LWN tags. Address feedback in the *architecture
document*, not in patch comments. Once the architecture has
"this makes sense" responses from at least two maintainers,
start sending code.

### Match maintainer priorities

- Berg: seccomp, virtio-uml, time-travel. Workstream A-03
  (seccomp wrap) is a natural collaboration.
- Ivanov: vector networking. Don't disturb; profile work
  doesn't touch.
- Bie: SMP. KCSAN port (C-03) is downstream of his SMP; offer
  to coordinate.
- Weinberger: nominal maintainer; he ack's the strategy.

### Co-author when offered

If a maintainer wants to be co-author or collaborator on a
workstream, accept gladly. Co-authored work lands faster.

### Land via -next

Most series go through linux-next via the maintainer's tree
before mainline. Patience: a series posted in week N typically
appears in linux-next around week N+2, mainline around week
N+8.

### Public progress reports

Monthly status to linux-um with:
- Recent merges
- Currently-pending series
- What's coming next month
- Any open architecture questions

This keeps maintainers and reviewers oriented; they don't have
to dig through patchwork to know where things stand.

### LWN articles at milestones

Major milestones (M1, M3, M5, M7, M11, M12) get an LWN article
or guest post. Builds external visibility; pulls reviewers in;
increases chance of additional contributors.

## Cadence

- **Weekly**: workstream sync, decisions log updates
- **Bi-weekly**: send next patch series (one per active
  workstream)
- **Monthly**: status post to linux-um
- **Quarterly**: LWN article + linux-next checkpoint
- **Milestone-driven**: blog/conference talk

## What to do when a series sits

Series posted, no review for 2 weeks:
- Ping the list politely once.

4 weeks no review:
- Ping the maintainer privately.
- Re-evaluate: is the series too large? too unclear? in
  conflict with active work?

8 weeks no movement:
- Probably the design is wrong or the timing is wrong.
- Reassess in workstream review.

## What to do when feedback is harsh

Some maintainer feedback is harsh by tradition. Don't take it
personally; engage with the technical substance.

If feedback is "I don't see why we'd want this" — that's a
sign we haven't sold the architecture. Respond by linking to
the vision/architecture docs, asking specific questions about
the gap.

If feedback is "this is wrong because X" — verify X is right;
fix or push back with rationale.

If feedback is "redo with different approach Y" — evaluate Y
seriously. Sometimes the maintainer's approach is better.
Sometimes they don't see all the constraints; in that case,
explain.

## The bail-out criterion (revisited from R1)

If after 6 months of work, no significant series has landed in
mainline:

1. Stop and reassess. Maybe the maintainers genuinely don't
   want this.
2. Options:
   a. Continue out-of-tree (downstream patchset; carry forever)
   b. Fork (`uml-next` or similar; risky)
   c. Pivot the work toward what maintainers DO want

Better to know at month 6 than month 24.

## Indicators of upstream success

- A maintainer adds `Reviewed-by` to a workstream A patch
- A second maintainer picks up a workstream task
- An external contributor submits a patch on the architecture
- syzbot adopts UML as a target
- LWN publishes a positive feature article
- A maintainer asks "when's the next series?"
