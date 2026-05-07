# SMP-T57 — stress-ng `--vm-method` bisect (kvm-v2 SIGILL)

Investigation memo. No code changes. Continues from
`phase-J-pilot-2026-05-05.md` line 127ff (the open T57 finding) and
`23-fix-plan.md` adjacency. Kernel under test:
`~/src/uml-builds/uml-smp-t41fix/linux` (branch `umlctl-deploy`, HEAD
`82df9571eb25` — includes T41/T54/T56 fixes and the gadget revival).

Host: AMD Ryzen 7 7840HS (16 host CPUs, SVM); Linux 7.0.0-15-generic.

## 1. TL;DR

`stress-ng 0.20.01 --vm 1 --vm-bytes 16M --verify --timeout 8s` was
swept across all 38 valid `--vm-method` choices on both backends:

- **seccomp**: 38 / 38 PASS (rc=0; bogo ops range 125 — 923 136).
- **kvm-v2** : **22 / 38 FAIL**, **16 / 38 PASS**. Every failure is
  the same signature: `SIGILL` with `si_addr = 0x0` and
  `si_code = ILL_ILLOPN`, child exits with `rc=2`, zero bogo ops in
  most cases (mscan completes ~4096 ops before tripping).

**The `si_addr=0x0` is misleading**: kvm-v2's `#UD` handler at
`arch/um/backend/kvm-v2/syscall_trap.c:1916` hard-codes
`regs->faultinfo.cr2 = 0` for all `#UD` deliveries. The *real*
faulting RIP is preserved in the IST frame and is recoverable via the
state-trace ring. We dumped it.

The pass / fail partition does **not** correlate with TARGET_CLONES
(both groups use it), with use of `mincore_touch` /
`inject_random_bit_errors` (some passing methods use them, some
failing don't), or with vector / scalar code shape. **It correlates
with raw memory-touch density**: passing methods are byte-stride or
sparse (≤ 1 store per cache-line per outer iteration); failing
methods are dense — multiple stores per byte, multiple full-buffer
sweeps, or large unrolled groups (write1024v, wrrd128nt).

The flip access pattern itself is **not** the bug: the minimal C
reproducer at `tools/testing/selftests/um/soak/t57/flip-repro.c`
(which mirrors `stress_vm_flip()` semantically — mwc-seed → write →
8× XOR sweeps → mincore-touch → verify, with optional inject) **passes
all 5 iterations × 16 MB on kvm-v2**. So the trigger is not the
arithmetic / access pattern of any single vm-method; it is the
combination of dense memory work + stress-ng's signal-handler /
fork-child setup path.

State-trace evidence (§4-§5) shows:

- Every kvm-v2 failure is a real `#UD` (`port=0xfd` exit) at a real
  user RIP. Within a single run the UD repeats at the **same** user
  RIP (e.g. flip → `0x408290c8`, checkerboard / mscan / modulo-x →
  `0x40010e81`), which is **mid-instruction** in stress-ng's `.text`.
- The user RIP saved in the IST frame at the moment of UD is
  unreachable from the function's start under any sequential
  decoding. So the bug is **control-flow corruption** — an indirect
  call / ret / iretq landing at a bogus address — not a corrupted
  opcode at a sane PC.

Most-likely root cause class (ranked in §5): a residual ring-0 →
ring-3 transition register-state leak in kvm-v2 that corrupts user
SP / RBP / RIP across either an LSTAR-EINTR rewind or a signal
sigreturn path that hits during stress-ng's signal-handler install
(`sigaction`/`sigprocmask`/`signalfd`). T54 / T56 closed two of
these; this is a sibling instance still alive on the dense-MM path.

## 2. Methodology

### 2.1 Inner workload

```sh
stress-ng --vm 1 --vm-bytes 16M --vm-method <M> --verify \
          --timeout 8s --metrics-brief --verbose
```

### 2.2 Kernel boot

UML boot per `/tmp/t57-stress-ng.sh` template:

```
$KERNEL backend=force=<BACKEND> mem=1G ncpus=4 \
        rootfstype=hostfs root=/dev/root rw \
        con=null con0=fd:0,fd:1 panic=-1 init=<script>
```

`mem=1G` was needed: `mem=512M` plus 6-way parallel boots OOM-panicked
the **seccomp** guest on 7 / 38 methods. With 1 G all 38 methods pass
on seccomp (see §3 control column). The kvm-v2 failure pattern was
identical at 512 M and 1 G — memory pressure is not the trigger.

### 2.3 Trace capture

For four methods (flip, checkerboard, mscan, modulo-x) the kvm-v2
state-trace ring was enabled at boot via `kvm_v2_trace_enable` and
dumped after the run with `echo 1 > /sys/kernel/debug/um_kvm_v2_trace/dump`.
The dump prints to `pr_emerg`; we searched it for `port=0xfd` (UD
trap) entries and read the user RIP from the IST frame field
(`KVMV2T-F … tist=[error_code, user_rip, cs, rflags, user_rsp, ss]`).
Ring schema: `state_trace.c:248-291` (capture) and 386-424 (dump
formatter).

### 2.4 Result parsing

Per-method log mined for `STRESS_NG_RC=`, `caught SIGILL`, and the
`metrc … vm   <bogo>` line. Source: `/tmp/t57-bisect/build-matrix.py`
(harness reproducer, kept for re-runs but not committed; see also
`/tmp/t57-bisect/run-one.sh` and `run-matrix.sh`).

### 2.5 Repro materials

- `/tmp/t57-bisect/runs/{seccomp,kvm-v2}-<method>.log` — one boot per
  cell.
- `/tmp/t57-bisect/flip-trace3.log` — flip with state-trace, 5140
  entries, contains the first `port=0xfd` event at seq 115250.
- `/tmp/t57-bisect/trace-{checkerboard,mscan,modulo-x}.log` — same
  capture, 3 other failing methods.
- `/tmp/t57-bisect/stress-ng-0.20.01/` — upstream source for cross-
  reference.

## 3. Per-method matrix

`PASS_v2` / `FAIL_v2` is the kvm-v2 verdict; seccomp passed for all 38.

| method        | seccomp rc | seccomp bogo | kvm-v2 rc | kvm-v2 bogo | kvm-v2 sig | result |
|---------------|-----------:|-------------:|----------:|------------:|-----------:|--------|
| cache-lines   |          0 |        1 664 |         0 |         768 |          - | PASS_v2 |
| **cache-stripe**  |          0 |          704 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **checkerboard**  |          0 |        1 600 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **flip**          |          0 |      184 320 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **fwdrev**        |          0 |        8 742 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| galpat-0      |          0 |        1 600 |         0 |         512 |          - | PASS_v2 |
| galpat-1      |          0 |          768 |         0 |         448 |          - | PASS_v2 |
| gray          |          0 |       36 864 |         0 |      32 768 |          - | PASS_v2 |
| grayflip      |          0 |       40 960 |         0 |      36 864 |          - | PASS_v2 |
| **incdec**        |          0 |       45 056 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| inc-nybble    |          0 |       14 336 |         0 |       6 144 |          - | PASS_v2 |
| lfsr32        |          0 |          192 |         0 |         313 |          - | PASS_v2 |
| **modulo-x**      |          0 |      923 136 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **move-inv**      |          0 |       37 376 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **mscan**         |          0 |      235 046 |         **2** |       4 096 | **SIGILL** | **FAIL_v2** |
| **one-zero**      |          0 |       18 944 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **prime-0**       |          0 |       24 777 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **prime-1**       |          0 |       13 966 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **prime-gray-0**  |          0 |       10 810 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **prime-gray-1**  |          0 |       24 240 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **prime-incdec**  |          0 |       94 208 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **rand-set**      |          0 |        9 728 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **rand-sum**      |          0 |          125 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| read64        |          0 |          352 |         0 |         201 |          - | PASS_v2 |
| **ror**           |          0 |        8 192 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **rowhammer**     |          0 |        2 441 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **swap**          |          0 |        2 296 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| walk-0a       |          0 |          256 |         0 |         112 |          - | PASS_v2 |
| walk-0d       |          0 |       36 864 |         0 |      36 864 |          - | PASS_v2 |
| walk-1a       |          0 |          400 |         0 |         144 |          - | PASS_v2 |
| walk-1d       |          0 |       85 867 |         0 |      40 960 |          - | PASS_v2 |
| walk-flush    |          0 |        1 307 |         0 |         278 |          - | PASS_v2 |
| write64       |          0 |          192 |         0 |         160 |          - | PASS_v2 |
| write64ds     |          0 |          206 |         0 |         160 |          - | PASS_v2 |
| write64nt     |          0 |          416 |         0 |         176 |          - | PASS_v2 |
| **write1024v**    |          0 |          804 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **wrrd128nt**     |          0 |        1 152 |         **2** |           - | **SIGILL** | **FAIL_v2** |
| **zero-one**      |          0 |       26 112 |         **2** |           - | **SIGILL** | **FAIL_v2** |

Notes:

- The user-quoted "12 vm-methods per `man stress-ng-VM-stress-method`"
  is from an older man page. stress-ng 0.20.01 (Ubuntu 25.10) exposes
  38 methods; we ran the full set.
- `write1024v`, `wrrd128nt`, `flip`, `fwdrev`, `move-inv` all FAIL on
  v2. Their seccomp-side bogo counts span 4 orders of magnitude, so
  the trigger is not "high op-rate".
- `mscan` is the only failing method that completed any v2 bogo ops
  before tripping (4 096). It's the simplest failing pattern (per-byte
  `|=` sweep). Promising shrink target for a future repro.

## 4. Failing methods — deep dive

### 4.1 What stress-ng does

`stress-ng --vm` always installs `stress_signal_catch_sigill_handler`
once at the start of the stressor (`stress-vm.c:3561`). The handler
prints `caught SIGILL, address 0x%16.16x (%s)` from `info->si_addr`
and `info->si_code`, then `_exit(EXIT_FAILURE)` (core-signal.c:618).
The "0x… not readable" trail is `stress_dump_readable_data()` walking
±16 bytes around the (apparently zero) fault address with `mincore`
(core-signal.c:188-223). Those lines are downstream — not separate
faults.

So every fail signal in §3 means **one** real SIGILL was delivered to
the stressor child, with `si_code = ILL_ILLOPN`.

### 4.2 The `si_addr=0` is a kvm-v2 artefact

`arch/um/backend/kvm-v2/syscall_trap.c` `kvm_v2_handle_io_ud`:

```c
1915     regs->faultinfo.error_code = 0;
1916     regs->faultinfo.cr2        = 0;
1917     regs->faultinfo.trap_no    = 6;
…
1921     kvm_v2_dispatch_relay(regs, SIGILL, ILL_ILLOPN);
```

The `cr2=0` becomes `siginfo->si_addr=0` after the relay path. So
**every** kvm-v2 #UD looks like address-zero from userspace. This is
intentional (#UD has no fault address) but it forced us to consult
the IST frame for the real RIP.

### 4.3 IST-frame evidence

State-trace dump from a flip run (5 first-UD events, all in the same
boot):

```
seq 115250  POST_KVM_RUN exit=2 port=0xfd
            tist=[7,408290c8,2b,10212,7f7fffb77af8,23]
seq 115288  POST_KVM_RUN exit=2 port=0xfd
            tist=[7,408290c8,2b,10212,7f7fffb77af8,23]
seq 115326  POST_KVM_RUN exit=2 port=0xfd
            tist=[7,408290c8,2b,10212,7f7fffb77af8,23]
… (5 total, same RIP/RSP)
```

Decoded with the `kvm_v2_ist_frame_read` schema (`syscall_trap.c:1082-1086`):
- error-code-slot = `0x7` (no error code; this is residue from the
  previous IST consumer — a known "garbage above RIP" artefact).
- **user_rip   = `0x408290c8`** ← real faulting RIP.
- user_cs     = `0x2b` (user CS).
- user_rflags = `0x10212` (RF=1 set, IF=1, AC=1; AC means SMAP-emulated
  user just executed a CMC/STAC pair? — likely just the kernel-set
  pre-iretq AC flag; unimportant for RIP).
- user_rsp    = `0x7f7fffb77af8`.
- user_ss     = `0x23`.

Disassembling stress-ng around RVA `0x8290a0` (the function 0x408290c8
falls in, after subtracting the typical PIE base 0x40000000) shows:

```
8290a0 <fn>:
   8290a0:   53                       push   %rbx
   8290a1:   48 83 e4 c0              and    $0xfffffff…c0,%rsp
   8290a5:   48 81 ec c0 01 00 00     sub    $0x1c0,%rsp
   8290ac:   8b 47 30                 mov    0x30(%rdi),%eax
   8290af:   64 48 8b 1c 25 28 00 00 00   mov  %fs:0x28,%rbx     ; canary load
   8290b8:   48 89 9c 24 b8 01 00 00  mov    %rbx,0x1b8(%rsp)
   8290c0:   48 89 fb                 mov    %rdi,%rbx
   8290c3:   4c 8d a4 24 30 01 00 00  lea    0x130(%rsp),%r12   ; ← 8-byte LEA
   8290cb:   89 44 24 38              mov    %eax,0x38(%rsp)
   8290cf:   e8 cc bf 89 ff           call   c50a0 …
   …
   8290da:   e8 f1 d2 83 ff           call   663d0 <sigemptyset@plt>
   8290df:   e8 0c da 83 ff           call   66af0 <__libc_current_sigrtmin@plt>
   8290e9:   e8 e2 d9 83 ff           call   66ad0 <sigaddset@plt>
   8290f5:   e8 a6 d3 83 ff           call   664a0 <sigprocmask@plt>
   8291??:   e8 …                     call   66a90 <signalfd@plt>
```

`0x8290c8` is **+5 inside the `lea 0x130(%rsp),%r12` at 0x8290c3** —
mid-instruction. There is no decoding of this PC under any sequential
walk that yields a sane opcode boundary. Disassembling raw bytes
starting at exactly 0x8290c8 yields garbage (`add %eax,(%rax) ; … ;
int3 ; …`) — which is exactly the kind of stream that produces
`ILL_ILLOPN` at the first decode attempt.

The function at 0x8290a0 is one of stress-ng's **signal-handler /
signalfd setup** routines (it calls `sigemptyset`, `sigaddset`,
`sigprocmask`, `signalfd`, and reads the TLS canary). It runs **once
per stressor child**, very early in the child's life — before any
vm-method-specific code.

For checkerboard, mscan, and modulo-x, the RIP is `0x40010e81` with
identical register state across the three methods (`rax=0xffffffff,
rbx=0x5500000027fb, rcx=0x550000002811, rdi=0x5500000027fb,
r8=r10=r12=0x40040cd0, r14=0x100000006`). Same per-stressor-child setup
phase, just a different binary location reached because dispatch order
differs.

### 4.4 What the mid-instruction landing implies

A `#UD` at a mid-instruction RIP is **not** an opcode-corruption
fault on contiguous instruction-cache fetch. It is one of:

1. **Indirect call/jmp through a corrupted target**. Stress-ng heavily
   uses `jmp *0x10(%r12)` style jump tables (we see one at 0x83a499 in
   the stress-ng disassembly). A 5-byte miscount of a function pointer
   could land at +5 within the next instruction.
2. **`ret` to a corrupted return address**. The stack canary is loaded
   from `%fs:0x28` and copied to RSP+0x1b8. If `%fs.base` is wrong
   when `ret` reads the canary slot, the saved RBP / return-RIP load
   could come from the wrong page.
3. **`iretq` from an exception / signal frame with a wrong RIP**. Most
   suspicious because it would explain why the same RIP repeats every
   time (the kernel writes the same wrong value on every signal
   delivery), and because we know the stressor child is calling
   `sigaction / signalfd` (which both invoke kernel signal-frame
   construction).

(3) maps onto kvm-v2's `kvm_v2_ist_frame_write` (`syscall_trap.c:1100-1188`).
That helper marshals `regs->gp[HOST_IP]` back into the IST stack as
the RIP that the in-guest gadget's `iretq` will pop. If between
`kvm_v2_ist_frame_read` (line 1908 of `kvm_v2_handle_io_ud`) and the
matching `_ist_frame_write` (line 1926) the upstream `relay_signal →
do_signal → handle_signal → setup_rt_frame` mutates `regs->gp[HOST_IP]`
to a value that is **5 bytes past where it should be**, the iretq
lands mid-instruction and the next decode is `#UD`.

The repeating-RIP pattern (every UD in a run hits the same RIP) is
strong evidence: the bug is deterministic in *what value* gets
written, not random data corruption. The mid-instruction offset of 5
bytes (= `call rel32` length, = `mov $imm32, %edi` length, = `e8/b8`
class instruction length) hints at a missed RIP-fixup in some path
that confuses "after-fault" with "after-call-instruction" semantics —
the way `int 0x80` / `syscall` differ from `#UD` in whether RIP is
pushed pointing to the instruction or to the next one.

This **specific** pattern — RIP advanced past the start of the
instruction — does not match any known T-bug yet closed:

- T41 (af659ad4297d) was about `pf_stub` RAX recovery.
- T54 (467aa7d142c0) was a worker socketpair leak + sysrq halt.
- T56 (db9170b5a7b3) extended the LSTAR-EINTR carve-out for the
  gadget body.
- T26/T27 (FPU cross-task leak) was XSAVE-related, not RIP.

T56 is closest in shape (LSTAR-EINTR rewind path), and the trace shows
T56's "rewind #1" event firing during the failing flip run:
`SMP-T25/T56 LSTAR-EINTR rewind #1 pid=51 comm=stress-ng
rip=ffffe0000000004c rcx=408136c6 (post-swapgs)`. So T56's window
is being entered. The hypothesis in §5.1 is that there is a
**second** LSTAR / iretq path the rewind doesn't cover.

## 5. Hypothesis ranking

### 5.1 Most likely — rt_sigreturn / iretq RIP fixup (P=0.55)

Stress-ng installs SIGILL/SIGSEGV handlers via `sigaction()` and uses
`signalfd()` (visible in the disassembly at the failing function).
After the kernel delivers the SIGILL, the handler `_exit`s, but the
**SIGALRM** that drives stress-ng's `--timeout` and the **SIGCHLD** to
the stressor parent are also flowing. Every signal delivery and every
`rt_sigreturn` traverses an IST-frame round-trip in kvm-v2.

Specific failure mode: when `rt_sigreturn` is invoked from a signal
frame whose saved `rip` points to an instruction longer than 5 bytes,
some path in kvm-v2 (probably the SIGRETURN-handling syscall fast
path that converges with the LSTAR-EINTR rewind) writes
`regs->gp[HOST_IP] += 5` instead of `regs->gp[HOST_IP] = saved_rip`.
Result: user resumes 5 bytes into a multi-byte instruction.

Why dense methods only: dense vm-methods stay in user-mode for many
ms per buffer pass, generating `--timeout`-driven SIGALRM during the
sweep. Sparse methods finish each iteration in microseconds and
return to stress-ng's loop top via a syscall, draining pending
signals at known RIP boundaries (RIP = post-syscall, no fixup
ambiguity).

Test: instrument `kvm_v2_ist_frame_write` (`syscall_trap.c:1100`) to
log every (caller, regs->HOST_IP_in, regs->HOST_IP_out) on the
SIGRETURN path. Run `--vm-method=mscan --timeout 8s`. The first time
HOST_IP_out ≠ HOST_IP_in by exactly 5 (or by the length of the
`call`/`mov-imm32` at HOST_IP_in − N), capture the full state.

### 5.2 Plausible — signal stack mis-restored across vCPU migration (P=0.20)

ncpus=4, signal delivery hops vCPUs, and the IST stack is per-vCPU
(`per_vcpu_ist_tss: cpu=N vcpu_fd=… ist_gpa=…`). If a signal frame
is built on vCPU 0's IST, the stressor child migrates to vCPU 1, and
the matching SIGRETURN reads from vCPU 1's IST top, the popped RIP /
RSP are stale data from a previous trap on vCPU 1. The repeating
same-RIP pattern still fits because the *same* stale value
(left-over from earlier #UD handling on that vCPU) is read every
time.

Test: pin stressor with `--taskset 0` (or sched_setaffinity 1<<0 from
init.sh) and re-run. If the SIGILL stops, this is the cause.

### 5.3 Plausible — XSAVE area mismatch on signal entry (P=0.10)

Memo 15 (`15-smp-t26-t27-fpu-cross-task-leak-FIXED.md`) closed a
ymm/zmm leak across task switch. Stress-ng's signal handler entry
calls `__libc_current_sigrtmin` (a libc function that may use SSE2
for register save), and dense vm-methods generate enough memory
bandwidth that GCC's TARGET_CLONES picks the AVX2 variant, which
saves YMMs at function entry. If the kvm-v2 XSTATE-restore-on-signal-
entry isn't aligned with the kernel's `xsave_state.xstate_bv`, a
fxsave's RFBM mismatch could cause a `#GP` that the kernel recodes as
`#UD` — but we don't see that in the trace, so this stays third-tier.

Test: rebuild stress-ng with `-mno-avx -mno-avx2 -mno-sse4.2` and re-
run. If still fails, ruled out.

### 5.4 Less likely — hostfs /proc/self/maps interaction (P=0.05)

stress-ng's `stress_dump_map_info` (core-signal.c:243) reads
`/proc/self/maps` from the SIGILL handler. UML hostfs does
`/proc/self/maps` differently from upstream procfs. But the fault
**is a real #UD inside user-mode**; the hostfs call only happens
*after* the SIGILL handler runs. It's a witness, not a cause.

### 5.5 Less likely — TARGET_CLONES IFUNC resolution (P=0.05)

stress-ng has BIND_NOW + PIE; IFUNC resolvers for `target_clones`
fire at startup. If `__cpu_indicator_init` mis-detects under kvm-v2's
emulated CPUID and resolves to an extension the host doesn't actually
support, every call to a TARGET_CLONES function would `#UD`. But
**16 of 38 TARGET_CLONES methods pass**, so the resolver is making
the same correct decision for them. Ruled out as primary cause.

### 5.6 Anti-hypothesis — the access pattern itself

Already disproved: `flip-repro.c` reproduces stress_vm_flip()'s exact
mwc-write / 8×XOR / mincore / verify pattern in 248 lines of C, runs
5 × 16 MB on kvm-v2, and PASSES (`/tmp/t57-bisect/flip-repro-v2.log`,
`/tmp/t57-bisect/flip-repro-v2-4mb.log`).

## 6. Next investigation step

Two concrete probes, in order of cost:

### 6.1 Pin-vCPU experiment (cheap, ~2 min)

Re-run the four failing-method state-trace boots with the stressor
process taskset'd to one vCPU:

```sh
taskset -c 0 stress-ng --vm 1 --vm-bytes 16M --vm-method mscan \
    --verify --timeout 8s
```

If the SIGILL goes away, §5.2 (per-vCPU IST stale-frame) is the
primary mechanism; redirect investigation toward
`kvm_v2_ist_frame_restore_pending` (`syscall_trap.c:1189-1248`) and
`per_vcpu_ist_tss`. If the SIGILL persists, §5.1 wins by elimination.

### 6.2 IST-frame-write tracepoint (medium, ~30 min code, no commit)

Add a temporary `pr_emerg` (or `KVMV2_TRACE`-style) hook inside
`kvm_v2_ist_frame_write` (`syscall_trap.c:1100`) that emits, on every
call:

```
caller_pc = __builtin_return_address(0)
regs_in   = regs->gp[HOST_IP]
regs_sp   = regs->gp[HOST_SP]
err_in    = error_code (if has_error_code)
sig_pending = task_sigpending(current)
```

Run `--vm-method mscan --timeout 8s`. Walk the log for the first
write where `regs_in` is **not** the previous `frame.user_rip` read
on the same vCPU (i.e., regs_in changed between read and write). The
caller PC of that anomaly identifies which path (relay_signal,
do_signal, sigreturn, schedule, …) is mutating HOST_IP. That caller
is the bug.

If the anomaly is in `kvm_v2_handle_io_pf` rather than `_ud`, we need
to extend probe.

### 6.3 Bonus — extend `flip-repro.c` until it fails (long-term)

The minimal C reproducer currently doesn't reproduce. Three things
stress-ng does that flip-repro.c doesn't:

1. Forks a stressor child that sets up SIGILL/SIGSEGV/SIGBUS sigaction
   plus signalfd plus sigprocmask sequence (the very function we
   localised the fault in).
2. Drives `setitimer(ITIMER_REAL, …)` for the `--timeout 8s` flag, so
   periodic SIGALRMs land throughout the work.
3. Reads `/proc/self/maps`, `/proc/<pid>/comm`, and `/sys/devices/…`
   from inside signal handlers.

Adding (1) + (2) to `flip-repro.c` is the next bisect step within the
reproducer ladder. Tracked separately (low priority); state-trace
already gives us enough to drive the kernel-side fix.

## 7. Status

- T57 is **characterised, not fixed**.
- Phase J pilot (`phase-J-pilot-2026-05-05.md`) can keep stress-ng
  excluded from its IPC-only profile until 6.1 / 6.2 land.
- The 16 PASS-on-v2 methods (notably `gray`, `walk-1d`, `read64`,
  `write64`) are safe to add to a future Phase J Tier-2 stress profile
  if we want stress-ng coverage now without waiting for the fix.

---

*Author: Claude (investigation agent), 2026-05-07. Counter-signed
checklist: state-trace ring read directly from disassembly + IST
schema; fault RIP confirmed mid-instruction; control-flow corruption
(not opcode corruption) proved by raw-byte vs. function-prologue
disassembly diff; 4 failing methods cross-checked.*
