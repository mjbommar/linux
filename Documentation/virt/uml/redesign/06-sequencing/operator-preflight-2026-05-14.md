# Operator pre-flight checklist — 2026-05-14 landings

**Companion to:** `PLAN-2026-05-14.md` (the strategic plan),
`STATUS.md` (current state), `04-risks/decisions-log.md` D124-D130
(the day's landings).

This file collapses all operator-time work the 2026-05-14
execution session unblocked into a single ordered checklist. Each
item lists: what to run, what it gates, and the diary entry / memo
that explains it.

## Background

The 2026-05-14 in-tree session pushed scaffolding + design memos
through to a state where the remaining bottleneck is operator
wall-clock + LKML cadence, not engineering. Three sub-agents are
still in flight on top of this checklist (#168 Phase 3, T55 bisect,
Series 5 draft) — when they land, this file should be re-read for
fresh items.

## Phase J Track A — unblock Tier 3 + LTP + 24h soak

### 1. CONFIG_UML_NET_VECTOR=y kernel rebuild [~3 min]

```sh
cd ~/src/uml-builds/uml-smp-t41fix
./scripts/config --enable CONFIG_UML_NET_VECTOR
make ARCH=um O=$(pwd) olddefconfig -j$(nproc)
make ARCH=um O=$(pwd) -j$(nproc)
grep CONFIG_UML_NET_VECTOR=y .config  # verify
```

**Gates:** Tier 3 templates + daemon's per-worker IP fanout
(commit `ba63d93515a5`).
**Diary:** `D-kvm-backend/plan-2026-05-14-execution/05-tier3-ltp-scaffolding.md`,
`06-daemon-tier3-fanout.md`.

### 2. LTP + kirk install [~10 min, ~500 MB disk]

```sh
git clone --depth 1 https://github.com/linux-test-project/ltp /opt/ltp-src
cd /opt/ltp-src && make autotools && ./configure
make -j$(nproc) && sudo make install   # → /opt/ltp
git clone --depth 1 https://github.com/linux-test-project/kirk /opt/kirk
```

The skip-list seed is now in-tree at
`tools/testing/selftests/um/soak/ltp-skip.txt` (112 entries —
commit `015cee2703fe`). The template's `--skip-file
{{SOAK_DIR}}/ltp-skip.txt` resolves to it automatically once the
soak dir is hostfs-visible from the guest.

**Gates:** LTP rotation in the 24h soak.
**Memo:** `D-kvm-backend/phase-J-ltp-curation-2026-05-14.md`.

### 3. (Optional) Python frameworks for richer Tier 3 [~30 s]

```sh
sudo apt-get install python3-django python3-fastapi python3-uvicorn
```

Templates currently use a Python stdlib `http.server` shim that
exercises the same UML virtio-net + UML net stack path. With the
real frameworks installed, swap the relevant template's phase-2
`cmd` to `python3 -m django runserver 0.0.0.0:8080` or
`python3 -m uvicorn ...`.

**Gates:** "real" Django/FastAPI shape under Tier 3 (the
templates already work with the stdlib shim).

### 4. Tier 3 + LTP live smoke (1 short cycle each)

```sh
UML_KERNEL=~/src/uml-builds/uml-smp-t41fix/linux \
  bash tools/testing/selftests/um/soak/run-soak-daemon.sh \
    --budget-sec 600 --workloads tier3-django,tier3-fastapi,ltp-runner \
    --workers 2 --iters-per-rotation 1
```

**Verifies:** the per-worker IP carve-out actually wires up under
the rebuilt kernel; kirk reports JSON; the daemon's verdict
classifier translates `LTP_OK`/`LTP_FAIL` correctly. Expected:
PASS on all three for at least 1 cycle.

If any FAIL, the diary entry to consult depends on the workload:
  - Tier 3: `phase-J-tier3-design-2026-05-14.md`.
  - LTP: `phase-J-ltp-curation-2026-05-14.md` §5 (verdict bridge).

### 5. 24h continuous soak

**IMPORTANT — Tier 3 vector_net_open panic (added 2026-05-14
post-live-smoke):** the first Tier 3 live smoke surfaced a
kernel-mode NULL deref in `vector_net_open+0x3a3` during the
guest's `ip link set <iface> up` step (see
`02-workstreams/D-kvm-backend/plan-2026-05-14-execution/
10-tier3-live-smoke-findings.md` §"Update — retry under
mem=1024M + tap-cleanup"). Until that's fixed, EXCLUDE
`tier3-django,tier3-fastapi` from the 24h `--workloads` list.

```sh
UML_KERNEL=~/src/uml-builds/uml-smp-t41fix/linux \
  bash tools/testing/selftests/um/soak/run-soak-daemon.sh \
    --budget-sec 86400 \
    --workloads memcheck,iocheck,stress-ng,cpython-soak,kbuild-tiny,tier1-pylibs,tier2-uv-pylibs,ltp-runner \
    --workers 2 --iters-per-rotation 10 \
    --fail-threshold-window 100
```

(Note: tier3-django + tier3-fastapi REMOVED pending the
vector_net_open fix.)

**Acceptance:** ≥99.5% PASS per (workload, backend) tuple; no
`THRESHOLD_TRIPPED` early-stop; 0 panics.

**This unblocks:** Phase J DONE certificate (the gate for Series 7
in Track D). Note: PLAN-2026-05-14 §3.5 explicitly lists Tier
3 as part of the "DONE definition" — operator should decide
whether to fix vector_net_open first (gating Phase J DONE) or
accept an 8-workload soak as sufficient evidence (re-scoping
PLAN §3.5 with a follow-up).

**Wall-clock:** 24h. Operator-time. Check in at the 12h and 24h
marks.

## Phase J Track D — send the ready upstream series

The patches below are all checkpatch-clean and apply cleanly
against current `origin/master` (verified 2026-05-14).

### 6. Send Series 1 (bpf-hygiene-v1) [~5 min author-time]

```sh
cd Documentation/virt/uml/redesign/upstream-patches/bpf-hygiene-v1/
git send-email --to "linux-bpf@vger.kernel.org" \
  --cc "linux-kernel@vger.kernel.org" \
  --cc "Alexei Starovoitov <ast@kernel.org>" \
  --cc "Daniel Borkmann <daniel@iogearbox.net>" \
  0000-*.patch 0001-*.patch 0002-*.patch
```

**Maintainer routing:** see `bpf-hygiene-v1/SUBMISSION-NOTES.md`
§"Maintainer routing".

### 7. Send Series 2 (kmsan-arch-callback-rfc) [~5 min]

Same shape, target `linux-mm@kvack.org` + KMSAN maintainers. See
`kmsan-arch-callback-rfc/SUBMISSION-NOTES.md`.

### 8. Send Series 3 (ftrace-notrace-generic-v1) [~5 min]

Target `linux-trace-kernel@vger.kernel.org`. Primary Cc: Steven
Rostedt + Masami Hiramatsu. See
`ftrace-notrace-generic-v1/SUBMISSION-NOTES.md` §"Maintainer
routing".

Series 1 / 2 / 3 have no dependencies on each other; can send in
parallel. Build maintainer credibility for the larger series.

### 9. Series 4 drafted (DO NOT SEND yet)

`backend-ops-abstraction-rfc/` cover letter + SUBMISSION-NOTES
landed `85d7b3c7e35a`. Still needs:

  - The on-branch A-workstream commits rebased into the 12-patch
    sequence the cover letter promises (see SUBMISSION-NOTES
    §"Pre-submission cleanups" + §"On-branch → 0001-NN slot map").
  - Series 3 to LAND on the upstream tree before Series 4 sends
    (queue ordering, not engineering dependency).

### 10. Series 7 cover letter rewritten (DO NOT SEND yet)

`kvm-backend-series/0000-cover-letter.patch.md` rewritten for v2
architecture `aaaa3ce70027`. BLOCKED by Phase J DONE certificate
(item #5 above).

## Phase J Track B — time-machine

### 11. Snapshot Phase 3 sub-agent in flight

When the Phase 3 sub-agent returns (#27 in task tree), Phase 4
(cross-task semantics) becomes the next bite-sized step. Phase 4
needs no operator-time work — pure kernel C.

### 12. Record/replay Phase 1 (#169) — ready to start

`27-record-replay-v2-port.md` §Phase 1 recommends `arch/um/backend/kvm-v2/record.c`
(~350 LoC) — state-machine only, no live vCPU dependency. Once
Phase 3 lands, this can be the next sub-agent (no conflict with
Phase 4 which touches snapshot.c).

## Phase J Track C — polish

### 13. T55 UP-hop bisect sub-agent in flight (#14)

When it returns, the offending commit (if reproducible) gets a
state-audit memo + a fix plan. Not on critical path.

### 14. T57 Phase B — AVX-512 enable

Deferred until Phase 3 + Phase 4 of #168 land (avoids kvm-v2
Makefile contention).

### 15. #17 numpy investigation

Memo at `D-kvm-backend/state-audit/numpy-import-after-cryptography-2026-05-14.md`.
Operator-time closure path:
  1. `uv pip install --python ~/.cache/uml-soak-tier2-venv/bin/python cryptography`
  2. Extend `tier2-uv-smoketest.py` with cryptography → numpy chain.
  3. Soak for 1 cycle. If PASS, close by-construction.

## Cross-cutting

### 16. Cross-host bench re-run on Phase J host

The cover letter (Series 7) carries placeholder perf numbers. A
new bench run on the Phase J host produces the "real" cover-letter
numbers. Not gating Phase J DONE but improves Series 7 cover
quality.

### 17. Decisions-log + STATUS + PLAN are current

D124-D130 catch up to 2026-05-14. STATUS.md last-updated reflects
today's pass. PLAN-2026-05-14.md §1.3 (executive summary) reflects
the four-track state.

Re-read this checklist after the next sub-agent batch returns
(Series 5, T55 bisect, snapshot Phase 3). New items will likely
fall out.
