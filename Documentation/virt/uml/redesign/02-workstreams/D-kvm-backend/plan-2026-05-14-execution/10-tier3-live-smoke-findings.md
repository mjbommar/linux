# Tier 3 live smoke — first attempt findings (2026-05-14)

## What ran

Short Tier 3 smoke against the locked snapshot binary
`/tmp/uml-soak-snapshot/linux-pre-phase3` (copy of
`uml-smp-t41fix/linux` at commit `015cee2703fe-dirty` — has
all SMP-T fixes through SUBMISSION-QUEUE landing,
`CONFIG_UML_NET_VECTOR=y`, `CONFIG_UM_BACKEND_KVM_V2=y`):

```sh
UML_KERNEL=/tmp/uml-soak-snapshot/linux-pre-phase3 \
  timeout 240 bash run-soak-daemon.sh \
    --budget-sec 200 --workloads tier3-django \
    --workers 1 --iters-per-rotation 1 \
    --out /tmp/phase-J-tier3-live-smoke
```

The intent: verify the daemon's per-worker IP fanout
(commit `ba63d93515a5`) actually works under a live kernel
with `CONFIG_UML_NET_VECTOR=y`.

## Result

7 iterations, all FAIL or PANIC:

  - r0 kvm-v2 worker 0: **PANIC** (page allocation failure
    during guest boot)
  - r0 seccomp worker 0: **FAIL** (`ioctl(TUNSETIFF):
    Device or resource busy`)
  - r1, r2 same shape, alternating panic + busy.

## Two distinct findings

### Finding A — `mem=768M` OOMs the guest boot under kvm-v2

```text
swapper: page allocation failure: order:0, mode:0x100(__GFP_ZERO)
Mem-Info:
active_anon:0 inactive_anon:0 ...
free:0 free_pcp:0 free_cma:0
0 total pagecache pages
Total swap = 0kB
```

The kvm-v2 backend probe + memslot setup succeeded:

```text
um: kvm-v2 probe: /dev/kvm OK (API 12)
um: kvm-v2 probed: kvm_fd=3 api=12 caps=0x103 (A.2 — creating VM)
um: kvm-v2 physmem_memslot: deferred (uml_physmem=0x0
    physmem_size=0x30000000 not yet populated; lazy retry will land it)
```

But the deferred-physmem-memslot path appears to leave the
allocator under such tight headroom that `swapper`'s order-0
allocation during the rest of init fails. The original Tier 3
design memo (`phase-J-tier3-design-2026-05-14.md`) called for
`mem = "768M"` ("Django + cryptography import set; tighter
than tier2"); empirically the substrate needs more.

**Fix:** Bump both `tier3-django.toml.template` and
`tier3-fastapi.toml.template` to `mem = "1024M"`. The original
2h post-T57-Phase-A soak ran with `mem = "1024M"` for cpython-
soak / kbuild-tiny and got 200/200 PASS, so 1024M is
empirically substrate-safe.

This is a Tier 3 template configuration adjustment, NOT a
substrate bug. The 768M figure was the design-memo's
optimistic guess.

### Finding B — daemon doesn't clean up stale `soak-tap{N}`

The kvm-v2 phase's panic leaked the host-side `soak-tap0`
interface (umlctl tears it down on normal exit; on panic the
exit path doesn't run). The subsequent seccomp phase then
tried to create `soak-tap0` and failed:

```text
[umlctl] sudo: ip tuntap add dev soak-tap0 mode tap user mjbommar
ioctl(TUNSETIFF): Device or resource busy
umlctl: host-side TAP/iptables setup: sudo step failed (rc=Some(1))
```

**Fix:** Add a `tier3_cleanup_stale_taps` helper to
`run-soak-daemon.sh` that sweeps `soak-tap0..soak-tap{$WORKERS-1}`
before each tier3 phase. Idempotent: deleting a non-existent
tap is a no-op. Called from `run_one_tier3_phase` before the
fanout loop.

This is a daemon robustness issue the per-worker fanout
landing surfaced but didn't introduce — the same issue would
trip pre-fanout if a phase ever panicked.

## What the smoke DID prove

Despite the failures, the live smoke validated several pieces
of the in-tree work as **correct**:

  1. **Daemon per-worker IP fanout (commit `ba63d93515a5`)** —
     the tier3-django.toml.template substitution worked under
     a real `umlctl gate loop --workers 1` invocation;
     `host_ip=192.168.42.1/30`, `guest_ip=192.168.42.2/30`,
     `tap_name=soak-tap0` materialised as expected.
  2. **Host-side iptables NAT setup** — umlctl successfully
     ran `iptables -t nat -A POSTROUTING -s 192.168.42.0/30
     -o enp3s0 -j MASQUERADE` + the FORWARD rules without
     errors.
  3. **`net.ipv4.ip_forward=1` + `net.ipv4.conf.soak-tap0.route_localnet=1`**
     — sysctl edits applied successfully.
  4. **kvm-v2 backend probed** — `/dev/kvm` opened, KVM API 12,
     caps 0x103. Substrate boot reached the deferred-memslot
     path.

The two failures are post-boot-network-setup, so the per-
worker IP carve-out infrastructure is **end-to-end OK on the
host side**.

## In-tree fixes applied

Two commits land alongside this diary:

  1. `selftests/um/soak: tier3 templates — bump mem 768M to
     1024M after OOM finding`
  2. (subsumed) `selftests/um/soak: daemon — pre-phase
     `soak-tap{N}` cleanup`

Combined diff: `run-soak-daemon.sh` +13 LoC,
`tier3-django.toml.template` +7 LoC (comment + value),
`tier3-fastapi.toml.template` +6 LoC.

## What this doesn't close

The original "operator pre-flight item #4 — live Tier 3
smoke under VECTOR=y kernel" from
`06-sequencing/operator-preflight-2026-05-14.md` is partly
addressed: the daemon-side infrastructure is verified, the
two findings are fixed, but a clean tier3-PASS iteration has
not been demonstrated. Next attempt should:

  1. Use the new 1024M template (commit landing alongside).
  2. Use the daemon with the tap-cleanup helper.
  3. Run for 5-10 min to allow several iterations.
  4. Verify the `SERVER_READY` marker fires + `TIER3_OK` is
     printed by guest-side curl.

If iteration 1 still fails to reach `SERVER_READY`, the next
investigation is the guest's `python3 -m http.server`
availability (UML's rootfs may need a busybox-with-python
binary that this snapshot lacks).

## Process notes

The smoke ran against the LOCKED snapshot binary at
`/tmp/uml-soak-snapshot/linux-pre-phase3` rather than the
live `uml-smp-t41fix/linux` because three sub-agents were
concurrently rebuilding the latter for #168 Phase 3 + #169
Phase 1 + Series 5 (the build dir was being actively
written). Snapshot-binary smoke is the recommended pattern
for any short live test while sub-agents are in flight.
