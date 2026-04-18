# RECON.1 audit — stale-vs-real in redesign/01-architecture/

Date: 2026-04-18
Scope: the three "real deviation" items from review-01 + the
cross-document propagation they require.

## Files touched in this pass

| File | Stale items | Verdict |
|---|---|---|
| `01-architecture/three-layers.md` | 5 blocks (args struct, ops struct, selection text, contract bullets, hook table) | edit |
| `01-architecture/README.md` | 1 item (Layer 1 diagram box lists old op names) | edit |
| `01-architecture/data-flow.md` | 5 `syscall_dispatch` references across 4 profile walkthroughs | edit |
| `02-workstreams/README.md` | 1 prose reference to `syscall_dispatch` | edit (1-line sweep) |
| `02-workstreams/A-backend-abstraction/02-ptrace-refactor.md` | "Approach" numbered list uses `syscall_dispatch` etc. | **LEAVE** — task spec is historical; workflow executed against it |
| `05-validation/benchmarks.md` | One `syscall_dispatch` reference | **LEAVE** — future spec, shorthand readers can translate |

## What stays (explicitly)

- The three-layer framework itself: Layer 1 (backend ops) + Layer 2
  (static-key gates) + Layer 3 (compile-time wraps). This is the
  architectural vision the project rallies around; the sketches got
  stale, the framework didn't.
- Layer 2 / Layer 3 narrative — those are future work (workstreams
  B and C); the prose describes the design intent correctly.
- `02-workstreams/A-backend-abstraction/02-ptrace-refactor.md`
  "Approach" list — rewriting task specs post-hoc rewrites history.
  The updated status table at the top captures what actually happened.
- `03-profiles/` / `04-risks/` / `05-validation/` / `06-sequencing/`
  / `07-references/` — no stale-contract references found; these
  docs talk about profiles, risks, and future work at a level that
  doesn't reference op names.

## Stale op-name mapping (reference for the edit passes)

Used throughout the edit:

| Old (sketched in pre-implementation docs) | Current (delivered in A-01.3 / A-03.S1) |
|---|---|
| `syscall_dispatch(regs)` | `run_userspace(regs)` |
| `page_fault(addr, write, exec)` | folded into `run_userspace` (faultinfo on regs) |
| `map_user(mm, va, pa, len, prot)` | `mm_map(mm_id, va, len, prot, fd, offset)` |
| `unmap_user(mm, va, len)` | `mm_unmap(mm_id, va, len)` |
| `host_io_submit(req)` | not implemented (virtio-uml uses os_* directly; reserved for future) |
| args: `want_seccomp`, `want_kvm`, `force_uniform`, `runtime_opts` | args: `requested` (kind enum), `force` (bool), `runtime_opts` |
| "stub -ENOSYS where not meaningful" | "all ops populated; stubs return -EOPNOTSUPP for int ops" (D14) |
| "probe in order: KVM → seccomp → ptrace" | Kconfig-chosen in *_ONLY; `using_seccomp` in DYNAMIC with `backend=` param override (D12/D15) |

## Out-of-scope for this pass

- Stubs/lifecycle/mm-id/header-split/auto-semantics/perf — already
  captured in D11/D12/D13/D14/D15 and the review-01 response (next
  task in RECON).
- Workstreams B/C/D docs — genuinely future work; nothing stale.
- Task specs' "Approach" / "Goal" text — historical.
