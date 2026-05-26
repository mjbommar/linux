# Tier 3 design memo landed + Series 1 re-verified (2026-05-14)

## Tier 3 design (Track A §3.2)

Sub-agent produced `phase-J-tier3-design-2026-05-14.md` (610 lines)
resolving open question #2 from `phase-J-design-2026-05-07.md` §3.3
(TAP IP discovery).

**Chosen option:** (b) fixed per-worker IP allocation policy.

Rationale: option (a) (extend `umlctl ps --json` with guest IP)
requires Rust changes across `manifest.rs`, `deploy.rs::cmd_up`,
and `registry.rs` — manifest schema is intentionally immutable
post-`create` today. Option (b) is ~10 lines in
`run-soak-daemon.sh` plus 4 `{{…}}` placeholders in the Tier 3
templates; zero Rust changes.

IP allocation: `192.168.42.0/24` carved into per-worker /30s.
Worker N → host `.4N+1`, guest `.4N+2`, tap `soak-tap<N>`.

Gotchas the sub-agent flagged for the implementer:
  1. `gate_loop.rs:253-277` only rewrites `instance.name` per
     worker — its `--workers N` fans out colliding network
     sections. Tier 3 sidesteps by spawning N `--workers 1`
     invocations from the daemon.
  2. The upstream Phase J memo cites `backend/net.rs` for TAP
     provisioning, but that file is just the vhost-user data
     path; the real compiler is
     `src/bin/umlctl/deploy.rs::compile()`. Filed for
     SUBMISSION-QUEUE follow-up.
  3. Tier 3 needs `CONFIG_UML_NET_VECTOR=y` pre-flight or the
     failure manifests as an in-guest "vec0: device not found".

Implementation is bash-only in the soak rig. Templates:
`tier3-django.toml.template` + `tier3-fastapi.toml.template`.

## Series 1 (bpf-hygiene-v1) re-verification (Track D §6.1)

Re-ran the SUBMISSION-NOTES build verification on a fresh
defconfig + BPF_SYSCALL+BPF_JIT build:

  - `master` baseline: `bpf_jit_comp.o` = 65 656 bytes.
  - `origin/bpf-hygiene-v1-submit`: `bpf_jit_comp.o` =
    65 656 bytes (byte-identical).
  - checkpatch on `git format-patch -2 …`: patch 0001 clean;
    patch 0002 has 1 false-positive warning (75-char max-line
    on the subject — LKML-acceptable).
  - Submission branch `origin/bpf-hygiene-v1-submit` is reachable
    and based off master (no rebase drift).

**Verdict:** Series 1 is **STILL READY** for `git send-email`.
Operator action only — see `upstream-patches/bpf-hygiene-v1/
SUBMISSION-NOTES.md` for the send-email recipe.

## What's still in flight

Sub-agents running:
  - LTP curation walk (Track A §3.3). Wakeup at ~22:06 UTC.
  - #168 snapshot port Phase 1 (Track B §4.1). Building +
    KUnit + memo. Larger scope; may take 30-60 min wall.

## Next

When LTP and snapshot sub-agents return, commit + push them.
Then continue with:
  - Series 2 (kmsan-arch-callback-rfc) readiness re-check.
  - Series 3 (ftrace-notrace-generic-v1) readiness re-check.
  - Series 7 (kvm-backend) cover-letter packaging draft (PLAN
    §6.3 — gated on Phase J DONE for final send but staging
    early is recommended).
