# Spike 04: long-mode guest + IO-port VMEXIT

Second KVM measurement spike. Where Spike 01 measured the
absolute floor of a VMEXIT round-trip using a real-mode
guest with a single `hlt`, this spike measures the **shape
the naive backend actually uses**: a long-mode (IA-32e, ring
0) guest with a userspace-visible VMEXIT from a
syscall-style instruction.

See `measurements.md` in the parent directory for the full
timing table across 8 hosts and the per-host cycle deltas
vs Spike 01.

## Running

```sh
make
sudo ./spike     # or just ./spike if you're in the kvm group
```

Needs `/dev/kvm` access. Users in the `kvm` group don't
need sudo; on hosts where only root can open `/dev/kvm`,
`sudo` is the fallback.

## What it builds

Same shape as Spike 01:

1. Open `/dev/kvm`, create VM, create one vCPU.
2. Allocate 2 MiB anonymous host memory, register it as
   guest physical memory at phys 0.
3. **Set up IA-32e long mode** with:
   - 3-entry GDT at guest phys 0x3000 (null / ring-0 code
     with L=1 / ring-0 data)
   - 4-level paging: PML4 → PDPT → PD, single 2 MiB huge
     page identity-mapping phys 0..2 MiB
   - CR0.PE|MP|NE|WP|PG, CR4.PAE, EFER.LME|LMA|SCE
4. Place guest code at phys 0x4000:
   ```
   out %al, $0xf4    ; 2 bytes — triggers KVM_EXIT_IO
   hlt               ; trailing exit for sanity
   ```
5. Place LSTAR trampoline at phys 0x4100 (kept for
   reference, not exercised in current spike):
   ```
   vmcall
   sysretq
   ```
6. Program LSTAR / STAR / SFMASK via `KVM_SET_MSRS`.
7. Run 1000 iterations. Each iteration:
   - reset `rip = 0x4000`
   - `rdtsc` before `KVM_RUN` #1
   - `KVM_RUN` (guest executes `out`, VMEXITs with
     `KVM_EXIT_IO`)
   - `rdtsc` after
   - mock-handle: set `rax=42`, advance `rip += 2`
   - second `KVM_RUN` (guest executes `hlt`, VMEXITs
     with `KVM_EXIT_HLT`)
8. Report p10 / median / mean / p90 cycles for both IO
   and HLT exits, with ns equivalents.

## Why `out` not `vmcall`

Original spike plan used `vmcall` to trigger VMEXIT (that's
what the design memo's bounce trampoline will use). In
practice, KVM in-kernel emulates `vmcall` via
`kvm_emulate_hypercall()` and resumes the guest without
exiting to userspace — unless a specific hypercall number
requests userspace handling via `KVM_CAP_EXIT_HYPERCALL`.

For the spike's purpose (measuring userspace-visible
VMEXIT round-trip cost), `out` to an unused port gives us
the same cost envelope with a reliably-userspace-visible
exit. `KVM_EXIT_IO` and `KVM_EXIT_HYPERCALL` are
architecturally similar: both are single-instruction
VMEXITs with serializing semantics, both route through the
same host-side `kvm_run` dispatch.

When we build the real backend, we can choose either:
- IO exit via `out` (always userspace-visible, measured here)
- `vmcall` with `KVM_CAP_EXIT_HYPERCALL` registered for our
  hypercall number space (slightly cleaner, same cost).

Either works. The design memo's bounce-trampoline section
should be updated with this note once we commit to one.

## What the spike does NOT measure

- The cost of a real syscall-via-LSTAR path. SYSCALL →
  LSTAR trampoline → vmcall is ~5-10 more cycles than a
  direct `out`; spike 04's `out` shortcut gives us the
  userspace-exit cost without the SYSCALL/SYSRET overhead.
  A follow-on spike (Spike 05) could A/B this to separate
  the two costs.
- Anything about signal / interrupt delivery.
- Page-fault path costs.
- Anything involving ring 3.
- CPU P-state stability — see `measurements.md`'s
  commentary on s0's 1.1 GHz vs 4.9 GHz run variance.

## What this spike settles

1. ✅ **Long-mode bring-up from userspace KVM is feasible.**
   A minimal 3-entry GDT + 4-level paging table + correct
   MSR programming is enough. No need for a real IDT,
   TSS, or early-boot firmware.
2. ✅ **IO-exit is a drop-in for vmcall** as the
   userspace-visible VMEXIT mechanism. Cleaner plumbing
   for the spike; the memo's shape is unchanged.
3. ✅ **Silicon-dependence of the floor holds.** The
   per-host cycle counts track Spike 01's ordering even
   with the IO-exit overhead added: Zen 4 ≈ Alder Lake
   i5 ≈ modern floor (~13k cyc); Skylake-era ≈ old floor
   (~25k cyc).
4. ⚠️ **Per-syscall performance claim needs P-state-
   locked re-measurement.** The i9's 1.1 GHz thermal run
   this session shows why: ns numbers swing wildly with
   clock, and the design memo's "~800 ns on Alder Lake
   i9" claim was measured at 4.9 GHz but may not hold
   under steady-state UML load. Follow-on: pin governor,
   measure again.

## Pitfalls for future spike authors

- `vmcall` does not exit to userspace by default. You get
  `KVM_EXIT_HYPERCALL` only if the hypercall number
  matches a registered userspace handler.
- `KVM_SET_MSRS` must happen **after** `KVM_SET_SREGS` has
  programmed EFER with LMA+LME set, or SYSCALL/SYSRET
  MSRs may be treated as invalid.
- Triple-fault symptoms (`KVM_EXIT_SHUTDOWN` reason 8) usually
  mean: bad segment descriptor (check kvm_segment fields
  match the GDT), bad page table (missing PTE_US if guest
  runs at CPL=3, bad PS flag at wrong level), or missing
  IDT with an exception raised.
- `KVM_EXIT_HLT` requires the guest CPU to be in a state
  where HLT is legal (CPL=0 with interrupts-enabled, or
  CPL=3 is #GP). We run at CPL=0 so HLT works.
