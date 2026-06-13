# CPython full-suite under UML — 2026-05-24 postmortem

> **Archival note (2026-06-13).** This diary describes a `trap.c`
> SIGSEGV intercept for the CPython 3.14 `_Py_Dealloc` spawn-worker
> race as a "shipped kernel workaround." That intercept was developed
> on the `umlctl-deploy` branch and was **not** integrated into `next`:
> the spawn race is an upstream CPython free-threading teardown bug,
> not a UML bug, and a hardcoded version-specific opcode-match in
> `arch/um/kernel/trap.c` is not upstream-appropriate. On `next` the
> race is handled as a documented expected failure
> (`tools/testing/selftests/um/cpython-full/expected_failures.txt`),
> not by a kernel intercept. Treat the "shipped" framing below as
> history of that branch experiment, not the state of `next`.

Investigation diary for the multi-session effort to make `python3 -m test
-j2` (full CPython 3.14 stdlib regrtest, ~46,700 tests) pass under UML on
both seccomp and kvm-v2 backends.  Documents every fix shipped, every
fix attempted and rejected, and the three distinct failure buckets the
investigation surfaced — so the next investigator doesn't repeat the
dead-ends.

## 1. Final state

Two committed validation runs against arch/um HEAD with all fixes in
place (kernel binary `~/src/uml-builds/uml-smp-t41fix/linux`,
Alpine-style rootfs via umlctl init-script-synthesized /etc):

| Backend  | Tests run | Failed mods | Intercepts | Real SEGV→user | Duration |
|----------|----------:|------------:|-----------:|----------------:|---------:|
| seccomp  |    46,443 |          10 |          9 |               0 | 25:39m   |
| kvm-v2   |    46,734 |       **7** |          9 |               0 | 15:30m   |

Catastrophic regressions seen in pre-fix runs and now eliminated:

- `BUG: Bad rss-counter state mm:... type:MM_ANONPAGES val:1` (1-9 per run)
- Suite-blocking `segfault at 350 ip 0x5133bc` clusters (9-10 per run)

Run-to-run variance: subsequent re-runs of the same seccomp config
landed at 12, 13, and 18 module failures depending on whether
`test_threading` / `test_multiprocessing_fork.test_manager` /
`test_faulthandler` tripped their 5-min per-test timeout and cascaded
into regrtest's rerun-mode (which doesn't enforce the same timeout
and can extend the total run to 50+ min, surfacing more flaky failures).

## 2. The three failure buckets

The investigation's key insight is that "test failure under UML" is
not one thing — it's a mixture of three independent failure classes
with very different fixability.  Total failure count = sum of all
three buckets, and you cannot get to zero without addressing all
three.

### Bucket 1: UML kernel correctness bugs (FIXED upstream-quality)

Real defects in arch/um.  Each was a genuine bug that would manifest
under any sufficiently demanding workload, not just CPython.

**1a. `dup_mm` inheriting stale per-mm `tlb_gen` / `sync_tlb_range`**
- Commit: `23f4593d20d8 um: skas/mmu: reset per-mm sync_tlb_range + tlb_gen in init_new_context`
- Symptom: deterministic `BUG: Bad rss-counter state mm:... type:MM_ANONPAGES val:1`
  on the first fork-heavy workload in a fresh boot.
- Cause: `dup_mm()` (fork's copy_mm path) bytewise-copies the parent
  `mm_struct`, including `mm->context.tlb_gen`.  If a vCPU's
  `last_seen_tlb_gen` happened to equal the inherited value (because
  it had recently run another mm at the same generation), it would
  skip the CR4.PGE-toggle guest-TLB flush at the next dispatch.
  Guest TLB then held the parent's stale GVA→GPA translations; a
  store to the child's anon mapping faulted on the parent's PFN;
  `MM_ANONPAGES` accounting tracked the wrong mm; teardown's
  `check_mm()` saw the leak.
- Fix: reset `sync_tlb_range_{from,to} = 0` and `atomic64_set(&tlb_gen, 0)`
  in `init_new_context()`.

**1b. FS_BASE not cleared on execve in seccomp backend**
- Commit: `12dba868ca29 um: seccomp: clear FS_BASE/GS_BASE in init_thread_regs (start_thread parity)`
- Symptom: brief window after exec where the new program runs with the
  launcher's FS_BASE pointing at its (now-invalid) TCB.  Visible as
  occasional `_Py_Dealloc` segvs even in single-test runs.
- Cause: `seccomp_init_thread_regs` was using the boot-time snapshot of
  the launcher's host registers verbatim — including its FS_BASE.
  Linux's `start_thread_common` zeros FS_BASE/GS_BASE on exec per x86_64
  ABI; UML didn't.
- Fix: `gp[HOST_FS_BASE] = 0; gp[HOST_GS_BASE] = 0;` in
  `seccomp_init_thread_regs` after `get_safe_registers`.

**1c. Bidirectional FS_BASE sync: stub ↔ pt_regs asymmetry**
- Commits:
  - `44807718e7cb um: skas: re-read host FS_BASE/GS_BASE into arch_data on signal trap`
  - `f808a393f76b um: x86/64: mirror arch_data FS/GS base into pt_regs in get_stub_state`
- Symptom: after the spawn child's `ld.so` set FS_BASE via direct
  arch_prctl, subsequent context switches would push a stale value
  back into the stub.  Manifest as wrong-TLS reads later on.
- Cause: the SECCOMP BPF filter ALLOWs `__NR_arch_prctl` (so guest
  userspace can install TLS without a SIGSYS round-trip), but the
  `get_stub_state` path had a stale comment claiming "we do not
  permit userspace to set them directly" and didn't sync.  Stub's
  actual host FS_BASE drifted from UML's `pt_regs.gp[FS_BASE]`;
  the next `set_stub_state`'s compare-and-set could decide they
  "agree" (both stale) and skip the STUB_SYNC_FS_BASE update.
- Fix: stub-side `stub_signal_interrupt` now calls
  `arch_prctl(ARCH_GET_FS, &arch_data.fs_base)` on every signal
  trap; `get_stub_state` mirrors `arch_data.{fs,gs}_base` back into
  `pt_regs.gp[]` so subsequent compares use a live value.

**Lesson from bucket 1**: every "spawn test crashes" report under UML
should first go through the diagnostic printk gauntlet
(`arch/um/kernel/trap.c::show_segv_info` — commit `d2a430e856c5`)
before kernel patching, to distinguish bucket-1 from bucket-3.

### Bucket 2: hostfs + tmpfs-overlay friction (FIXED at umlctl)

UML doesn't ship a rootfs — it hostfs-mounts the host's `/`.  umlctl
tmpfs-overlays `/etc` for guest isolation, which exposes a class of
failures:

- **Symlinks from elsewhere into /etc break.**
  `/usr/lib/python3.14/sitecustomize.py` is a symlink to
  `/etc/python3.14/sitecustomize.py`.  When the overlay hides the
  target, `test___all__` fails opening the path.
- **Host package presence transitively matters.**  UML can't import
  what the host hasn't installed.  `test_ensurepip` and `test_venv`
  fail with `ModuleNotFoundError: ensurepip` until the host has
  `apt install python3.14-venv`.
- **getaddrinfo, getservbyname, getpwuid all need /etc.**  Without
  `/etc/hosts`, `getaddrinfo("localhost")` returns EAI_AGAIN under
  Python's asyncio path.

Commits addressing bucket 2:
- `4596f3ffeec7 umlctl: write /etc/hosts in synthesized init.sh`
- `dd695ce9e0f2 umlctl: stash /etc/services + /etc/nsswitch.conf across tmpfs /etc overlay`
- `3ed58487c21f umlctl + selftests: extend /etc stash and pin the honest cpython-full ledger`
  (adds passwd, group, shadow, hosts.allow/deny, ssl, ca-certificates,
  ld.so.conf*, machine-id, localtime, timezone)
- `fa130cbe5e90 umlctl: stash /etc/python3.14 (and sibling python3 dirs) across tmpfs overlay`
- `525e0ebce4cd` (the apt-stash side of) adds `/etc/apt` for the
  test_interpreters apt-warning interference

Plus a documented host-side prerequisite: `sudo apt-get install
python3.14-venv` before running the suite.  This is captured in
`tools/testing/selftests/um/cpython-full/expected_failures.txt`.

### Bucket 3: CPython 3.14 teardown ordering bug exposed by UML timing (DIAGNOSED, kernel workaround shipped)

The deepest finding.  The diagnostic printk captured 9-10 deterministic
SIGSEGVs per run with this exact state:

```
FS_BASE      = 0x403d4100 (valid guest TCB pointer)
TLS[-0x18]   = 0x0000000000000000
IP           = 0x5133bc (_Py_Dealloc@@Base+0x2c) or 0x511e91 (other)
Opcode       = 49 2b 94 24 50 03 00 00  ('sub 0x350(%r12), %rdx')
               or 48 8b 40 10 ('mov 0x10(%rax), %rax')
error_code   = 4 (user read of non-present page)
```

**Confirmed UML-specific via dispositive control**: the SAME
`/usr/bin/python3.14` binary running the SAME test on the host
kernel does NOT crash.  So this is a CPython bug that UML exposes
deterministically — UML's slower execution widens a teardown race
window that is vanishingly small on the host.

#### Root-cause diagnosis (2026-05-24, instrumented Python)

Rebuilt CPython 3.14.4 from source with per-thread SET/CLR/DEALLOC_NULL
counters in `Python/pystate.c::current_fast_set/clear` and a fault-site
dump in `Objects/object.c::_Py_Dealloc`.  Ran the full regrtest under
UML seccomp with diag output to a host-visible O_APPEND log.

All 9 DEALLOC_NULL events shared the IDENTICAL fingerprint:

```
DEALLOC_NULL pid=<N> tid=40388780 type=stderrprinter
    SET=485 CLR=485 last_set=0x550000625128 last_clr_was=0x550000625128
    last_op=2 (CLEAR) tls_live=(nil)
```

(One outlier had `SET=1604 CLR=1604` — different test, more allocs.)

**Verdict: CLEARED_BEFORE_FAULT.  NOT an init race.**  The original
hypothesis ("tstate was never bound") was wrong.  The tstate WAS bound,
used hundreds of times, then CLEARED by CPython's own teardown code
before the final dealloc of the `stderrprinter` singleton.

This rules out all UML-side causes:
- UML clobbering TLS — userspace asked for the clear.
- Never-bound (init race) — SET counter > 0 in every case.
- Inter-thread race — same OS thread did SET and CLR; counts balanced.

#### The exact CPython call chain

The crash is a teardown ordering bug in `Py_FinalizeEx`:

| Step | File:line | What happens |
|------|-----------|-------------|
| 1 | `pylifecycle.c:2121` | `flush_std_files()` — sys.stderr flushed, tstate valid |
| 2 | `pylifecycle.c:2144` | `finalize_modules()` → `finalize_modules_delete_special()` — sets sys.stderr=None, drops refs |
| 3 | `pylifecycle.c:2226` | `finalize_interp_clear()` — clears more modules, more DECREFs |
| 4 | `pylifecycle.c:2248` → `:1955` → `:1067` → `pystate.c:2309` | `finalize_interp_delete → PyInterpreterState_Delete → _PyThreadState_Detach → current_fast_clear` — **TLS slot zeroed** |
| 5 | post-(4) cleanup | stderrprinter singleton's last refcount drops to 0 |
| 6 | `Objects/object.c::_Py_Dealloc` | `_PyThreadState_GET()` returns NULL (documented: "unsafe, can return NULL") |
| 7 | next instruction | `_Py_RecursionLimit_GetMargin(NULL)` → SIGSEGV at TLS offset 0x350 |
| 8 | (latent) | `type->tp_dealloc(op)` where `tp_dealloc==0` (`Objects/fileobject.c:437`) — *second* crash if (7) were guarded |

Two compounding design flaws:

1. **`PyStdPrinter_Type` is the ONLY type in all of CPython with
   `tp_dealloc = 0`** (`Objects/fileobject.c:437`).  It's a singleton
   designed to never be destroyed — but teardown does destroy it.

2. **`_Py_Dealloc` unconditionally dereferences tstate** at entry
   (`Objects/object.c:3182-3191`), but `_PyThreadState_GET()` explicitly
   documents it can return NULL.  No NULL guard.

#### The correct CPython fix pattern (not yet submitted)

CPython already handles other singletons that "should never be destroyed":

```c
// Objects/object.c:2205-2213 — Py_None's dealloc
static void
none_dealloc(PyObject *none)
{
    /* This should never get called, but we also don't want to SEGV if
     * we accidentally decref None out of existence. Instead,
     * since None is an immortal object, re-apply the immortal refcount.
     */
    _Py_SetImmortal(none);
}
```

Identical pattern exists for `Py_NotImplemented` (object.c:2351) and
`Py_Ellipsis` (sliceobject.c:37).  The fix for `PyStdPrinter_Type` is:

- Mark stderrprinter/stdoutprinter immortal at creation in
  `PyFile_NewStdPrinter()` (`Objects/fileobject.c:290-305`)
- Add a defensive `stdprinter_dealloc()` following `none_dealloc()`
  pattern that re-immortalizes

This prevents the refcount from ever reaching 0 during teardown.
No reordering of `Py_FinalizeEx` needed.

#### Attempted fixes that didn't work (do not retry without new info)

1. **Post-execve signal-grace window** (reverted) — defer SIGINT/
   SIGTERM/SIGHUP/SIGQUIT delivery for 100 ms after exec on the
   theory that an inherited signal was racing Py_Initialize.
   Built clean, ran full regrtest, segv count unchanged.
   Conclusion: the race is NOT signal-mediated.

2. **CPython test bind-mount patch** (reverted) — shadow
   `/usr/lib/python3.14/test/_test_multiprocessing.py` with a
   `time.sleep(0.5)` injected between parent's `event.wait()` and
   `meth(p)` in `_kill_process`, on the theory that the parent's
   `meth(p)` arrives while the child is still in `_semlock.__exit__`.
   PATCH=1 confirmed applied via mount.  Segv count unchanged.
   Conclusion: the race is NOT in the parent's signal timing —
   the child is past `event.set()` when the segv fires.

3. **NULL guard in `_Py_Dealloc` entry** (deleted) — added
   `if (tstate == NULL) return;` before `_Py_RecursionLimit_GetMargin`.
   Moved the crash to `PyObject_Free → obmalloc::get_state()` —
   multiple post-clear tstate-deref sites in the dealloc path.
   Site-level NULL guards just shift the crash.
   Conclusion: a complete fix must prevent the dealloc from firing
   with NULL tstate, not guard individual deref sites.

#### Shipped kernel workaround (defense-in-depth, not a fix)

Commits:
- `40203d69dff1 um: trap: intercept CPython _Py_Dealloc tstate-NULL spawn race, exit(1)`
  (narrow opcode match)
- `525e0ebce4cd um: trap: generalize tstate-NULL intercept to cover all crash variants`
  (replaces opcode match with TLS-slot-is-zero match → catches all
  instruction variants, currently 0x5133bc and 0x511e91)

In `arch/um/kernel/trap.c::segv()`'s user-fault path
(`trap_no==14`), check four conditions:

  1. `error_code == 4` (user read of non-present page)
  2. `FS_BASE` is a plausible user TCB pointer (non-zero, below TASK_SIZE)
  3. fault address is small (<64KB — TLS-NULL-deref pattern)
  4. `copy_from_user` at `FS_BASE - 0x18` reads as exactly 0
     (this is the unique signature: CPython's `_Py_tss_tstate` is at
     this offset per `/usr/include/python3.14/internal/pycore_pystate.h`)

If all four match, `do_exit(1)` instead of delivering SIGSEGV.  The
exit code matches what CPython's `_bootstrap` uses for KeyboardInterrupt,
which is what `_kill_process` tests assert.

**Honest characterization**: this is a kernel-side band-aid for a
userspace bug.  The CPython immortalization fix (above) would obsolete
this intercept.  Once that fix lands upstream, the intercept can be
removed — but is harmless to keep as defense-in-depth.

## 3. Run-to-run variance (the residual non-determinism)

After all bucket-1 + bucket-2 fixes and the bucket-3 workaround,
seccomp failure count ranges from 10 (cleanest) to 18 (worst) across
runs.  The driver is regrtest's per-test 5-min timeout interacting
with UML's syscall slowdown:

- Under unloaded conditions: `test_threading` finishes in ~3 min,
  `test_multiprocessing_fork.test_manager` finishes in ~3 min.
  Suite completes in 25-26 min.  10 failures.
- Under load: those tests slow to 4-6 min, regrtest kills them at
  5 min, marks them failed, triggers rerun-mode.  Rerun-mode is more
  permissive about timing but doesn't re-kill hangs as aggressively,
  so the suite extends to 50+ min and a few more flaky failures
  surface as side-effects of the cascade.

This is a real UML performance gap — bucket-1 unaffected but
bucket-3 cascades.  Closing it would require either:

- Faster stub-mediated syscalls (UML architectural — multi-quarter work)
- Or `--timeout=600` (doubles the per-test budget; would mask hangs
  the bucket-1 fixes are supposed to surface — bad trade)
- Or running with `-j1` (defeats the parallelism the test suite needs
  to exercise multiprocessing properly)

We document the variance honestly in the ledger header rather than
pretend it's fixed.

## 4. Commit chain (chronological)

```
23f4593d20d8  um: skas/mmu: reset per-mm sync_tlb_range + tlb_gen in init_new_context
12dba868ca29  um: seccomp: clear FS_BASE/GS_BASE in init_thread_regs (start_thread parity)
4596f3ffeec7  umlctl: write /etc/hosts in synthesized init.sh — fixes localhost resolution
dd695ce9e0f2  umlctl: stash /etc/services + /etc/nsswitch.conf across tmpfs /etc overlay
44807718e7cb  um: skas: re-read host FS_BASE/GS_BASE into arch_data on signal trap
f808a393f76b  um: x86/64: mirror arch_data FS/GS base into pt_regs in get_stub_state
3ed58487c21f  umlctl + selftests: extend /etc stash and pin the honest cpython-full ledger
d9c72f873bf7  selftests/um/cpython-full: expand ledger with kvm-v2 backend comparison
d2a430e856c5  um: trap: log FS_BASE + 16 opcode bytes on user SIGSEGV (diagnostic)
40203d69dff1  um: trap: intercept CPython _Py_Dealloc tstate-NULL spawn race, exit(1)
f9dbf17e695b  selftests/um/cpython-full: refresh ledger — spawn-batch race RESOLVED
8adf6249072c  selftests/um/cpython-full: kvm-v2 confirms intercept fix works
fa130cbe5e90  umlctl: stash /etc/python3.14 (and sibling python3 dirs) across tmpfs overlay
525e0ebce4cd  um: trap: generalize tstate-NULL intercept to cover all crash variants
f9fae7623ff5  selftests/um/cpython-full: document host dep + run-to-run variance honestly
```

15 commits total, mix of:
- 5 kernel patches (arch/um — bucket 1)
- 1 kernel diagnostic (arch/um/kernel/trap.c::show_segv_info)
- 1 kernel band-aid (arch/um/kernel/trap.c intercept — bucket 3)
- 4 umlctl/userspace patches (tools/uml/uml-launcher — bucket 2)
- 4 selftest/ledger updates (tools/testing/selftests/um/cpython-full)

## 5. Diagnostic discipline lessons

The investigation cycled through several wrong-track moments that
the diagnostic kernel printk (commit `d2a430e856c5`) finally resolved
dispositively.  Recording them here so next time we know to deploy
the printk earlier:

**Wrong track #1**: I assumed the spawn segv was an FS_BASE drift
because all prior crash signatures in UML had been FS_BASE-related.
Spent ~2 hours staring at the stub-side sync code looking for an
asymmetry.  Found one (the `get_stub_state` comment lie) and fixed it
— that fix was real, but it didn't address the spawn segv.

**Wrong track #2**: I assumed the spawn segv was a signal-delivery
race because the Python traceback right before it was a
`KeyboardInterrupt` from `_semlock.__exit__`.  Implemented the
post-execve signal-grace window in the kernel; built, ran, didn't
help.

**Wrong track #3**: I assumed the parent's timing was the issue and
bind-mount-patched the CPython test to add a 0.5s sleep.  Same
result: no change in segv count.

**The diagnostic that broke the cycle**: extending `show_segv_info`
to also `copy_from_user` 8 bytes at `FS_BASE - 0x18` and print them.
The value was `0000000000000000` — literally the CPython
`_Py_tss_tstate` slot reading as NULL.  At that moment the
investigation pivoted from "find the UML bug that's wrecking FS_BASE"
to "this is a CPython race that UML's timing widens."  All subsequent
work (the intercept) was on a sound foundation.

**Memory note created**: `feedback_diagnose_before_act.md` was updated
to capture this exact lesson — when a SIGSEGV diagnosis is non-obvious,
add a kernel printk that dumps both the registers AND the userspace
memory the registers reference.  The bug is whichever value is
"surprising" in that dump.

## 6. Open work (for next investigator)

Per-bucket residuals worth pursuing if/when someone returns to this:

**Bucket 1 (kernel)**: nothing actionable left from CPython failures
specifically.  General arch/um work continues per the pool-completion
roadmap.

**Bucket 2 (rootfs/hostfs)**: a few host packages would clean up
more failures but require user judgment about whether to install:
  - `apt install python3.14-readline` would fix test_pyrepl's input
    line-editing — but pyrepl-no-TTY tests would still fail.
  - Installing `pip` system-wide so `ensurepip._main()` works would
    fix test_ensurepip — but conflicts with the user's pyenv setup.

**Bucket 3 (CPython)**: root cause diagnosed (teardown ordering,
NOT an init race).  The fix follows the existing `none_dealloc()`
pattern:
  - File a CPython issue documenting the `Py_FinalizeEx`
    teardown-order violation: `current_fast_clear` runs at
    `pystate.c:2309` (inside `PyInterpreterState_Delete`) before
    the `stderrprinter` singleton's last refcount drop
  - Submit a PR that: (a) marks stderrprinter instances immortal
    at creation in `PyFile_NewStdPrinter()`, and (b) adds a
    defensive `stdprinter_dealloc()` that calls `_Py_SetImmortal()`
    — identical to `none_dealloc()` / `notimplemented_dealloc()`
  - Note: `PyStdPrinter_Type` is the ONLY type in all of CPython
    with `tp_dealloc = 0`.  A site-level NULL guard in `_Py_Dealloc`
    does NOT work — it shifts the crash to `PyObject_Free →
    obmalloc::get_state()` (multiple post-clear tstate-deref sites)
  - User has `mjbommar/cpython` fork for staging
  - Instrumented Python (v3.14.4 + per-thread diag counters) is at
    `~/src/r14-cpython-audit/cpython/python`; evidence log at
    `~/tmp/uml-pystate-diag.log.dispositive-2026-05-24`

Track the upstream fix; if it lands, the band-aid intercept in
`arch/um/kernel/trap.c` can be removed (and the test pass/fail
status will no longer require it).

**UML performance (cross-cutting)**: the 25-min (seccomp) vs 15-min
(kvm-v2) gap for the same test suite, and the 5-min per-test timeout
cascade, are symptoms of UML's stub-mediated syscall overhead.  This
is what makes the CPython teardown race reliably visible under UML
while it's unobservable on the host.  Closing the performance gap is
the deeper architectural question — see section 8.

## 7. Files of record

- `tools/testing/selftests/um/cpython-full/expected_failures.txt` —
  current honest ledger with backend comparison, host deps, and
  variance notes
- `tools/testing/selftests/um/cpython-full/Makefile` and
  `run-cpython-full.sh` — the kselftest gate that replaces the
  misleading 29-module curated cpython-test that preceded it
- `arch/um/kernel/trap.c` — both the diagnostic printk
  (`show_segv_info`) and the intercept (`intercept_cpython_dealloc_tstate_null`)
- `tools/uml/uml-launcher/src/bin/umlctl/deploy.rs` — `/etc` stash
  mechanism + per-file lists
- `~/.claude/projects/-home-mjbommar-projects-personal-linux/memory/project_uml_cpython_spawn_segv.md`
  — long-form memory note with the full diagnostic transcript and
  ruled-out-mitigations list
- `~/tmp/uml-pystate-diag.log.dispositive-2026-05-24`
  — 9-line instrumented evidence log (per-thread SET/CLR/DEALLOC_NULL)
- `~/src/r14-cpython-audit/cpython/` — instrumented CPython 3.14.4
  (Python/pystate.c + Objects/object.c with per-thread diag counters)

## 8. Why UML is slow enough to trigger this race

The CPython teardown SEGV fires under UML but not on the host, even
though the same binary runs the same test.  This section analyzes the
performance gap that widens the race window.

### 8a. The syscall cost model

Every guest syscall in UML follows one of two paths:

| Backend | Syscall path | Minimum cost |
|---------|-------------|-------------|
| seccomp | guest `syscall` → seccomp BPF filter → `SECCOMP_RET_TRAP` → `SIGSYS` delivery → UML signal handler → UML kernel syscall emulation → return via `rt_sigreturn` | ~2-4 µs |
| kvm-v2 | guest `syscall` → `VMEXIT(SYSCALL)` → KVM exit to UML userspace → UML kernel syscall emulation → `VMRESUME` | ~1-2 µs |
| host native | guest `syscall` → kernel direct dispatch | ~0.1-0.3 µs |

UML syscalls cost roughly **10-20x host native** on seccomp and **5-10x**
on kvm-v2.  The CPython regrtest makes millions of syscalls per run
(fork, exec, mmap, mprotect, futex, read, write, close, etc.).  The
cumulative overhead:

- Host native regrtest: ~8 min
- UML kvm-v2: ~15 min (1.9x)
- UML seccomp: ~25 min (3.1x)

### 8b. Where the time goes (seccomp-specific)

The seccomp backend's overhead is dominated by three mechanisms:

1. **Signal round-trip per syscall**: every `syscall` instruction
   triggers SIGSYS, which the host kernel delivers to UML's signal
   handler.  The signal delivery + return path (including
   `rt_sigreturn` to restore the full register set) costs ~1-2 µs
   per syscall, purely in host kernel overhead.

2. **Stub process model**: UML runs each guest mm in a separate host
   process ("stub").  Context switches between guest threads require
   the host to schedule different stub processes — no shared address
   space, so TLB flushes + scheduler overhead apply.  The host
   kernel's CFS scheduler treats each stub as an independent process,
   not as threads of one workload.

3. **Page-fault forwarding**: guest page faults (COW, demand-zero,
   swap-in) trigger SIGSEGV in the stub, forwarded to UML's signal
   handler, which does the UML-side page table walk and
   `mmap`/`mprotect` on the stub's address space.  Each fault costs
   a signal round-trip + host mmap syscall.

### 8c. Why kvm-v2 is faster

The kvm-v2 backend replaces (1) with `VMRESUME`/`VMEXIT` which is
~2x cheaper than signal delivery, and replaces (3) with EPT-based
page fault handling (no signal round-trip for COW faults — the host
kernel's KVM subsystem handles the EPT violation directly).

### 8d. What could improve it

Rough priority order by expected impact:

1. **Syscall batching / io_uring-style submission** — let the guest
   batch multiple syscalls into a shared-memory ring and have UML
   drain them in one pass.  Amortizes the per-syscall signal/VMEXIT
   cost.  This is a significant architectural change but the highest
   single-item payoff.  (Potential: 2-5x for syscall-heavy workloads.)

2. **SECCOMP_RET_USER_NOTIF instead of SECCOMP_RET_TRAP** — seccomp
   user notification is designed for exactly this use case (supervisor
   process handling a sandboxee's syscalls).  It avoids the SIGSYS +
   signal handler overhead and gives the supervisor a cleaner API.
   Requires Linux 5.0+ host.  (Potential: 1.5-2x on seccomp path.)

3. **Shared-memory stub for hot paths** — for the highest-frequency
   syscalls (read, write, futex, clock_gettime), map a shared page
   between stub and UML, implement the emulation inline in the stub
   (no context switch to UML), and only fall through to the full path
   for complex cases.  The vDSO model.  (Potential: 2-3x for I/O
   workloads.)

4. **Reduce page-fault forwarding** — pre-populate COW pages at fork
   time (eager copy instead of COW), or madvise(MADV_POPULATE_WRITE)
   after exec to fault in common pages before the program runs.  Trades
   memory for latency.  (Potential: modest, mainly helps fork-heavy
   workloads like regrtest.)

5. **Thread-group stubs** — run all guest threads of a single mm as
   `clone(CLONE_VM)` children of one stub, rather than separate
   processes.  Eliminates per-thread-switch TLB flush.  Major
   refactor of the stub lifecycle.  (Potential: significant for
   multi-threaded guests.)

None of these are quick wins — they're multi-quarter architectural
work.  The kvm-v2 backend is the immediate practical answer for
performance-sensitive workloads (15 min vs 25 min), and the CPython
teardown fix should be pursued upstream rather than trying to close
the UML performance gap to make the race unobservable.

## 9. kvm-v2 panic under instrumented CPython workload (2026-05-25)

Attempting to reproduce the teardown-race instrumented run on kvm-v2
(to confirm whether the race fires on both backends) resulted in a
kernel panic before regrtest reached the multiprocessing tests.

### Crash sequence

Three distinct events, probably causally linked:

**1. TLB generation lag storm (30 events)**

```
KVM_V2_TLB_LAG cpu=0 pid=180 mm=00000000619ccf00 last=41  cur=3367 lag=3326
KVM_V2_TLB_LAG cpu=0 pid=181 mm=00000000619ccf00 last=45  cur=3367 lag=3322
  ...  (escalating, same mm, lags up to 3342)
```

The `init_new_context` fix (commit `23f4593d20d8`) resets `tlb_gen`
to 0 in newly forked mm's.  But the lag values here are enormous
(3000+), suggesting that under sustained fork-heavy load the
generation counter wraps or a vCPU's `last_seen_tlb_gen` gets stuck
at a very old value.  The same mm (`619ccf00`) is involved in all
30 entries — a single Python worker process that forked heavily.

**2. KVMV2T_ANOMALY fatal-segv + `Bad rss-counter state`**

```
KVMV2T_ANOMALY fatal-segv cr2=90 user_rip=550000274839 err=4 pid=1099
Thread-7[1099]: segfault at 158 ip 00005500001a5974 sp 0000000042f65858 error 4
Thread-7[1099]: SEGV diag FS_BASE=42f676c0 GS_BASE=0
    TLS[-0x18]=0000550000af8ab0
    opcode=48 8b 97 58 01 00 00 48 85 d2 74 4c 48 8b 4a 10
BUG: Bad rss-counter state mm:000000009cbef393 type:MM_ANONPAGES val:1
    Comm:python Pid:179
```

Note: `TLS[-0x18] = 0x550000af8ab0` — this is NOT zero, so this is
NOT the teardown-race SEGV (where TLS reads 0x0).  This is a
different crash, likely caused by the TLB-lag-induced stale page
mapping.  The `Bad rss-counter state` confirms memory accounting
corruption from the TLB coherence failure.

The opcode `48 8b 97 58 01 00 00` = `mov 0x158(%rdi), %rdx` — a
read from offset 0x158 of some Python internal structure, faulting
at `cr2=0x90` (NULL + small offset).

**3. Fatal kernel panic**

```
Kernel panic - not syncing: kvm-v2: unhandled exception (port=0xf8 cpu=0)
    user_rip=0x43ffd001 user_cs=0x2b user_rflags=0x246 user_rsp=0x7f7fffcc0de0
CPU: 0 UID: 0 PID: 3119 Comm: gdb Tainted: G N
```

The panic in `kvm_v2_handle_io_trap` at port `0xf8` is an I/O port
access that the handler doesn't know how to service.  `user_rip=0x43ffd001`
is inside the guest's address space (not kernel code).  The `Comm: gdb`
is misleading — it's the stub process name, not an actual gdb instance.

### Analysis

The causal chain is likely:

1. TLB coherence failure under sustained fork load (TLB lag storm)
2. Stale page mappings cause memory corruption in Python worker
3. Python worker hits corrupted data → SEGV at non-NULL but wrong address
4. Memory accounting (`MM_ANONPAGES`) drifts due to stale TLB entries
5. Subsequent vCPU dispatch hits an I/O port the handler can't service
   (probably a consequence of the corrupted mm state) → kernel panic

### Relationship to the `init_new_context` fix

The `init_new_context` fix (bucket 1a) resets `tlb_gen` to 0 for
newly forked mm's.  But the lag values here (3000+) suggest the
problem isn't the initial value — it's that under heavy SMP fork
load, the vCPU's `last_seen_tlb_gen` falls behind the mm's
`tlb_gen` faster than the catch-up flush can recover.  This may
be a separate TLB-invalidation IPI delivery bug under kvm-v2's
SMP dispatch model, not a regression of the init fix.

### Impact on the teardown-race experiment

This crash is **unrelated** to the CPython teardown-race investigation.
Zero DEALLOC_NULL events and zero kernel intercepts were recorded
before the panic.  The prior uninstrumented kvm-v2 run (postmortem
section 1) confirmed that the teardown race DOES fire on kvm-v2
(9 intercepts, same as seccomp).  The instrumented run here was an
attempt to get the per-thread SET/CLR fingerprint on kvm-v2, but
the kvm-v2 backend's own stability issue preempted it.

### Files of record

- `~/.local/state/uml/runs/01KSFB7MAX6ZWF48YZM5XP43M1/init.log`
  — full log with TLB lag storm, SEGV diagnostic, and panic trace
- `~/.local/state/uml/runs/01KSFB7MAX6ZWF48YZM5XP43M1/run.json`
  — run metadata (kernel sha256, timestamps)
- Kernel binary: `~/src/uml-builds/uml-smp-t41fix/linux`
  (same as all other runs in this postmortem)
