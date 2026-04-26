# TLB Invalidation Discipline for the Integrated KVM Backend

**Status:** Investigation complete. Recommends replacing the CR3-bit-12 toggle hack
in `kvm_enter_guest` with a CR4.PGE-toggle keystone, plus adding a missing-flush
audit checklist for the shadow_sync.c / lifecycle.c mutators.

**Branch:** `uml-redesign-plan` (post `901213a8d2d1` keystone)
**Gate:** cpython parity 17/21; remaining 4-5 modules are multi-task / threading.
**Author note:** every code citation is `path:line` against the current tree.

## Table of contents

1. [Why same-CR3 KVM_SET_SREGS does not flush](#1-why-same-cr3-kvm_set_sregs-does-not-flush)
2. [KVM userspace TLB API surface](#2-kvm-userspace-tlb-api-surface)
3. [In-kernel KVM TLB request flags](#3-in-kernel-kvm-tlb-request-flags)
4. [The current keystone hack — analysis & risks](#4-the-current-keystone-hack--analysis--risks)
5. [Architectural recommendations — option matrix](#5-architectural-recommendations--option-matrix)
6. [Missing-flush audit (shadow_sync.c / lifecycle.c)](#6-missing-flush-audit)
7. [Recommendation & commit plan](#7-recommendation--commit-plan)
8. [Appendix A — instruction-level cost notes](#appendix-a--instruction-level-cost-notes)
9. [Appendix B — multi-task threading hypothesis](#appendix-b--multi-task-threading-hypothesis)

---

## 1. Why same-CR3 KVM_SET_SREGS does not flush

### 1.1 The path the comment claims (and why it's wrong)

The keystone fix in `arch/um/backend/kvm/thread.c:2310-2351` reads:

> "KVM's `kvm_set_cr3` calls `invalidate_pcid` only conditionally"

This comment is *factually misleading*. `kvm_set_cr3` is **never reached** through
`KVM_SET_SREGS`. It is reached only from:

* `arch/x86/kvm/x86.c:8624` — `emulator_set_cr` for guest `MOV CR3` instructions
  that end up emulated (rare on TDP).
* `arch/x86/kvm/x86.c:14328` — INVPCID emulation (single-context mode).

The KVM_SET_SREGS path runs entirely through `__set_sregs_common` →
`__set_sregs` (`arch/x86/kvm/x86.c:12451-12544`).

### 1.2 What __set_sregs_common actually does with sregs.cr3

```
arch/x86/kvm/x86.c:12473   vcpu->arch.cr2 = sregs->cr2;
arch/x86/kvm/x86.c:12474   *mmu_reset_needed |= kvm_read_cr3(vcpu) != sregs->cr3;
arch/x86/kvm/x86.c:12475   vcpu->arch.cr3 = sregs->cr3;
arch/x86/kvm/x86.c:12476   kvm_register_mark_dirty(vcpu, VCPU_EXREG_CR3);
arch/x86/kvm/x86.c:12477   kvm_x86_call(post_set_cr3)(vcpu, sregs->cr3);
```

Only the **inequality test** at line 12474 contributes to `mmu_reset_needed`.
`vcpu->arch.cr3` is updated **unconditionally**, but with the same value, so the
VMCS GUEST_CR3 field doesn't actually need to change either (`vmx_load_mmu_pgd`
short-circuits — see below).

`kvm_x86_call(post_set_cr3)` is a no-op on VMX (no `.post_set_cr3` set in
`vmx_x86_ops` at all), and on SVM only does anything for SEV-ES guests
(`arch/x86/kvm/svm/svm.c:1732-1748`).

### 1.3 What __set_sregs gates on mmu_reset_needed

```
arch/x86/kvm/x86.c:12529   if (mmu_reset_needed) {
arch/x86/kvm/x86.c:12530           kvm_mmu_reset_context(vcpu);
arch/x86/kvm/x86.c:12531           kvm_make_request(KVM_REQ_TLB_FLUSH_GUEST, vcpu);
arch/x86/kvm/x86.c:12532   }
```

So the **only** way KVM_SET_SREGS issues a TLB flush is to make at least one of
these differ from the current vCPU state:

* CR3 (line 12474)
* EFER (line 12481)
* CR0 (line 12484)
* CR4 (line 12487)
* PAE PDPTRs (line 12490-12497, only when `is_pae_paging(vcpu)`)

Same-CR3 + same-CR0/CR4/EFER = `mmu_reset_needed=0` → no
`KVM_REQ_TLB_FLUSH_GUEST` → no flush at the next VMENTER.

### 1.4 What VMCS does on same-CR3

`vmx_load_mmu_pgd` at `arch/x86/kvm/vmx/vmx.c:3489`:

```
arch/x86/kvm/vmx/vmx.c:3504   else if (kvm_register_is_dirty(vcpu, VCPU_EXREG_CR3))
arch/x86/kvm/vmx/vmx.c:3505           guest_cr3 = vcpu->arch.cr3;
arch/x86/kvm/vmx/vmx.c:3506   else /* vmcs.GUEST_CR3 is already up-to-date. */
arch/x86/kvm/vmx/vmx.c:3507           update_guest_cr3 = false;
arch/x86/kvm/vmx/vmx.c:3508   vmx_ept_load_pdptrs(vcpu);
...
arch/x86/kvm/vmx/vmx.c:3514   if (update_guest_cr3)
arch/x86/kvm/vmx/vmx.c:3515           vmcs_writel(GUEST_CR3, guest_cr3);
```

The `kvm_register_mark_dirty(vcpu, VCPU_EXREG_CR3)` at line 12476 *does* mark
CR3 dirty so the value will be re-written into VMCS GUEST_CR3. But — and this is
the key architectural detail — **writing the same VMCS GUEST_CR3 value does not
on its own flush TLB on VMENTER**. The Intel SDM Vol 3 Sec 28.3.3 says
VPID-tagged TLBs survive CR3 reload unless explicitly invalidated; on bare MOV
to CR3 the architecture flushes via the implicit non-NOFLUSH side effect, but
that path is bypassed here.

### 1.5 What enable_ept (TDP) does to the picture

`tdp_enabled` is true in essentially every modern KVM/VMX deployment
(`arch/x86/kvm/mmu/mmu.c:6609`). Under TDP, GUEST_CR3 is a guest-physical
address that the CPU walks through EPT — but the **guest's TLB** is still tagged
with VPIDs, and stale VA→GPA cached translations remain valid until either:

* the guest issues `MOV CR3` (architectural full flush of non-global PCID 0
  entries),
* the guest issues `INVLPG`/`INVPCID`,
* the host issues `INVVPID` (via `vpid_sync_*` in
  `arch/x86/kvm/vmx/vmx.c:3262-3268`),
* a VMENTER occurs with the VMCS-internal state requesting a flush
  (`KVM_REQ_TLB_FLUSH_GUEST` → `vmx_flush_tlb_guest` → `vpid_sync_context`).

So same-value CR3 stuffing through SREGS does **none** of those — the guest's
shadow VA→PFN entries persist across the entry, even though our software shadow
PT (the guest-page-walk-target memory in our identity map) was mutated by
`shadow_sync.c`.

### 1.6 The empirical proof

`thread.c:2319-2324` cites the read_test5 harness: without the toggle, only 1/8
host #PFs fire on a re-mmap'd VA. The other 7 reads short-circuit through stale
TLB entries that point at the OLD PFN, returning prior file content. With the
toggle: 8/8 faults, 8/8 installs. This matches the SDM exactly.

### 1.7 Aside: the kvm_invalidate_pcid path (for completeness)

`kvm_invalidate_pcid` at `arch/x86/kvm/x86.c:1407-1448` runs only via
`kvm_set_cr3` and the INVPCID emulator. Its TDP-enabled fast path
(line 1420-1423) does:

```
arch/x86/kvm/x86.c:1420   if (unlikely(tdp_enabled)) {
arch/x86/kvm/x86.c:1421           kvm_make_request(KVM_REQ_TLB_FLUSH_GUEST, vcpu);
arch/x86/kvm/x86.c:1422           return;
arch/x86/kvm/x86.c:1423   }
```

So **if** `kvm_set_cr3` were on the SREGS path (it isn't), TDP-enabled would do
the right thing. The SREGS path's `mmu_reset_needed` check effectively replaces
this — but only on actual change.

---

## 2. KVM userspace TLB API surface

This section enumerates every userspace mechanism we could plausibly invoke from
`kvm_enter_guest` to force a guest-TLB invalidation.

### 2.1 KVM_DIRTY_TLB ioctl

* **Definition:** `include/uapi/linux/kvm.h:1414`  
  `#define KVM_DIRTY_TLB _IOW(KVMIO, 0xaa, struct kvm_dirty_tlb)`
* **Cap:** `KVM_CAP_SW_TLB` (`include/uapi/linux/kvm.h:819`).
* **Implementation:** `arch/powerpc/kvm/powerpc.c:2075`. **PowerPC only.**
* **x86 status:** Not implemented. `KVM_CAP_SW_TLB` returns 0 on x86.
* **Verdict:** **Not usable.**

### 2.2 KVM_SET_VCPU_EVENTS

* **Definition:** `include/uapi/linux/kvm.h:1398`.
* **Implementation:** `arch/x86/kvm/x86.c:5644-5755` (`kvm_vcpu_ioctl_x86_set_vcpu_events`).
* **TLB capability:** None. Sets only NMI / SIPI / SMM / interrupt / triple-fault state.
  No path issues `kvm_make_request(KVM_REQ_TLB_FLUSH_*)`.
* **Verdict:** Cannot be used to force TLB flush.

### 2.3 KVM_SET_SREGS / KVM_SET_SREGS2

* **Definition:** `include/uapi/linux/kvm.h` (KVM_SET_SREGS = 0x84, SREGS2 = 0xcd).
* **Implementation:** `arch/x86/kvm/x86.c:12580-12593`.
* **TLB-flush triggers:**
    * Any of (CR3, CR0, CR4, EFER) differs from current vCPU state →
      `mmu_reset_needed=1` → `kvm_make_request(KVM_REQ_TLB_FLUSH_GUEST, vcpu)`
      (line 12531).
    * PAE PDPTRs reload (line 12490-12497) sets `mmu_reset_needed=1` too.
* **Cost:** the ioctl itself is ~2-5 µs; the resulting `vmx_flush_tlb_guest` does
  one `vpid_sync_context` (single VPID-tagged INVVPID) which is ~50-100 ns.
* **Verdict:** **THIS IS WHAT WE'RE USING.** The hack toggles CR3 to fake
  inequality.

### 2.4 KVM_SET_REGS / KVM_SYNC_X86_REGS via run->kvm_dirty_regs

* `arch/x86/kvm/x86.c` (KVM_SET_REGS path, around line 11820 and following).
* No TLB-flush implications — only sets RIP/RSP/RFLAGS/GPRs.
* **Verdict:** Not relevant to TLB.

### 2.5 KVM_SET_USER_MEMORY_REGION / KVM_SET_USER_MEMORY_REGION2

* `virt/kvm/kvm_main.c:5169-5198`.
* Adding/removing/resizing a memslot triggers MMU notifier-style invalidation
  via `kvm_arch_flush_shadow_*` which calls `kvm_flush_remote_tlbs` (`virt/kvm/
  kvm_main.c:293`).
* **Cost:** ~10-50 µs per ioctl (memslot mutation is heavyweight; updates the
  rmap, walks SP cache, etc.).
* **Verdict:** Heavyweight, not appropriate for per-entry use. Could be useful as
  a one-time bulk flush after a heavy shadow-PT mutation, but our identity
  memslot covers the entire 512 MiB physmem so we'd be flushing everything.

### 2.6 Hyper-V KVM_HC_FLUSH_TLB hypercall (KVM_HV_PV_TLB_FLUSH)

* `arch/x86/kvm/hyperv.c:2007-2200` (`kvm_hv_flush_tlb`).
* Available only when guest CPUID exposes Hyper-V vendor and the
  `KVM_CAP_HYPERV_DIRECT_TLBFLUSH` cap is enabled.
* Triggered from inside the guest — **not from userspace**.
* **Verdict:** Not usable from `kvm_enter_guest` userspace.

### 2.7 KVM_HC_KICK_CPU / PV_EOI / PV_TLB_FLUSH (kvmclock)

* `arch/x86/kvm/x86.c:3786-3822` (`record_steal_time` triggers a guest-side flush
  via `KVM_VCPU_FLUSH_TLB` flag in steal_time struct).
* This is a guest-driven path — the guest sets the flag, KVM observes on
  preemption, calls `kvm_vcpu_flush_tlb_guest`. Not actuatable from userspace
  before VMENTER.
* **Verdict:** Not usable.

### 2.8 KVM_SET_GUEST_DEBUG

* Doesn't touch TLB.

### 2.9 INVLPG injection via guest-instruction gadget

* No KVM ioctl issues a single-page INVLPG. `kvm_mmu_invlpg`
  (`arch/x86/kvm/mmu/mmu.c:6562-6577`) is reached only from guest INVLPG
  emulation (`arch/x86/kvm/x86.c:8531` emulator hook; `arch/x86/kvm/vmx/vmx.c:5816`
  VMEXIT handler; `arch/x86/kvm/svm/svm.c:2394, 2506`).
* **However** we can stuff RIP at a guest-side gadget that runs `INVLPG (mem)` or
  `INVPCID` and then exits. Same machinery as the LSTAR trampoline at
  `arch/um/backend/kvm/thread.c:472, 654` — see option (d) in §5.
* **Verdict:** Implementable, surgical, but requires gadget infrastructure.

### 2.10 Toggling CR4.PGE inside SREGS

* `arch/x86/kvm/x86.c:1356-1358`:
  ```
  if (((cr4 ^ old_cr4) & X86_CR4_PGE) ||
      (!(cr4 & X86_CR4_PCIDE) && (old_cr4 & X86_CR4_PCIDE)))
          kvm_make_request(KVM_REQ_TLB_FLUSH_GUEST, vcpu);
  ```
* **Caveat:** This is in `kvm_post_set_cr4`, which is called from `kvm_set_cr4`
  (line 1401), **not** from `__set_sregs_common`. The SREGS path uses
  `kvm_x86_call(set_cr4)(vcpu, sregs->cr4)` (line 12488) which is the vendor
  callback (vmx_set_cr4 / svm_set_cr4) directly — bypassing `kvm_post_set_cr4`.
* **However** — change of `sregs->cr4` triggers `mmu_reset_needed |=
  kvm_read_cr4(vcpu) != sregs->cr4` (line 12487), which lands in the same
  `kvm_make_request(KVM_REQ_TLB_FLUSH_GUEST)` at line 12531 anyway.
* **Implication:** **Toggling CR4.PGE bit on the sentinel SREGS write produces
  the same KVM_REQ_TLB_FLUSH_GUEST as the CR3 toggle** — but with a single ioctl
  instead of two, **and** without ever stuffing a "fake" CR3 (so no
  reserved-bit / CR3-validity-check risk).

This is the cleanest mechanism. See §5 option (c).

### 2.11 Summary table

| Mechanism                            | x86 supported | Per-page granularity | Cost per call | Userspace-actuatable | Notes |
|--------------------------------------|--------------|----------------------|---------------|----------------------|-------|
| KVM_DIRTY_TLB                        | No           | Yes                  | n/a           | Yes                  | PowerPC only |
| KVM_SET_VCPU_EVENTS                  | n/a          | n/a                  | n/a           | Yes                  | No TLB hooks |
| KVM_SET_SREGS w/ different CR3       | Yes          | No (full guest flush)| 1 ioctl       | Yes                  | Current hack |
| KVM_SET_SREGS w/ toggled CR4.PGE     | Yes          | No (full guest flush)| 1 ioctl       | Yes                  | **Recommended** |
| KVM_SET_USER_MEMORY_REGION          | Yes          | No (full physmem)    | 10-50 µs      | Yes                  | Too heavy |
| Inject INVLPG via gadget RIP stuff   | Yes          | Yes                  | 1 KVM_RUN     | Yes (indirect)       | See §5(d) |
| Hyper-V KVM_HC_FLUSH_TLB            | Yes          | Yes                  | n/a           | No (guest-driven)    | Can't trigger from userspace |
| Add new UAPI ioctl                  | Patch needed | Either               | TBD           | Yes                  | Out of scope |

---

## 3. In-kernel KVM TLB request flags

For reference; we cannot directly invoke these from userspace, but understanding
their semantics tells us what `KVM_SET_SREGS+mmu_reset` actually buys us.

### 3.1 KVM_REQ_TLB_FLUSH (generic)

* **Definition:** `arch/x86/include/asm/kvm_host.h` (inherited from
  `include/linux/kvm_host.h:KVM_REQ_TLB_FLUSH`).
* **Maker:** `kvm_make_all_cpus_request(kvm, KVM_REQ_TLB_FLUSH)` from
  `kvm_flush_remote_tlbs` (`virt/kvm/kvm_main.c:309`).
* **Service:** `arch/x86/kvm/x86.c:11218-11219` →
  `kvm_vcpu_flush_tlb_all(vcpu)` (`arch/x86/kvm/x86.c:3692-3699`):
    * VMX EPT: `ept_sync_global` → INVEPT all-context. Wipes every cached
      GVA→GPA *and* GPA→HPA across all VPIDs.
    * VMX VPID-only: `vpid_sync_vcpu_global` if INVVPID-global supported, else
      single-context for vmx->vpid + nested vpid02.
    * Clears KVM_REQ_TLB_FLUSH_CURRENT (superset).
* **Userspace:** Not directly reachable. Triggered from MMU-notifier paths,
  memslot updates, etc.

### 3.2 KVM_REQ_TLB_FLUSH_CURRENT

* **Definition:** `arch/x86/include/asm/kvm_host.h:120`
  `#define KVM_REQ_TLB_FLUSH_CURRENT KVM_ARCH_REQ(26)`
* **Service:** `arch/x86/kvm/x86.c:3726-3730`
  `kvm_vcpu_flush_tlb_current` → `vmx_flush_tlb_current`
  (`arch/x86/kvm/vmx/vmx.c:3313`):
    * EPT: `ept_sync_context(eptp)` — single-context INVEPT (current root).
    * Non-EPT: `vpid_sync_context(vmx_get_current_vpid(vcpu))` — single VPID.
* **Drained at:** `arch/x86/kvm/x86.c:11221`
  `kvm_service_local_tlb_flush_requests(vcpu)`.
* **Maker examples:** `kvm_invalidate_pcid` line 1432 (matching active PCID),
  `kvm_post_set_cr4` line 1368 (CR4.PAE / CR4.SMEP toggle), MMU-internal
  paths.

### 3.3 KVM_REQ_TLB_FLUSH_GUEST

* **Definition:** `arch/x86/include/asm/kvm_host.h:121-122`
  `#define KVM_REQ_TLB_FLUSH_GUEST KVM_ARCH_REQ_FLAGS(27, KVM_REQUEST_WAIT|KVM_REQUEST_NO_WAKEUP)`
* **Service:** `arch/x86/kvm/x86.c:3701-3723`
  `kvm_vcpu_flush_tlb_guest`:
    * Non-TDP: walks all roots, calls `kvm_mmu_sync_roots` and
      `kvm_mmu_sync_prev_roots`.
    * Calls `vmx_flush_tlb_guest` →
      `vpid_sync_context(vmx_get_current_vpid(vcpu))`.
    * Purges Hyper-V precise-flush queue.
* **Drained at:** `arch/x86/kvm/x86.c:11221` via
  `kvm_service_local_tlb_flush_requests`.
* **Makers:** `kvm_invalidate_pcid` line 1421 (TDP path), `__set_sregs` line
  12531, `__set_sregs2` line 12575, MSR writes (line 13141, 13155), INVPCID
  emulation (line 14346), nested VMX paths.
* **This is what our SREGS-based hack triggers.**

### 3.4 KVM_REQ_HV_TLB_FLUSH

* `arch/x86/include/asm/kvm_host.h:129-130`. Hyper-V precise flush queue.
* Service: `arch/x86/kvm/x86.c:11230-11233`. Drains a per-vCPU FIFO of
  `(gva, count)` ranges into `kvm_x86_call(flush_tlb_gva)`.
* Userspace can't enqueue without exposing Hyper-V capabilities to the guest.

### 3.5 KVM_REQ_LOAD_MMU_PGD / KVM_REQ_MMU_SYNC

* `arch/x86/kvm/x86.c:11208-11211`. Triggered by `kvm_mmu_new_pgd`
  (`arch/x86/kvm/mmu/mmu.c:5234`); reloads CR3 in VMCS (under EPT, the
  GUEST_CR3 field) but does **not** by itself flush TLB unless paired with
  `force_flush_and_sync_on_reuse` (line 5236-5239), which is normally false.

### 3.6 Order of processing at vcpu_enter_guest

`arch/x86/kvm/x86.c:11195-11221`:

```
11195   KVM_REQ_MMU_FREE_OBSOLETE_ROOTS
11208   KVM_REQ_MMU_SYNC          → kvm_mmu_sync_roots
11210   KVM_REQ_LOAD_MMU_PGD      → kvm_mmu_load_pgd (writes GUEST_CR3)
11218   KVM_REQ_TLB_FLUSH (all)   → kvm_vcpu_flush_tlb_all
11221   kvm_service_local_tlb_flush_requests:
            KVM_REQ_TLB_FLUSH_CURRENT → kvm_vcpu_flush_tlb_current
            KVM_REQ_TLB_FLUSH_GUEST   → kvm_vcpu_flush_tlb_guest
11230   KVM_REQ_HV_TLB_FLUSH      → drain precise queue
```

Everything happens **before** the actual VMENTER. Crucially the GUEST_CR3 write
(11210) precedes the TLB flush (11218-11221), so even if we trigger
LOAD_MMU_PGD without any of the FLUSH requests, the new VMCS GUEST_CR3 would
land in the guest with the stale TLB intact.

---

## 4. The current keystone hack — analysis & risks

`arch/um/backend/kvm/thread.c:2339-2351`:

```c
{
    struct kvm_um *ctx = kvm_backend_ctx();
    bool same_cr3 = ctx->sregs_primed &&
                    ctx->cached_cr3_gpa == cr3_gpa;

    if (same_cr3) {
        struct kvm_sregs s2 = sregs;

        s2.cr3 = sregs.cr3 ^ 0x1000;
        (void)os_ioctl_generic(vcpu_fd, KVM_SET_SREGS,
                               (unsigned long)&s2);
    }
}
rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS, (unsigned long)&sregs);
```

### 4.1 What works

* `cr3 ^ 0x1000` differs from current → `mmu_reset_needed=1` (line 12474).
* `__set_sregs` enqueues `KVM_REQ_TLB_FLUSH_GUEST` (line 12531).
* Second SREGS write restores the real CR3 → `mmu_reset_needed=1` again
  (because `vcpu->arch.cr3` was just stuffed with the sentinel) → second
  `KVM_REQ_TLB_FLUSH_GUEST`.
* Net effect: at the next `KVM_RUN`, `vcpu_enter_guest` services the (idempotent)
  `KVM_REQ_TLB_FLUSH_GUEST` once → `vpid_sync_context` → INVVPID.
* `vpid_sync_context` invalidates **all** GVA→GPA entries for the active VPID,
  which is what we want.

### 4.2 What is fragile

#### 4.2.1 Sentinel CR3 validity check

`__set_sregs_common` at line 12457 calls `kvm_is_valid_sregs`
(`arch/x86/kvm/x86.c:12426-12449`), which at line 12436 calls
`kvm_vcpu_is_legal_cr3(vcpu, sregs->cr3)`:

```
arch/x86/kvm/cpuid.h:280   static inline bool kvm_vcpu_is_legal_cr3(struct kvm_vcpu *vcpu, unsigned long cr3)
arch/x86/kvm/cpuid.h:281   {
arch/x86/kvm/cpuid.h:282           if (guest_cpu_cap_has(vcpu, X86_FEATURE_LAM))
arch/x86/kvm/cpuid.h:283                   cr3 &= ~(X86_CR3_LAM_U48 | X86_CR3_LAM_U57);
arch/x86/kvm/cpuid.h:284
arch/x86/kvm/cpuid.h:285           return kvm_vcpu_is_legal_gpa(vcpu, cr3);
arch/x86/kvm/cpuid.h:286   }

arch/x86/kvm/cpuid.h:77    static inline bool kvm_vcpu_is_legal_gpa(struct kvm_vcpu *vcpu, gpa_t gpa)
arch/x86/kvm/cpuid.h:78    {
arch/x86/kvm/cpuid.h:79            return !(gpa & vcpu->arch.reserved_gpa_bits);
arch/x86/kvm/cpuid.h:80    }
```

For the toggle `cr3 ^ 0x1000`:

* Bit 12 is page-aligned and below `MAXPHYADDR=46` (typical), so the toggle
  produces a valid GPA on every CPU we ship to.
* `reserved_gpa_bits` is computed from `vcpu->arch.maxphyaddr` and is normally
  bits >= 46 or 52 — bit 12 is never reserved.
* No PCIDE bit → no `X86_CR3_PCID_NOFLUSH` collision.
* No LAM enabled in our CR4 → no LAM bit interaction.

**Risk:** this is **safe today** but assumes:
1. `MAXPHYADDR > 12` (true on every x86_64).
2. CR4.PCIDE is not set (true today; see `arch/um/backend/kvm/sregs.c:261`).
3. CR4.LAM is not set (true today; LAM not in our CR4).
4. The `cr3 ^ 0x1000` GPA isn't backed by a memslot whose contents matter when
   KVM speculatively walks it. The GPA is "dummy" — KVM only stores it in
   `vcpu->arch.cr3`; the next SREGS overwrites it before the VMENTER actually
   walks. **But** between the two ioctls, KVM may fault if e.g. nested SVM/VMX
   paths walk the value. We don't run nested → ok.

**Failure mode if violated:** `__set_sregs` returns `-EINVAL`; the wrapper
discards it (`(void)os_ioctl_generic(...)`); the second SREGS still runs but
now without the `mmu_reset_needed` trigger (CR3 didn't change for the second
write either, because the first write was rejected) → **silent regression to
the original missed-flush bug**.

We currently swallow the return code at line 2348-2349. **This is a latent
issue.** If a future kernel hardens validity (e.g. checks that CR3 points into
a real memslot), the toggle silently degrades.

#### 4.2.2 Two ioctls per dirty entry

* Each SREGS call is ~3-5 µs.
* On a clean entry (sregs-skip-cache hits) we pay zero ioctls.
* On a dirty-with-same-CR3 entry we now pay **two** ioctls instead of one.
* On a dirty-with-different-CR3 entry (cross-mm switch) we pay one ioctl —
  the CR3 already differs, hack short-circuits via `same_cr3==false`.

For multi-task workloads (the cpython parity gate at 17/21) the rate of
"same-CR3, dirty" entries is exactly the rate of intra-mm shadow mutations
that survived the cached-skip. Under heavy mmap/munmap churn (e.g. test_decimal,
test_struct subinterpreters) this can hit ~100/sec. Cost: ~500 µs/sec — not
material.

#### 4.2.3 The KVM_GET_SREGS at line 2248 is unconditional

If we trip the same-CR3 path, we still issued KVM_GET_SREGS (line 2248) before
the two KVM_SET_SREGS — three total ioctls per dirty entry. The GET could be
elided when we know the merge target is unchanged but that's a §5 perf lift
for later.

#### 4.2.4 The fix is "all-or-nothing"

The CR3 toggle issues `KVM_REQ_TLB_FLUSH_GUEST` which is **full guest TLB
invalidation** (single VPID, all GVAs). For a single-page mutation
(`shadow_sync.c::kvm_shadow_sync_pte`) this is hugely wasteful — INVLPG of one
page would suffice, but we're nuking the whole VPID's TLB.

The cost of the over-flush:
* INVVPID (single-context) is a single hardware instruction, ~50-100 cycles.
* But the guest's *re-warm* cost is enormous: every TLB miss in the next
  burst of guest execution triggers a full page walk through 4 levels of
  shadow PT (or EPT, but our shadow IS the guest PT). For a Python interpreter
  with thousands of hot pages, this can add hundreds of microseconds of
  page-walk latency per shadow mutation event.

Note: this re-warm cost is the most plausible explanation for the residual
flake on `import unittest` (8/10) — the SIGALRM-driven scheduler can land
between a leaf-write and the dirty-flag update window (see §6.2 below), so the
flush either doesn't fire or fires too aggressively.

#### 4.2.5 Comment ages the wrong direction

`thread.c:2310-2338` cites "kvm_invalidate_pcid is called only conditionally" —
this is a misread of the kernel code. As shown in §1.1, that path doesn't run
at all from SREGS. The comment will mislead the next maintainer who reads it
and tries to "fix" the problem by cleaning up `kvm_invalidate_pcid`.

---

## 5. Architectural recommendations — option matrix

### Option (a) — Status quo: keep CR3 ^ 0x1000

**Pros:**
* Already shipping at 17/21.
* Simple; one local change.

**Cons:**
* Misleading comment (§4.2.5).
* Latent failure on unusual CPU configurations (LAM, MAXPHYADDR=12 — never
  actually 12, but let's be honest about the brittleness).
* Two ioctls per dirty entry.
* Swallows ioctl error code silently.

**Verdict:** Acceptable for the immediate parity gate, but not the disciplined
fix.

### Option (b) — Toggle CR4.PGE in the sentinel write

The cleanest single-change replacement. Mechanism:

```c
if (same_cr3 && !shadow_dirty_handled_already) {
    struct kvm_sregs s2 = sregs;

    /* CR4.PGE is bit 7 (0x80). Our production CR4 has neither PGE nor
     * PCIDE set (sregs.c:261). Setting PGE temporarily then clearing
     * triggers TLB_FLUSH_GUEST without ever stuffing an invalid CR3.
     */
    s2.cr4 = sregs.cr4 | X86_CR4_PGE;
    rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS, (unsigned long)&s2);
    if (rc < 0)
        return rc;
}
rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS, (unsigned long)&sregs);
```

**Why it works:**
* `__set_sregs_common` line 12487-12488 detects `cr4` change → `mmu_reset_needed=1`.
* `__set_sregs` line 12531 enqueues `KVM_REQ_TLB_FLUSH_GUEST`.
* Second write toggles CR4 back; same logic enqueues another (idempotent)
  flush.
* `kvm_is_valid_cr4` (line 12447 / `arch/x86/kvm/x86.c:1373-1404`) accepts
  CR4.PGE on any CPU that supports paging (every x86_64). No
  reserved-bit check failure mode.

**Pros:**
* Bit-7 toggle never collides with reserved bits, never violates LAM, never
  fails `kvm_vcpu_is_legal_cr3`.
* Single, clean diff.
* Code-comment matches reality (`cr4` change → mmu_reset_needed → flush).
* Failure mode of the first ioctl: caught and propagated, not swallowed.
* Same number of ioctls as today's hack (two), same flush granularity.

**Cons:**
* Still two ioctls; still nukes the whole VPID's TLB.
* Doesn't address §4.2.4 over-flush.

**Verdict:** **Should be the immediate replacement.** Strict improvement over
status quo with no behavioural risk.

### Option (c) — Single-ioctl: stuff CR4.PGE into the real SREGS write

Even cleaner: don't bother with a sentinel write. Just toggle PGE on every
write:

```c
sregs.cr4 ^= X86_CR4_PGE;
rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS, (unsigned long)&sregs);
/* Cached cr4 must track what we wrote, so subsequent same-cr4 detection
 * can skip when nothing changed. */
ctx->cached_cr4 = sregs.cr4;
```

Let `ctx->cached_cr4` swing between `CR4_BASE` and `CR4_BASE | PGE` on
alternate writes. Each write differs from the previous → mmu_reset_needed=1
every time → flush every time.

**Pros:**
* One ioctl per entry that needed any SREGS write at all.
* No "sentinel" concept; the cr4 alternation **is** the flush trigger.

**Cons:**
* Loses the ability to short-circuit subsequent same-state writes (which we
  already do via `sregs_primed && cached_cr3 && cached_fs && cached_gs &&
  !shadow->dirty` skip at line 2235-2240). Adding `cached_cr4 == sregs.cr4`
  to that test would re-enable the skip but break the alternation.
* Subtle: requires the per-CPU cache invariant "cached_cr4 reflects what was
  last written" to hold across all paths.

**Verdict:** Modest win over (b); the saved ioctl is ~3 µs * 100 dirty
entries/sec = 300 µs/sec. Not worth the cache-invariant complexity unless
profiling shows it matters.

### Option (d) — Inject a guest INVLPG via a gadget

For per-page invalidation. Shape:

1. Add a small gadget at `bootstrap_va + KVM_BOOTSTRAP_INVLPG_OFFSET`:
   ```
       invlpg (%rdi)           ; 3 bytes  — 0x0f, 0x01, 0x3f
       sysretq                 ; 3 bytes  — 0x48, 0x0f, 0x07
   ```
2. When `shadow_sync.c` mutates a leaf, append the VA to a per-vCPU
   "pending INVLPG" ring.
3. In `kvm_enter_guest`, before the productive RIP write, drain the ring by
   stuffing RIP at the gadget, RDI at each VA in turn, and running KVM until
   it hits `sysretq`. (Actually one INVLPG per round-trip is heavyweight; a
   loop gadget that walks an array of VAs in a tight `xor %rax %rax` loop is
   smarter.)

**Pros:**
* Page-granular: surgical flush, preserves hot-path TLB entries.
* True architectural model (matches what real OSes do via INVLPG broadcast).
* Future-proof against shadow-PT optimisations.

**Cons:**
* Significant new infrastructure: gadget, ring, drain loop, MMIO trap for
  ring overflow.
* One full KVM_RUN round-trip per drain (~10 µs each). Beats KVM_SET_SREGS
  only if we're invalidating one page and the guest's working set is huge.
* Race: the gadget runs in guest context with the new shadow PT installed
  — the INVLPG operates on the **new** translation, not the stale one.
  INVLPG by VA is a hardware instruction that invalidates the cached
  translation **for that VA**, regardless of what's currently in the page
  table; this is well-defined per SDM. So the race is benign.
* Still requires the SREGS-based flush as a fallback for "dirty but no
  per-page record" cases (e.g. range-clear that bailed to needs_full_resync).

**Verdict:** Better long-term; defer until the SREGS-keystone option (b) is
shipped and we have profiling data showing per-page granularity is worth the
complexity. The cpython gate moves from 17/21 to ≥20/21 with (b) alone (we
expect; see §appendix B).

### Option (e) — Refactor: shadow_sync.c emits per-page invalidate events

Logical extension of (d). Instead of the SREGS write being the
flush-trigger, treat the dirty-flag → flush coupling as an event log:

* `shadow_sync.c` mutators push `(va, kind)` tuples into a shadow ring.
* `kvm_enter_guest` drains the ring before the next KVM_RUN, choosing
  full-flush via SREGS when the ring overflowed and per-page INVLPG via
  gadget otherwise.

**Pros:**
* Cleanest separation: shadow_sync.c stops setting `shadow->dirty`; it sets
  the ring head. The dirty flag becomes implicit (ring nonempty).
* Enables future per-page INVPCID, per-page INVEPT, etc.

**Cons:**
* Big refactor. Every shadow_sync.c call site needs to push to the ring
  instead of (or in addition to) flipping `dirty`.
* Risk of regression on the well-tested status-quo flush behaviour.

**Verdict:** Don't do this in the short term. Re-evaluate after (d) ships
and we have data on per-page costs.

### Option (f) — Add a UAPI ioctl to KVM (out of scope)

A new `KVM_FORCE_TLB_FLUSH` ioctl that calls
`kvm_make_request(KVM_REQ_TLB_FLUSH_GUEST, vcpu)` directly. One line in
`arch/x86/kvm/x86.c`. But this is a kernel-API change requiring upstream
review, which UML cannot ship unilaterally.

**Verdict:** Right answer in the long term; not actionable right now. Worth
mentioning to upstream as a "would simplify many use cases including UML and
some emulator hosts."

### Recommendation summary

| Option | When                          | Risk    | Code lines |
|--------|-------------------------------|---------|-----------:|
| (a)    | Status quo                    | Latent  | 0         |
| (b)    | **Immediate replacement**     | None    | ~5        |
| (c)    | Mid-term ioctl reduction      | Medium  | ~15       |
| (d)    | After (b), if profiling needs | High    | ~150      |
| (e)    | Long-term refactor            | High    | ~400      |
| (f)    | Upstream wishlist             | n/a     | n/a       |

**Pick (b) now**, queue (c) as a perf lift, defer (d)/(e), file (f) as a
KVM-list discussion item.

---

## 6. Missing-flush audit

Sites where the shadow-PT is mutated but the dirty-flag → flush coupling may
not fire correctly. These are candidates for the residual 17/21 → 21/21 gap.

### 6.1 kvm_shadow_map_page (lifecycle.c:1381-1443)

Behaviour: walks PGD/PUD/PMD, may allocate intermediates via
`kvm_shadow_table_step`, writes the leaf at line 1439-1440, then
**unconditionally** sets `shadow->dirty = true` at line 1441.

```c
arch/um/backend/kvm/lifecycle.c:1439   pte[pte_i] = (phys_gpa & ~0xfffULL & 0x000ffffffffff000ULL) |
arch/um/backend/kvm/lifecycle.c:1440                leaf_flags;
arch/um/backend/kvm/lifecycle.c:1441   shadow->dirty = true;
```

**Issue 1 — no smp_wmb between leaf write and dirty=true.** Compare with the
shadow_sync.c discipline at line 280-282 where the WRITE_ONCE(*spte) is
followed by `smp_wmb()` then `WRITE_ONCE(shadow->dirty, true)`. Here it's a
plain `pte[pte_i] = ...` (which the compiler may reorder relative to
`shadow->dirty = true` even on x86 — the compiler barrier matters even if
the hardware barrier is implicit on x86).

**Issue 2 — overwrite of present PTE doesn't invalidate the old VA's TLB.**
If the previous leaf was present and the guest's TLB cached that translation,
the same-CR3 SREGS write triggers a full flush via the keystone. **OK** with
the keystone in place; **wrong** without.

**Issue 3 — shadow->synced is not reset.** `kvm_shadow_map_page` doesn't
touch `shadow->synced`. A direct `kvm_shadow_map_page` call (e.g. for the
gadget vvar at thread.c:2098) followed by a `cached_skip` next entry would
SREGS-skip even though the shadow now has new mappings. The dirty flag does
trigger SREGS write (line 2239), so the keystone covers this. But it's
fragile.

**Recommended fix:**
```c
arch/um/backend/kvm/lifecycle.c:1439
        WRITE_ONCE(pte[pte_i], (phys_gpa & ~0xfffULL & 0x000ffffffffff000ULL) | leaf_flags);
        smp_wmb();
        WRITE_ONCE(shadow->dirty, true);
```

### 6.2 shadow_sync.c install path (shadow_sync.c:274-298)

Already correct: `WRITE_ONCE(*spte, new); smp_wmb(); WRITE_ONCE(shadow->dirty,
true);` (lines 280-282). Comments at lines 263-273 even cite the race.

**However** — there is one subtle hole:

> "happen-before the dirty flag write so kvm_enter_guest's SREGS-skip
> predicate (which reads shadow->dirty) cannot observe a stale dirty=false"

This protects the **producer** side. The consumer side at thread.c:2239 does
`READ_ONCE(shadow->dirty)`. **There is no smp_rmb pairing** with the smp_wmb
in shadow_sync.c. On x86 this is implicit (loads are ordered with respect
to all stores), but the comment claims the pairing exists when it doesn't.

**Recommended fix:** add `smp_rmb()` between the cached-CR3/FS/GS reads and
the `READ_ONCE(shadow->dirty)` read at thread.c:2235-2240, or make the
shadow->dirty read use `smp_load_acquire`. This is purely a comment/safety
issue on x86 but would matter on other archs (UML on arm64 in some hypothetical
future).

**Stale-after-install scenario worth thinking about:**

1. Thread A: shadow_sync.c install at VA X — writes spte, smp_wmb, sets dirty.
2. SIGALRM fires between the two stores (yes — the wmb is a compiler barrier
   on x86, not a preemption point, but our cooperative scheduler can preempt).
3. Thread B (kvm_enter_guest): reads dirty (still false) → SREGS-skip → no
   flush.
4. Thread B: KVM_RUN. Guest reads VA X. The TLB had a stale "not present"
   entry from before the install → guest #PF → KVM EPT walk hits the new
   spte → installs translation. **But wait** — there was no stale TLB entry
   for "not present"; TLB only caches successful translations. So the guest
   walks the new spte directly and hits.

So the "stale-after-install" concern is **mostly self-curing** for the install
path (TLB doesn't cache absent entries). The keystone is needed for the
**clear-then-install** path (old PFN cached in TLB; install writes new PFN to
shadow; without flush, guest reads through old TLB entry to old PFN). And the
SIGALRM/dirty-flag-race scenario is the actual risk.

**Action item:** the smp_rmb pair on the consumer side is the right hardening.
Add it.

### 6.3 shadow_sync.c clear path (shadow_sync.c:193-213)

```c
arch/um/backend/kvm/shadow_sync.c:193   {
arch/um/backend/kvm/shadow_sync.c:194           u64 old = READ_ONCE(*spte);
arch/um/backend/kvm/shadow_sync.c:195
arch/um/backend/kvm/shadow_sync.c:196           if (old & 1ULL) {
arch/um/backend/kvm/shadow_sync.c:197                   WRITE_ONCE(*spte, 0);
arch/um/backend/kvm/shadow_sync.c:198                   smp_wmb();      /* P0-1 */
arch/um/backend/kvm/shadow_sync.c:199                   WRITE_ONCE(shadow->dirty, true);
```

Correct shape. Same consumer-side smp_rmb gap as §6.2.

### 6.4 shadow_sync.c install-when-translator-says-absent (lines 225-242)

```c
arch/um/backend/kvm/shadow_sync.c:225           if (spte && (READ_ONCE(*spte) & 1ULL)) {
arch/um/backend/kvm/shadow_sync.c:226                   u64 old = READ_ONCE(*spte);
arch/um/backend/kvm/shadow_sync.c:227
arch/um/backend/kvm/shadow_sync.c:228                   WRITE_ONCE(*spte, 0);
arch/um/backend/kvm/shadow_sync.c:229                   smp_wmb();      /* P0-1 */
arch/um/backend/kvm/shadow_sync.c:230                   WRITE_ONCE(shadow->dirty, true);
```

Correct.

### 6.5 shadow_sync.c needs_full_resync path (lines 250-258)

```c
arch/um/backend/kvm/shadow_sync.c:250           if (!spte) {
arch/um/backend/kvm/shadow_sync.c:251                   WRITE_ONCE(shadow->needs_full_resync, true);
arch/um/backend/kvm/shadow_sync.c:252                   WRITE_ONCE(shadow->dirty, true);
```

Sets dirty before needs_full_resync is observed. **Issue:** the order should be
`needs_full_resync` first, then `dirty`. If the consumer thread reads dirty,
runs the SREGS-skip predicate (which doesn't check needs_full_resync), and
KVM_RUNs without doing the full-fill repair, the guest walks the *old* shadow
PT.

Wait, no — line 2150 in thread.c does check `READ_ONCE(shadow->needs_full_resync)`
and forces a fill if set:

```c
arch/um/backend/kvm/thread.c:2150           if (shadow && shadow->synced &&
arch/um/backend/kvm/thread.c:2151               shadow->synced_pgd_va == (u64)mm->pgd &&
arch/um/backend/kvm/thread.c:2152               !READ_ONCE(shadow->needs_full_resync))
```

So the consumer reads needs_full_resync **before** the SREGS-skip path. Good.
But there's still a race: producer writes `needs_full_resync=true` at
shadow_sync.c:251, then `dirty=true` at line 252. Consumer at thread.c:2152
reads needs_full_resync — could observe either old (false) or new (true)
state. If false, falls through to the SREGS-skip predicate at line 2235-2240
which reads `dirty` — also could be either old (false) or new (true).

If the consumer sees old needs_full_resync (false) AND old dirty (false), it
SREGS-skips and KVM_RUNs without the full fill **and** without the flush.
This is the bug we'd see if the WRITE_ONCEs aren't ordered.

**Recommended fix:** order the producer:
```c
WRITE_ONCE(shadow->needs_full_resync, true);
smp_wmb();
WRITE_ONCE(shadow->dirty, true);
```

And add the matching `smp_rmb()` on the consumer side between the
needs_full_resync read and the dirty read.

This is a **plausible candidate for the residual flake** — multi-task
workloads churn allocation-failure paths under contention, hitting the
needs_full_resync code path more often.

### 6.6 shadow_sync.c range-clear (lines 413-431)

```c
arch/um/backend/kvm/shadow_sync.c:413           if ((end - start) >> PAGE_SHIFT >= 512) {
arch/um/backend/kvm/shadow_sync.c:414                   WRITE_ONCE(shadow->needs_full_resync, true);
arch/um/backend/kvm/shadow_sync.c:415                   WRITE_ONCE(shadow->dirty, true);
```

Same ordering issue as §6.5.

### 6.7 lifecycle.c::kvm_shadow_invalidate_va_range (lines 1498-1564)

```c
arch/um/backend/kvm/lifecycle.c:1534                   if (pte[pte_i] & KVM_X86_PTE_P) {
arch/um/backend/kvm/lifecycle.c:1535                           pte[pte_i] = 0;
arch/um/backend/kvm/lifecycle.c:1536                           cleared++;
arch/um/backend/kvm/lifecycle.c:1537                   }
...
arch/um/backend/kvm/lifecycle.c:1560           shadow->dirty = true;
arch/um/backend/kvm/lifecycle.c:1561           shadow->synced = false;
```

**Issue 1 — no WRITE_ONCE on the PTE clear.** Compiler can reorder.

**Issue 2 — no smp_wmb between PTE clears and dirty=true.**

**Issue 3 — runs under `guard(mutex)(&shadow->fill_lock)` (line 1511)** which
provides ordering against concurrent fill, but **not** against the lock-free
SREGS-skip path in `kvm_enter_guest` (which doesn't take the fill_lock).

**Recommended fix:** convert all the PTE writes in this loop to WRITE_ONCE,
add smp_wmb before setting `dirty`, and use `WRITE_ONCE(shadow->dirty, true)`.

This is the **other** plausible candidate for the residual flake — invalidate
ranges fire on every kvm_mm_unmap (mm.c:229) which is part of the munmap
fast path under load.

### 6.8 lifecycle.c::kvm_shadow_pgd_clear_user (lines 1480-1495)

```c
arch/um/backend/kvm/lifecycle.c:1493           if (shadow)
arch/um/backend/kvm/lifecycle.c:1494                   shadow->dirty = true;
```

Plain assignment, no WRITE_ONCE. Setter is on a teardown path (per #275) so
race window is small, but worth WRITE_ONCE-ing for consistency.

### 6.9 Missing INVLPG-on-install scenario (the cited concern)

> "For the shadow_sync.c install path: does the new install need an INVLPG
> of the just-installed VA? (Stale-after-install scenario where TLB has the
> OLD non-present entry)"

Per §6.2: hardware TLBs do not cache absent entries (Intel SDM Vol 3 4.10.2.3).
So the "stale non-present TLB → blocks new present install" scenario is
hardware-impossible. The only stale-TLB-after-install scenarios are:

1. **Old present entry → new present entry** (mmap-over-mmap, mprotect, COW).
   The keystone full flush handles this.

2. **Old present entry → cleared, intermediate kvm_enter_guest, new present
   entry installed at the same VA.** The first kvm_enter_guest's flush handles
   the clear; the second install has no stale TLB to worry about.

So **no per-install INVLPG is needed** in the basic install path. This is
worth documenting in shadow_sync.c so the next maintainer doesn't add a
spurious invalidation.

### 6.10 Audit summary

| File                                     | Site                          | Issue                                              | Severity |
|------------------------------------------|-------------------------------|----------------------------------------------------|----------|
| lifecycle.c:1381-1443 (`kvm_shadow_map_page`) | install path              | no smp_wmb, no WRITE_ONCE on leaf                  | medium   |
| lifecycle.c:1498-1564 (`kvm_shadow_invalidate_va_range`) | bulk clear      | no WRITE_ONCE, no smp_wmb                          | **high** |
| shadow_sync.c:250-258 (needs_full_resync) | producer ordering            | `dirty` written before `needs_full_resync` smp_wmb | **high** |
| shadow_sync.c:413-431 (range-clear bail)  | producer ordering            | same as above                                      | high     |
| thread.c:2235-2240 (SREGS-skip predicate) | consumer ordering            | no smp_rmb pair                                    | medium   |
| lifecycle.c:1480-1495 (`pgd_clear_user`)  | dirty flag set               | no WRITE_ONCE                                      | low      |

The top three (high severity) are plausible explanations for the residual
flake on `import unittest` (8/10) and the multi-task module failures.

---

## 7. Recommendation & commit plan

### What to commit (immediate, single PR)

**Commit 1 — Replace CR3-toggle keystone with CR4.PGE-toggle keystone**
(`arch/um/backend/kvm/thread.c`):

* Lines 2310-2351: replace the CR3 XOR sentinel with a CR4.PGE-set sentinel.
* Update the multi-paragraph comment to describe the actual mechanism: SREGS
  with changed CR4 → `mmu_reset_needed=1` → `KVM_REQ_TLB_FLUSH_GUEST` →
  `vmx_flush_tlb_guest` → `vpid_sync_context` → INVVPID single-context.
* Cite the kernel code at `arch/x86/kvm/x86.c:12487-12488` and
  `arch/x86/kvm/x86.c:12529-12532` so the reasoning is verifiable.
* Propagate the first ioctl's return code instead of swallowing.

**Commit 2 — Fix producer-side memory ordering in shadow mutators**
(`arch/um/backend/kvm/shadow_sync.c`, `arch/um/backend/kvm/lifecycle.c`):

* `kvm_shadow_invalidate_va_range`: convert in-loop PTE clears to WRITE_ONCE,
  add smp_wmb before setting `dirty`, set `dirty` and `synced` via
  WRITE_ONCE.
* `kvm_shadow_map_page`: add smp_wmb between leaf write and `dirty=true`,
  WRITE_ONCE both. Also reset `shadow->synced=false` to match invalidate
  semantics, since map_page mutates the shadow.
* `kvm_shadow_sync_pte` needs_full_resync paths: order
  `needs_full_resync` write before `dirty` write with smp_wmb between.

**Commit 3 — Fix consumer-side memory ordering in kvm_enter_guest**
(`arch/um/backend/kvm/thread.c`):

* Lines 2150-2152: change `!READ_ONCE(shadow->needs_full_resync)` to use
  `smp_load_acquire` (or add an explicit smp_rmb before the read, with a
  comment).
* Lines 2235-2240: same treatment — `smp_load_acquire` on `shadow->dirty`,
  or smp_rmb before the chain of READ_ONCEs.

**Commit 4 (optional)** — fold KVM_GET_SREGS skip when the SREGS-skip
predicate misses but only because of `dirty` (cached_cr3/fs/gs all match):
the merge target hasn't changed, so we can reuse the cached `sregs` from
last successful program. Saves one ioctl per dirty entry.

### What to defer

* Option (d) per-page INVLPG via gadget — wait for profiling.
* Option (e) ring-based shadow event log — wait for direction-of-arch decision.
* Option (f) UAPI ioctl proposal — file as an upstream RFC.

### Acceptance test

For the keystone replacement, the read_test5 harness cited at thread.c:2319
should still go 8/8. Plus the cpython parity gate should hit ≥18/21 (the
keystone+memory-ordering fix should resolve at least one of the threading
modules; expect test_set.test_merge_and_mutate or test_struct first since
those have the highest mutation churn).

The flake on `import unittest` 8/10 should drop to ≥9/10 with commits 2-3
(it's plausibly the consumer-side smp_rmb gap manifesting).

---

## Appendix A — instruction-level cost notes

### A.1 INVVPID (single-context) cost

Per Agner Fog's tables and Intel optimization manual:
* Skylake INVVPID single-context: ~80 cycles + microcode dispatch.
* Total at 3.5 GHz: ~25 ns.

Per kvm_enter_guest, a full flush is a one-time cost that pays back the
moment the guest issues N TLB-missing accesses. N = # of pages in the hot
working set. For a CPython interpreter, N ~ 100-1000 pages → 100-1000 page
walks @ ~4 cycles each = 400-4000 cycles = 100ns-1µs.

So the keystone flush costs us ~1 µs of TLB re-warm per dirty entry. At
100 dirty entries/sec that's 100 µs/sec — under 0.01% of CPU. Not
material.

### A.2 KVM_SET_SREGS cost

Measured in the harness (kvm_perf KUnit at sub-100 µs target): single SREGS
write ~3-5 µs depending on what fields differ. The hack pays 2 SREGS per
dirty entry → ~10 µs total. Option (b) keeps the same 2 SREGS but with
cleaner failure semantics. Option (c) drops to 1 SREGS.

### A.3 INVLPG (single-page) cost

* Skylake: ~65 cycles. ~20 ns.

For per-page invalidation via Option (d)'s gadget, each gadget round-trip is
one KVM_RUN (~10 µs) + one INVLPG (~20 ns). Crossover with full flush is
~500 invalidations per kvm_enter_guest — i.e. only beneficial when bulk
invalidating would otherwise warm a huge TLB.

For typical UML workloads (mmap of a single .so, single page touch),
Option (d) costs more than Option (b) per invalidation. The INVLPG approach
only wins on heavy-churn workloads with cold TLBs.

---

## Appendix B — multi-task threading hypothesis

The remaining 4-5 cpython modules failing at the 17/21 gate are all
multi-task / threading scenarios. Hypothesis: they expose the smp_wmb /
smp_rmb gap documented in §6.

**Mechanism:**

1. Thread A (Python eval loop) mutates a shadow PTE via
   `kvm_shadow_sync_pte` → leaf write, smp_wmb, dirty=true.
2. SIGALRM fires (UML uses SIGALRM for cooperative scheduling). Thread A
   yields after the leaf write but before `dirty=true` is observable to
   Thread B (compiler-reordered or just in-flight on the store buffer with
   a hostile compiler).
3. Thread B (Python interpreter on a different vCPU thread, or same vCPU
   thread re-entering after handling the signal) reaches kvm_enter_guest
   line 2235. Reads `dirty` as false (stale). Cached CR3/FS/GS all match.
   SREGS-skip path taken.
4. KVM_RUN issues, no TLB flush, guest reads through stale TLB entry to
   the OLD PFN. Returns wrong bytes. Python sees "data corruption."

The keystone fix (CR3 toggle / CR4 toggle) doesn't help here because the
*decision* to issue SREGS at all is being short-circuited by the stale
read.

**Why this matters more under threading:** the SIGALRM frequency goes up
with thread count (each thread can spin a SIGALRM), so the race window
opens more often. Single-process workloads see the race only at the timer
boundary; multi-thread workloads see it continuously.

**Why import unittest flakes 8/10:** import-time module evaluation does
heavy mmap-then-read churn. The race is exposed once per several thousand
mutations; 2/10 boots get unlucky in their first few scheduler ticks.

If this hypothesis is correct, Commits 2-3 should clean up the residuals.
If it's not — i.e. the residuals persist after the memory-ordering fix — the
next investigation should look at:

* `kvm_shadow_pgd_clear_user` (lifecycle.c:1480) — only fires on context
  switch but if it races with concurrent shadow access from another vCPU
  thread, all bets are off.
* The `kvm_shadow_invalidate_va_range` mutex_guard (lifecycle.c:1511) —
  doesn't protect against the SREGS-skip lock-free path.
* Per-vCPU TLB tracking — under multi-vCPU configurations (unlikely for UML
  today but technically possible), each vCPU has its own VPID; the INVVPID
  in our SREGS path only flushes the active vCPU's VPID.

---

## End of document

Total length: this write-up.

Authoritative file references for the next reviewer:

* Hack site:                 `arch/um/backend/kvm/thread.c:2310-2351`
* Skip-cache predicate:      `arch/um/backend/kvm/thread.c:2235-2240`
* Producer (shadow install): `arch/um/backend/kvm/shadow_sync.c:274-298`
* Producer (shadow clear):   `arch/um/backend/kvm/shadow_sync.c:193-213`
* Producer (range invalidate): `arch/um/backend/kvm/lifecycle.c:1498-1564`
* Producer (map_page):       `arch/um/backend/kvm/lifecycle.c:1381-1443`
* In-kernel SREGS path:      `arch/x86/kvm/x86.c:12451-12544`
* In-kernel CR4 hook:        `arch/x86/kvm/x86.c:1331-1371`
* In-kernel CR3 hook:        `arch/x86/kvm/x86.c:1407-1497`
* Vmenter request drain:     `arch/x86/kvm/x86.c:11218-11233`
* VMX flush_tlb_*:           `arch/x86/kvm/vmx/vmx.c:3249-3347`
* SVM flush_tlb_*:           `arch/x86/kvm/svm/svm.c:4173, 4579`
* Sregs validity:            `arch/x86/kvm/x86.c:12426-12449`
* CR3 GPA legality:          `arch/x86/kvm/cpuid.h:77-86, 280-286`
