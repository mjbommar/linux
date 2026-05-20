# 06 — vCPU host-thread CPU pinning

**Sprint:** post-2026-05-19
**Priority:** MEDIUM
**Effort:** small (~200 LoC across `tools/uml/uml-launcher/` and
`arch/um/os-Linux/main.c`)
**Status:** investigated 2026-05-19 — design partially subsumed.
The kvm-v2 backend's per-host-CPU vCPU pool already pins each
vCPU's KVM_RUN to its own host CPU by construction: `vcpus[N]`
is *only* used from `smp_processor_id() == N`, so Phase 1's
intent ("each vCPU's KVM_RUN host thread stays on its own host
CPU") is essentially free from the pool design.  What memo 6
*additionally* wanted — pinning specific guest kernel **tasks**
to specific host CPUs — doesn't map cleanly onto UML's threading
model (UML's task threads aren't 1:1 with QEMU/KVM-style vCPU
threads; Linux schedules them across the process's CPU mask).
The realistic next step is cgroup-cpuset partitioning + NUMA
mbind, both already deferred under memo 52 §3.2 (Tier 3).
Memo 6 is therefore documented-as-superseded; Phase 2-4 wait
for a NUMA-host bench appearance.
**Depends on:** memo 52 (done — provides the
`[host_resources]` TOML and `UM_KVM_V2_CPU_AFFINITY` env-var).

## Why this matters

SMP-T13 (migrate_disable), T55 (per-vCPU FPU-dirty), T73
(YMM-upper XSAVE) closed the SMP correctness problems. SMP-T82
shipped process-level CPU affinity via
`UM_KVM_V2_CPU_AFFINITY=0-3` (memo 52 §2.2). What's still on the
table is **per-vCPU-host-thread** affinity within the process's
overall CPU set, so each vCPU's KVM_RUN host thread stays on its
own host CPU.

Memo 52 §3.2 deferred NUMA `mbind` as Tier 3. This memo is the
intermediate step: thread-level affinity that doesn't require
NUMA topology awareness. On a single-socket host it consolidates
the per-host-CPU vCPU pool's design intent (vCPU N runs on host
CPU N); on a multi-socket host it sets up the pre-condition for
the NUMA work.

## Current state

| Surface | Path | State |
|---------|------|-------|
| Process-level affinity | `arch/um/os-Linux/main.c` SMP-T82 block | active when `UM_KVM_V2_CPU_AFFINITY` is set |
| Per-vCPU host thread | `arch/um/backend/kvm-v2/vcpu.c::kvm_v2_vcpu_create_one` | spawns; no `sched_setaffinity` per thread |
| `umlctl` `[host_resources]` schema | `tools/uml/uml-launcher/src/bin/umlctl/deploy.rs::HostResourcesSection` | has `cpu_affinity` field (process-level only) |
| Memo 52 §3.2 NUMA `mbind` | deferred | Tier 3 |

Per memo 52 the host-thread / per-vCPU pinning was explicitly
deferred to "when a NUMA host appears in the bench matrix." This
memo opens that door without committing to full NUMA awareness.

## Proposed change

### Phase 1 — `[host_resources].vcpu_thread_affinity` field

```toml
[host_resources]
cpu_affinity = "0-7"             # process-level (existing, SMP-T82)
vcpu_thread_affinity = "auto"    # NEW: "auto" / "off" / explicit list
```

Semantics:

- `"auto"` (default): each vCPU host thread pinned to a single
  host CPU within the parent's `cpu_affinity` mask. vCPU N → host
  CPU `cpu_set[N % len(cpu_set)]`.
- `"off"`: no thread-level pinning; threads inherit the parent's
  process mask only (current behaviour).
- explicit list: per-vCPU override
  (e.g. `"0,1,4,5"` for ncpus=4 maps vCPU 0/1/2/3 → host
  CPUs 0/1/4/5).

### Phase 2 — Kernel-side enforcement

```c
/* arch/um/backend/kvm-v2/vcpu.c::kvm_v2_vcpu_create_one */
static int kvm_v2_vcpu_create_one(struct kvm_v2_vm *vm, int cpu,
                                  int mmap_size)
{
    ...
    /* SMP-T96 (memo 06 of post-2026-05-19 sprint): per-vCPU
     * host-thread affinity. Read UM_KVM_V2_VCPU_AFFINITY from env,
     * compute the per-cpu mask, sched_setaffinity at the host
     * thread that will own this vCPU's KVM_RUN. */
    if (vcpu_affinity_policy != OFF) {
        cpu_set_t mask;
        compute_per_vcpu_mask(cpu, &mask);
        sched_setaffinity(host_thread_tid, sizeof(mask), &mask);
    }
    ...
}
```

The translation umlctl → env var follows the same pattern as
memo 52's other knobs: TOML `vcpu_thread_affinity` →
`UM_KVM_V2_VCPU_AFFINITY` env → kernel-side parser.

### Phase 3 — `umlctl mission` Phase 4 verification

The mission gate's Phase 4 (host_resources verification) checks
that the env var was applied. Add a check that on a booted UML
with `vcpu_thread_affinity = "auto"`, `cat
/proc/$pid/task/$tid/status | grep Cpus_allowed_list` for each
KVM_RUN thread shows a single-CPU mask, not the full process
mask.

### Phase 4 — NUMA-awareness preflight (optional)

If `numa_available()` returns 0 (NUMA host detected) and
`vcpu_thread_affinity = "auto"`, the preflight emits a hint
suggesting also setting `[host_resources].numa_node = "0"` (or
similar) — the Tier 3 NUMA work (memo 52 §3.2, SMP-T86)
remains opt-in.

## Effort breakdown

- Phase 1 (TOML schema + env-var translation): ~50 LoC across
  `deploy.rs` + manifest writer.
- Phase 2 (kernel-side per-vCPU `sched_setaffinity`): ~80 LoC
  in `arch/um/backend/kvm-v2/vcpu.c` + `os-Linux/main.c`.
- Phase 3 (mission Phase 4 verification): ~40 LoC.
- Phase 4 (NUMA preflight hint): ~30 LoC.

Total: ~200 LoC.

## Acceptance criteria

- **Functional gate:** `[host_resources].vcpu_thread_affinity =
  "auto"` boots cleanly; `/proc/$pid/task/$tid/status` shows
  per-thread `Cpus_allowed_list` matching the policy.
- **Mission gate:** Phase 4 verification adds the per-thread
  affinity check.
- **No regression gate:** `umlctl mission --quick` with
  `vcpu_thread_affinity = "off"` produces the same per-phase
  output as today.
- **Bench gate:** on a host with ≥ 8 CPUs, the snapshot bench
  median restore latency improves by ≥ 5% with
  `vcpu_thread_affinity = "auto"` (less inter-CPU bouncing on
  KVM_RUN re-entry).

## Dependencies

- **Memo 52:** provides the env-var-translation infrastructure
  and the existing `cpu_affinity` field. This memo is the
  natural extension.
- **No memo dependency in this sprint:** can land any time
  after memo 52; doesn't block other sprint memos.

## Risk notes

- **Pinning fights cgroup cpuset.** If `[host_resources]` also
  sets `cpu_max = "200%"`, the cgroup imposes a CPU budget but
  doesn't pick which CPUs. Per-thread affinity inside a
  cgroup-restricted set works as long as the affinity mask is
  a subset of the cgroup's allowed CPUs. Mitigation: preflight
  check that `cpu_affinity ⊆ /sys/fs/cgroup/<...>/cpuset.cpus.
  effective` (when cgroup is active).
- **Conservative default.** "auto" is the default for new
  configs; existing configs with no `[host_resources]` block
  retain the current (no-pinning) behaviour. No silent
  behaviour change for unconfigured users.
- **NUMA misconfiguration.** A user that sets
  `vcpu_thread_affinity = "0-3"` on a NUMA host where CPUs 0-3
  span sockets gets cross-socket cache traffic. Mitigation:
  the preflight hint in Phase 4 surfaces the NUMA detection.
- **Rollback:** revert is a single TOML field flip to
  `vcpu_thread_affinity = "off"` (or unset).

## Cross-references

- Memo 52 §2.2 (SMP-T82 process-level affinity): the
  predecessor.
- Memo 52 §3.2 (Tier 3 NUMA `mbind`): the successor (still
  deferred).
- `tools/uml/uml-launcher/src/bin/umlctl/mission.rs` Phase 4:
  the consumer that verifies the knob took effect.
