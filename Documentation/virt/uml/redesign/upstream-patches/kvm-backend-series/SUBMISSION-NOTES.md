# Series 7 (KVM backend) — submission-readiness notes

**Status:** SCOPED. Patches not yet rebased. Dependent on
Series 4 (backend-ops-abstraction-rfc) landing upstream first.

**Authoring branch:** `uml-redesign-plan` on this tree, 69 commits
touching `arch/um/backend/kvm/` + related paths. See
`git log master..HEAD -- arch/um/backend/kvm/`.

**Target squash:** ~15 upstream-shaped patches. The fork's commit
granularity is optimized for review during the redesign —
per-audit-finding fixes, per-G-ladder step, per-memo-decision.
Upstream wants the final coherent shape, not the walkthrough.

## Companion docs

- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/`
  — full D-workstream memo tree.
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/measurements.md`
  §"2026-04-24 G8" — fleet bench that landed D70 = GO.
- `Documentation/virt/uml/redesign/04-risks/decisions-log.md`
  D66 (shadow-PT encoding) / D67 (syscall classification) /
  D68 retracted / D69 (memo 11 GO) / D70 (gadget go/no-go +
  audit F2 fix entries) / D71-D86 (per-G-step + audit
  closures).
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/11-systrap-gadget.md`
  — systrap gadget design spec (G1-G8 ladder, safety
  discipline, upstream known limitations).
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/10-syscall-classification.md`
  — exhaustive A/B/C/D/E classification with the inventory TSV
  as authoritative source.

## Planned patch ordering (~15 patches)

This is a squashed + reorganized view of the 69-commit fork
history. Each bullet below becomes one upstream-shape patch.
Listed in send-order.

### Foundation (patches 1-3) — backend shell + Kconfig

1. **`um: kvm: Kconfig + shell lifecycle ops`**
   - `CONFIG_UM_BACKEND_KVM` + `CONFIG_UM_BACKEND_KVM_INTEGRATED`
     + `CONFIG_UM_BACKEND_KVM_GADGET` Kconfig entries.
   - `arch/um/backend/kvm/lifecycle.c` with probe / init /
     shutdown / mm_attach / mm_detach stubs wired into the
     A-series ops table.
   - `struct kvm_um` per-process context.
   - Originates: early D-01 / D-02 ops stubs (commits
     scattered across the branch's 2026-03 / 2026-04 range).

2. **`um: kvm: memslot registration + basic KVM_RUN loop`**
   - `kvm_ensure_memslot()` — identity memslot covering UML's
     physmem.
   - `kvm_run_userspace` skeleton that just does `KVM_RUN`
     and reports the first exit reason via panic (placeholder
     dispatcher; filled in by patches 4-8 below).
   - Originates: memo 08 sub-commit #0 + #1 (early
     "boot-to-VMEXIT" work).

3. **`um: kvm: vcpu0 creation + kvm_regs ↔ uml_pt_regs marshalling`**
   - Helpers `kvm_uml_regs_to_kvm_regs` +
     `kvm_kvm_regs_to_uml_regs` for GP-reg round-trip.
   - KUnit round-trip test via `kvm_enter_guest_probe` /
     `kvm_exit_guest_probe` hooks.
   - Originates: memo 08 sub-commit #2b.

### Shadow page table (patches 4-6)

4. **`um: kvm: shadow PGD allocator + x86 PTE encoding`**
   - `kvm_shadow_pgd_alloc` + `kvm_shadow_pgd_free` with
     lazy allocation.
   - `KVM_X86_PTE_*` bit constants (`kvm_backend.h`).
   - `kvm_um_pte_to_x86()` translator that explicitly
     references UML's software-only PTE bits by their
     absolute values (per D66 finding: UML's PTE bits do
     not match hardware x86 positions, unlike the eagerly-
     copied naïve path).
   - Originates: memo 09 step 1 + D66 analysis.

5. **`um: kvm: eager shadow fill + lazy #PF-on-demand path`**
   - `kvm_shadow_fill_from_uml_pgd` 4-level walk.
   - `kvm_shadow_map_page` 4-level install.
   - `kvm_shadow_invalidate_va_range` (audit F6 fix baked in).
   - `#PF` handler in the bootstrap page + host-side
     recovery via `copy_from_user/to_user`.
   - Originates: memo 09 step 2 + step 3 + audit F6 (D82).

6. **`um: kvm: mm_map / mm_unmap wire the shadow PT`**
   - Backend `mm_map` / `mm_unmap` ops call
     `kvm_shadow_invalidate_va_range` after host-side
     os_{map,unmap}_memory succeeds. Shadow-dirty flag
     tracked on struct kvm_um.
   - Drops the write-back in `kvm_touch_all_user_vmas` per
     audit F9.
   - Originates: memo 09 step 3 + audit F6 (D82) + F9 (D85).

### Bootstrap page + entry (patches 7-9)

7. **`um: kvm: bootstrap page (GDT + LSTAR + SYSRET gadget)`**
   - One-page bootstrap allocator with GDT at +0x000,
     LSTAR trampoline at +0x040, first-entry SYSRETQ gadget
     at +0x4b0.
   - `kvm_bootstrap_copy_lstar` test hook + KUnit byte-match
     of the trampoline wire form.
   - Originates: memo 08 sub-commits #1 + #2 + #5a.

8. **`um: kvm: IDT + TSS + #PF handler in bootstrap`**
   - IDT[14] routes #PF through a ring-0 handler that emits
     `out %al, $0xfb` → host-side shadow refill.
   - TSS + IST stack setup for the ring-0 handler.
   - US bit dropped on bootstrap page PTE per audit F5.
   - Originates: memo 08 sub-commit #5b + audit F5 (D81).

9. **`um: kvm: kvm_enter_guest (SREGS + MSRs + SYSRETQ bootstrap)`**
   - SREGS programming (long mode, CR3 = shadow PGD, TR /
     LDT / IDT / GDT baselines).
   - MSR programming (STAR / LSTAR / FMASK; FS_BASE / GS_BASE
     propagation).
   - First-entry uses the SYSRET gadget to transition
     into ring-3 at user RIP.
   - `kvm_build_sysret_r11()` helper preserves user RFLAGS
     across SYSRETQ (audit F2).
   - Originates: memo 08 sub-commits #3 + #5c + audit F2
     (D75).

### Dispatcher (patches 10-11)

10. **`um: kvm: run_userspace dispatcher — per-trap interrupt_end`**
    - KVM_EXIT_IO decode for SYSCALL (0xf4), #PF (0xfb),
      MMIO, HLT, INTR, SYSRETQ (0xf5 — panic, unexpected).
    - Per-trap exit model: one KVM_RUN, one dispatch,
      interrupt_end(), then return to the outer loop
      (matches ptrace + seccomp's per-trap contract per
      audit A1).
    - `is_user` derived from CPL sampled via KVM_GET_SREGS
      post-VMEXIT (audit A2).
    - Originates: memo 08 sub-commit #6 + audit A1 (D70) +
      audit A2 (D70) + audit F2 (D75) + audit F7/1 (D86).

11. **`um: kvm: syscall classifier + class-D trap path`**
    - `enum kvm_syscall_class` with A/B/C/D/E classes.
    - `kvm_classify_syscall` lookup table of non-A entries
      (memo 10).
    - class-D short-circuit in kvm_decode_syscall returns
      -EPERM.
    - arch_prctl class-B FS_BASE / GS_BASE propagation.
    - Originates: memo 10 + audit F10 (D84).

### Systrap gadget (patches 12-14)

12. **`um: kvm: gadget state channel (MSR_KERNEL_GS_BASE + vvar)`**
    - Per-vCPU state page mapped P (no US, no RW) at
      bootstrap_va + PAGE_SIZE.
    - Shared vvar clock page mapped P | RW (no US) at
      bootstrap_va + 2 * PAGE_SIZE.
    - Refresh in kvm_enter_guest; seqlock write pattern in
      kvm_gadget_vvar_refresh.
    - Originates: memo 11 G3 + G5a + audit F5/F8 mapping
      tightening.

13. **`um: kvm: LSTAR gadget body — 11 CLASS_E handlers`**
    - 7 pid-family + sched_yield + clock_gettime + time +
      getcpu handlers inline in LSTAR.
    - Upper-NR guard (`cmp $0x135, %eax` pre-check +
      `test $0xffffff00, %eax`) prevents low-byte alias
      hijacks (audit F4).
    - clock_gettime seqlock retry uses %edx (not %eax) so
      NR survives fallback (audit F7/2).
    - clock_gettime call budget bounds staleness to
      ≤10000 calls (audit F8).
    - KUnit `kvm_bootstrap_lstar_bytes_test` byte-matches
      the 327-byte LSTAR table.
    - Originates: memo 11 G2 → G6 ladder + audit F4 + F7/2
      + F8.

14. **`um: kvm: gadget-mid-fault → -EFAULT via handle_syscall`**
    - `kvm_gadget_fault_nr` classifies faulting RIP as
      gadget-body vs user-code.
    - #PF handler diverts gadget-mid-store faults to
      `kvm_decode_syscall` so POSIX -EFAULT semantics apply
      (audit F7/1).
    - Originates: audit F7/1 (D86).

### Measurement + docs (patch 15)

15. **`Documentation/virt/uml: KVM backend + systrap gadget`**
    - `Documentation/virt/uml/backends.rst` KVM-specific
      §§ (backend lifecycle, CR3 model, shadow PT semantics,
      systrap gadget).
    - MAINTAINERS entry (arch/um/backend/kvm/ under the UML
      maintainer plus a KVM contact for the shadow PT +
      VMEXIT dispatcher).
    - Originates: memo 06 documentation work.

## Routing

Per SUBMISSION-QUEUE.md §"Series 7":

- **Primary list:** `linux-um@lists.infradead.org`
- **Cc:** `kvm@vger.kernel.org`, Paolo Bonzini + Sean
  Christopherson (KVM x86), Richard Weinberger + Johannes
  Berg + Anton Ivanov + Benjamin Berg + Tiwei Bie (UML),
  `linux-arch@vger.kernel.org` (ops-table consumers).
- Run `scripts/get_maintainer.pl` against the rebased
  series before emission.

## Hard prerequisites

Before this series can go out, the following must all be
true:

1. **Series 4 (backend-ops-abstraction-rfc) landed upstream.**
   Series 7 patches 1-2 depend on the typed
   `struct um_backend_ops` header Series 4 introduces.
   Sending 7 before 4 forces reviewers to reconstruct
   the dispatch shape from scratch.

2. **Squash pass against the 69-commit branch.** The
   current branch organization is optimized for redesign
   review, not upstream. Squashing is a separate
   engineering exercise that needs:
   - A clean topic branch off `master`.
   - `git rebase -i` with per-patch semantic grouping
     (see "Planned patch ordering" above).
   - `checkpatch.pl --strict` run on every squashed
     patch.
   - Build + KUnit + perf-getpid verification on every
     step of the bisect.

3. **G8 fleet numbers validated on silicon the
   maintainers can reproduce.** Cover letter cites
   s0-s7 results; ideally at least one P-core Intel,
   one AMD Zen, and one server-class Xeon. Current
   numbers (commit `d22103ba4fe6`) satisfy this.

4. **Upstream audit round on a fresh branch.** After
   squashing, rerun checkpatch + the external audit
   prompts against the final patch-emission order.
   Any findings become a v2.

## Framing pitch (cover letter)

See `0000-cover-letter.patch.md` (this directory, WIP
draft) for the cover-letter body. Key pitch points:

- **Why a KVM backend for UML.** ptrace is slow (~52k
  cyc/getpid). seccomp is faster (~41k) but requires
  the syscall-user-dispatch infrastructure. KVM gives
  us hardware-enforced ring-3 execution with
  <4k cyc/getpid on the fallback path and ~100 cyc
  (~30 ns) on the in-guest systrap gadget fast path —
  a 400× speedup vs ptrace, 13× vs seccomp on the
  fallback, 1600× vs seccomp on the gadget.

- **How it fits upstream.** Class-E gadget handlers
  are in an opt-in Kconfig
  (`UM_BACKEND_KVM_GADGET`); fallback semantics are
  identical to class A for every gadget-handled
  syscall, so a disabled-gadget kernel behaves
  exactly like the ptrace/seccomp backends. The
  gadget bytes are a single static table
  (`kvm_bootstrap_lstar_bytes[]`) with a KUnit
  byte-match test so drift is caught at boot.

- **Security posture.** The gadget runs in ring-0
  inside the guest's own VM — it cannot escape to
  the host without a KVM CVE (same trust model as
  any KVM guest). The bootstrap page is
  K-mapped only (US=0); state + vvar pages drop
  US (F5/F8 tightening). Classifier's class-D list
  traps the dangerous syscalls (ptrace, reboot,
  bpf, kexec, module-loading) at -EPERM before
  they reach UML's handle_syscall.

- **Measurements.** G8 fleet bench (s0-s7) cleared
  the pre-registered <100 ns target on every host
  (23-34 ns range). gadget:fallback ratio ≤ 0.012
  everywhere; gadget:seccomp ratio 0.002-0.009.
  Reproducibility instructions in
  `tools/testing/selftests/um/perf-getpid/run-perf-getpid.sh`
  (dual-binary mode).

## checkpatch discipline

Every squashed patch must pass `checkpatch.pl --strict
--no-signoff` clean. The fork already keeps checkpatch
clean on each commit (`CLAUDE.md` memory), so the
squash pass inherits that baseline; the squash itself
doesn't introduce new warnings.

Known items to spot-check:

- Long string literals in the LSTAR dispatch comments
  (they're architecturally precise — `0xffffff7b`
  rel32 bytes etc. — not a style issue, but checkpatch
  sometimes complains about LONG_LINES around 100 cols).
- Multi-line macro definitions in `kvm_backend.h`'s
  `KVM_X86_PTE_*` (already hand-checked for
  parenthesization).

## Post-send resolution path

Each review round = new `v<N>/` directory sibling in
`upstream-patches/kvm-backend-series/`. On land, move
to `upstream-patches/landed/kvm-backend-series-vN/` and
append the landing commit IDs to this file.

## Session log pointer

Author activity 2026-03 through 2026-04 landed all
work referenced in "Planned patch ordering." Key
arc: D66 → D71 (G2 GO) → D73 (G5 landed) → D79
(G8 GO) → audit round-5 closures (D81/D82/D83/D84/
D85/D86) → this scoping doc. Decisions-log is
authoritative for chronology.
