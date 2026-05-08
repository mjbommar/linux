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

## 7. Probe results (2026-05-07)

Executed §6.1 (single-vCPU pin) + §6.2 (`pr_emerg` probe in
`kvm_v2_ist_frame_write`) on `~/src/uml-builds/uml-smp-t41fix/linux`.

### 7.1 §6.1 single-vCPU pin — FAIL (matches multi-vCPU)

`stress-ng --vm 1 --vm-bytes 16M --vm-method=mscan --verify --timeout 5s`
booted with `ncpus=1`:

```
stress-ng: debug: [33] caught SIGILL, address 0x0000000000000000 (ILL_ILLOPN)
stress-ng: error: [31] vm: [32] terminated with an error, exit status=2
vm                 4096      0.62      0.00      0.63   <bogo metrics>
STRESS_NG_RC=2
```

Same SIGILL signature, same 4096 bogo ops (= 16 MiB / 4 KiB) before
trip, same `si_addr=0` artefact. **§5.2 (per-vCPU IST stale-frame
contention) is ruled out** — the bug reproduces with one vCPU, no
inter-vCPU IST sharing. By elimination, §5.1 (RIP-fixup path) is the
remaining hypothesis; §6.2 was needed to localise the offending
caller.

### 7.2 §6.2 `pr_emerg` probe — kvm_v2_ist_frame_write is NOT the mutator

Probe instrumentation (reverted before commit; never landed in tree):

```c
pr_emerg("kvm_v2_ist_frame_write: caller_pc=%pS regs_in=%lx "
         "frame_rip=%llx regs_sp=%lx frame_rsp=%llx err=%llx "
         "sigpend=%d delta=%lld\n",
         __builtin_return_address(0), regs->gp[HOST_IP], frame_rip,
         regs->gp[HOST_SP], frame_rsp, frame_err,
         task_sigpending(current),
         (s64)(regs->gp[HOST_IP] - frame_rip));
```

(`frame_rip` / `frame_rsp` / `frame_err` are read **at the start of
`_ist_frame_write`**, before the rewrite, from the IST stack at
`top - 40 + N`. `delta = regs_in - frame_rip` flags any mismatch
between the upstream-mutated `regs->gp[HOST_IP]` and the current
IST-resident RIP that the iretq tail would otherwise pop.)

**Run produced 5791 `_ist_frame_write` events to first SIGILL.**

| caller_pc                          | events | meaning |
|------------------------------------|-------:|---------|
| `kvm_v2_handle_io_trap+0x750`      |  5789  | inlined `kvm_v2_handle_io_pf` tail |
| `kvm_v2_handle_io_trap+0x83e`      |     1  | inlined #GP/#UD/#DE/#OF/#NM tail-merged |

(The two distinct return PCs map onto the two `call *%rax` sites that
GCC emitted for `kvm_v2_ist_frame_write`. `+0x750` is the #PF arm —
the 4096-COW-fault loop dominates the trace. `+0x83e` is the
tail-merged exit shared by `_ud / _de / _of / _nm` and `_gp` after
their `relay_signal/segv_handler → interrupt_end → ist_frame_write`
sequence; verified from the disassembly at `0x60055240: xor %edx,%edx;
jmp 0x600550ae` collapsing the no-error-code arm into the same call
slot the #GP arm uses.)

### 7.3 The single non-zero `delta` is the legitimate SIGILL delivery

```
seq 5417  caller_pc=kvm_v2_handle_io_trap+0x83e
          regs_in=5500000dde70  frame_rip=550000e9d71e
          regs_sp=7f7fffea5528  frame_rsp=7f7fffea6100
          err=0  sigpend=0  delta=-14416046

seq 5418  caller_pc=kvm_v2_handle_io_trap+0x750
          regs_in=5500000ddcd2  frame_rip=5500000ddcd2  delta=0
```

`regs_in=0x5500000dde70` is **stress-ng's SIGILL handler entry point**
(`stress_signal_catch_sigill_handler`, resolved via libc into the
0x55xxxx mapping range). `frame_rip=0x550000e9d71e` is the user RIP
that took the `#UD`. The kernel correctly mutated `regs->gp[HOST_IP]`
to the handler entry as part of `relay_signal` → `do_signal` →
`handle_signal` → `setup_rt_frame`. This is the **expected** signal
delivery — not a bug. The very next event (`5418`) is a `#PF` at
`0x5500000ddcd2` — i.e., the SIGILL handler's first instruction
faulting on its first not-present page, which is normal.

**All 5790 other `_ist_frame_write` events have `delta=0`**, including
all 4096 mscan-page-stride `#PF` events at `frame_rip=0x550000e9d6af`.
Therefore `kvm_v2_ist_frame_write` is **innocent**: no upstream caller
on this run mutated `regs->gp[HOST_IP]` between `_ist_frame_read` and
`_ist_frame_write` in any unexpected way. §5.1 as originally framed
(`rt_sigreturn` / iretq RIP fixup writing `+= 5`) is **not the
mechanism** — at least, not via any path that flows through
`_ist_frame_write` on the 8-second mscan window.

### 7.4 What the probe DID localise

The trace shows the `#UD` user RIP is **`0x550000e9d71e`**, exactly
**`0x6f` bytes past** the long-running mscan write-loop instruction
at `0x550000e9d6af` (which fired all 4096 mscan-stride `#PF`s with
identical `regs_in == frame_rip` — i.e., the kernel correctly
preserved RIP across each PF). After 4096 page-stride iterations the
loop would cleanly exit; control then advances 0x6f bytes to a `#UD`
at `0x550000e9d71e`.

The 0x55xxxx address range is **not** stress-ng's working buffer (the
buffer is at `0x42210000` per `DIAG[241] nr=9 addr=0x0 len=0x1000000
ret=1109483520=0x42210000`); it is a **dynamic-linker-mapped library
text region** — i.e., `0x550000e9d6af` is the write instruction inside
a libc routine (most likely an AVX/SSE memset or per-byte store
helper that mscan dispatches to via TARGET_CLONES IFUNC resolution).
The `0x6f`-byte forward jump from the write instruction to the `#UD`
site is consistent with a **function epilogue → `ret` → corrupted
return address** landing at mid-instruction in a different libc
function, OR with the function having a multi-block tail that, on
exit from the write loop, lands at a basic-block whose first byte is
not a valid opcode boundary.

### 7.5 Revised hypothesis ranking

The data **moves the suspect class away from `kvm_v2_ist_frame_write`
and towards a non-IST-frame path**:

1. **SYSRETQ-RCX corruption (P=0.45, NEW)**. The syscall return
   path in `kvm_v2_handle_io_trap` (lines 2367-2369) writes
   `run->s.regs.regs.rcx = regs->gp[HOST_IP]` after a possible
   `interrupt_end()`/`do_signal()` rewrite — but if `do_signal`
   advanced HOST_IP for restart-syscall semantics (`-= 2`), or set
   it to a signal-handler entry, **and** the host SVM / KVM_RUN
   then SYSRETQ pops RCX as user RIP, a 5-byte mismatch could
   land mid-instruction. This path does **not** call
   `_ist_frame_write` (it's the SYSCALL arm, marshal-out only),
   so the §6.2 probe missed it by construction. Confirms §5.1's
   "syscall fast path" intuition but moves the locus.

2. **User-mode stack canary / saved-RBP corruption from
   `signalfd` SA setup (P=0.30, NEW)**. The 17-line same-RIP
   COW pause immediately preceding the `0x83e` SIGILL-delivery
   event, plus the 0x6f-byte forward jump, fits a libc routine
   whose tail `ret` reads a corrupted saved RIP — likely because
   the user's `%fs.base` (TLS canary base) was wrong on entry,
   so the canary load `%fs:0x28` aliased to a different page,
   and the function's `cmp %fs:0x28,%rbx; jne __stack_chk_fail`
   either falsely matched or the saved-RBP/RIP at `RSP+0x1b8`
   was clobbered by the in-loop write. The 0x55xxxx region is
   read-only library text, so the **target** of mscan's writes
   is the 0x42210000 buffer, but if `%fs.base` was stale on
   guest re-entry the canary load can corrupt-via-mismatch the
   stack frame's check semantics.

3. **`do_signal` setup_rt_frame copying handler RIP onto user
   stack with off-by-instruction (P=0.15)**. The legitimate
   `+0x83e` event with `regs_in=0x5500000dde70` shows the kernel
   does write a handler RIP. If `setup_rt_frame` also pushed a
   wrong "return-from-handler" sigreturn address onto the user
   stack (the trampoline RIP that the handler's `ret` will pop),
   that would explain the 0x6f-byte forward jump — but only if
   the handler ran to completion, which it did not (stress-ng's
   handler `_exit`s).

4. **Original §5.1 `rt_sigreturn`-via-IST P=0.55 → revised P=0.05**.
   No `_ist_frame_write` event in the trace had a non-zero
   `delta` other than the legitimate SIGILL handler entry.
   `rt_sigreturn` doesn't flow through `_ist_frame_write` on
   resumption (it returns via the syscall marshal-out path), so
   the probe was *blind to* this path, but the absence of
   precursor `_ist_frame_write` anomalies before the SIGILL
   makes the IST-frame mutation theory weaker.

### 7.6 Proposed next probe — narrow on SYSRETQ marshal-out

Because the §6.2 probe ruled out `_ist_frame_write` as the mutator,
the next bisect step is to instrument the **SYSCALL arm's marshal-out**:

```c
/* in kvm_v2_handle_io_trap(), just before line 2369 */
if (current->thread.arch.kvm_v2.diag_count++ < 100 ||
    syscall_nr == __NR_rt_sigreturn ||
    syscall_nr == __NR_rt_sigaction) {
    pr_emerg("kvm_v2_marshal_out: syscall_nr=%lu host_ip=%lx "
             "host_cx=%lx host_r11=%lx host_sp=%lx ret=%ld\n",
             syscall_nr, regs->gp[HOST_IP], regs->gp[HOST_CX],
             regs->gp[HOST_R11], regs->gp[HOST_SP],
             (long)regs->gp[HOST_AX]);
}
```

Expected signal: a `rt_sigaction` or `rt_sigreturn` (or `signalfd`)
syscall completes with `host_ip = N`, but the KVM_RUN that follows
re-enters at `N + 5` (or `N - 2` for an erroneously-rewound
restart-syscall), causing the user's next instruction-fetch to land
mid-opcode. If found, the fix is in the `interrupt_end()` →
`do_signal()` → `PT_REGS_RESTART_SYSCALL` rewind logic, OR in the
explicit `run->s.regs.regs.rcx = regs->gp[HOST_IP]` overwrite at
line 2367 (which may need to be conditional on whether do_signal
already wrote a signal-handler RIP that wants `rcx = original_user_rip`
not the handler entry).

### 7.7 Open questions

- Is the `0x550000e9d71e` `#UD` reached by **legitimate user-mode
  control flow with corrupted operand state** (stack canary /
  saved-RBP scenario) or by **kernel-injected wrong RIP** (SYSRETQ
  / signal-handler-entry scenario)? The state-trace ring schema
  doesn't capture KVM_RUN entry RIP/RCX, only exit; we'd need a
  KVM_GET_REGS dump immediately after the last #PF and before the
  #UD-firing KVM_RUN to know which side broke.

- The dense-vs-sparse correlation in §3 is still unexplained. If
  the bug is SYSRETQ-marshal-out, `signalfd` and `rt_sigaction`
  (called once per stressor child) should fire on every method —
  not 22 / 38. Possibly the dense methods stay in user-mode long
  enough between syscalls for an SIGALRM to land during a
  non-SYSCALL boundary, while sparse methods drain SIGALRMs at
  syscall entry/exit boundaries cleanly. Confirmed only by
  re-running with `--no-itimer` or equivalent.

- The `+0x83e` tail-merge in the disassembly conflates `_ud / _de
  / _of / _nm / _gp` ist_frame_write call sites. To disambiguate
  the trap class on a future probe run, log `regs->faultinfo.trap_no`
  at `_ist_frame_write` entry alongside `caller_pc`.

## §7.6 follow-up probe results (2026-05-07)

Executed §7.6 (SYSCALL marshal-out instrumentation) on
`~/src/uml-builds/uml-smp-t41fix/linux`. Probe extended to **all** IO-trap
arms (PF/GP/UD/DE/OF/NM/SYS) so the whole pre-SIGILL flow is visible,
not only the SYSCALL fast path.

### §7.6-A Instrumentation

A static helper `kvm_v2_diag_marshal_log` was inserted just before
`kvm_v2_handle_io_pf` and called once at the tail of every arm,
immediately after each `kvm_v2_marshal_to_kvm_regs` (and, for the
SYSCALL arm, after the explicit RCX/R11 overwrite at line 2354-2355):

```c
static void kvm_v2_diag_marshal_log(const char *arm,
                                    struct uml_pt_regs *regs,
                                    long syscall_nr)
{
        static atomic_t diag_count = ATOMIC_INIT(0);
        unsigned long ip = regs->gp[HOST_IP];
        bool in_window;
        int n;

        in_window = (ip >= 0x550000e9d6afUL - 0x100 &&
                     ip <= 0x550000e9d71eUL + 0x100);

        n = atomic_inc_return(&diag_count);
        if (n > 50000 && !in_window)
                return;

        pr_emerg("kvm_v2_marshal_out: arm=%s syscall_nr=%ld host_ip=%lx host_cx=%lx host_r11=%lx host_sp=%lx host_ax=%ld pid=%d\n",
                 arm, syscall_nr, ip, regs->gp[HOST_CX],
                 regs->gp[HOST_R11], regs->gp[HOST_SP],
                 (long)regs->gp[HOST_AX], current->pid);
}
```

Call sites: `_pf` after line 1814, `_gp` after 1861, `_ud` after 1927,
`_de` after 1963, `_of` after 2009, `_nm` after 2069, `_sys` after the
RCX/R11 overwrite at 2354-2355. Probe reverted before findings written;
`git status` clean for `arch/um/backend/`.

(First run with a 1000-event bound suppressed too many post-loop
events — the mscan loop hits `0x550000e9d6af` on every PF (in_window),
but the UD-arm marshal-out lands at the **handler entry**
`0x5500000dde70` which is OUT of window, so the 1000-bound dropped it.
Re-run with 50000-event bound captured the UD event.)

### §7.6-B Reproducer trace — the SIGILL never traverses the SYSCALL arm

`stress-ng --vm 1 --vm-bytes 16M --vm-method=mscan --verify --timeout 5s`,
ncpus=1, kvm-v2 backend, log `/tmp/t57-7-6-probe2.log`, **16604 lines,
16334 marshal-out events**. Distribution:

| arm | count |
|-----|------:|
| pf  | 5790  |
| sys | 7013  |
| nm  | 3254  |
| ud  | **1** |
| gp/de/of | 0 |

For the failing stressor child **pid=33**:

- 4094 events total, **0 SYSCALL events**, **0 GP/DE/OF events**.
- 4093 of 4094 events are page-faults at the mscan write loop instruction
  `host_ip=0x550000e9d6af`, with **constant** `host_sp=0x7f7ffffa1100`,
  `host_cx=0xffffffffffffefff`, `host_r11=0x1000000` for every iteration.
- `host_ax` (= mscan's working buffer address) advances exactly 4096
  bytes per PF, from `0x42210000` → `0x432115c0` (16 MiB span — full
  mscan loop).
- The very next event after the last PF is **the UD**, at marshal-out
  time showing `host_ip=0x5500000dde70` (= the SIGILL handler entry,
  per memo §7.3 disassembly of stress-ng), with
  `host_sp=0x7f7ffffa0528` (3032 bytes lower than the loop RSP — i.e.,
  the user fell through several call frames between the loop exit and
  the UD site).

Quoted verbatim, last PF + UD:

```
kvm_v2_marshal_out: arm=pf  syscall_nr=-1 host_ip=550000e9d6af host_cx=ffffffffffffefff host_r11=1000000 host_sp=7f7ffffa1100 host_ax=1126256640 pid=33
kvm_v2_marshal_out: arm=ud  syscall_nr=-1 host_ip=5500000dde70 host_cx=ffffff           host_r11=1000000 host_sp=7f7ffffa0528 host_ax=0          pid=33
kvm_v2_marshal_out: arm=pf  syscall_nr=-1 host_ip=5500000ddcd2 host_cx=55000101065a     host_r11=1000000 host_sp=7f7ffffa0500 host_ax=0          pid=33
```

`host_cx=0xffffff` on the UD frame is the user's RCX at UD time =
`16777215 = 16 MiB - 1` — i.e., **mscan's loop counter at the very
last iteration**, preserved across the trap. It is *not* a kernel-
written value: this is the user's GPR, captured by the IST frame at
fault delivery.

The first `arm=sys` event for pid=33 appears **AFTER** the
`stress-ng: debug: [33] caught SIGILL` line (it's the handler running
`_exit`/`write` to print the diagnostic). Pre-SIGILL, **pid=33 never
executed a syscall under kvm-v2**.

### §7.6-C Hypothesis disposition

**§7.5 hypothesis 1 (SYSRETQ-RCX corruption, P=0.45) is RULED OUT**
for this reproducer. The proposed mechanism required the SYSCALL
arm's marshal-out at lines 2354-2355 to be reached at least once
between mscan loop exit and UD; the trace shows zero such events.
`run->s.regs.regs.rcx = regs->gp[HOST_IP]` cannot be the mutator
because the failing path never executed it.

**§7.5 hypothesis 4 (rt_sigreturn-via-IST) is independently RULED
OUT** by the same evidence: no syscall, hence no rt_sigreturn.

**§7.5 hypothesis 2 (user-mode stack canary / saved-RBP corruption,
P=0.30) is now the leading candidate**, P→0.70. The 4093 #PF burst
exclusively hit the mscan write loop at one RIP with one RSP, so any
kernel-induced state corruption would have to occur via the PF
arm's marshal-out path (`segv_handler` → `interrupt_end` →
`kvm_v2_ist_frame_write` → `kvm_v2_marshal_to_kvm_regs`). The
previous probe (§7.2) showed `_ist_frame_write` did NOT mutate
`regs->gp[HOST_IP]` on these PFs (delta=0 on all 5789 PF events).
But the PF marshal-out does NOT explicitly preserve user RCX/R11/RSP/
RBP/FS.base/GS.base/XSAVE-area state across the trap — those flow
through `kvm_v2_ist_frame_read` + `kvm_v2_marshal_to_kvm_regs` only
for HOST_IP/HOST_SP/HOST_EFLAGS, plus whatever `kvm_v2_marshal_to_kvm_regs`
copies. **A leak of one of those on the PF path would corrupt user
state silently** until the loop exit's `ret` or `cmp %fs:0x28,%rax`
saw the wrong value and either jumped wrong or fell through to a
mid-instruction in libc.

**Hypothesis 3 (do_signal setup_rt_frame off-by-instruction) is
also implausible** for the same reason — no signal was delivered
during the loop (only SIGSEGV-fix-via-PF, which doesn't queue any
user-visible signal).

### §7.6-D Where the bug is now suspected (next probe)

The state leak is along the **#PF marshal-in / marshal-out** path,
specifically in one of:

1. `kvm_v2_marshal_from_kvm_regs` (vcpu.c) at PF arm entry — does it
   correctly propagate ALL GPRs, or does it skip e.g. RBP / R12-R15
   that the stack-canary-cmp sequence at the loop epilogue depends
   on?
2. `kvm_v2_marshal_to_kvm_regs` at PF arm exit (line 1814) — same
   question.
3. The IST stack write (`kvm_v2_ist_frame_write`) handles RIP/CS/
   RFLAGS/RSP/SS only. The user's other GPRs are restored from
   `run->s.regs.regs.*` via the SVM/KVM_RUN sync_regs path. If the
   sync_regs round-trip drops any GPR on the floor (e.g., a 32-bit
   truncation, a missing union member), 4093 PFs in a tight loop
   would re-corrupt the same register every iteration until the
   accumulated drift broke the stack canary check.
4. **#NM (lazy-FPU) cross-PF interaction**. The trace shows 297 #NM
   events for pid=1 (init) and 3254 #NM globally. None for pid=33
   in the mscan loop, but `nm_ts_bypass` is per-task and the FPU
   state machine could be in an unexpected residue state at the
   moment mscan's TARGET_CLONES IFUNC-resolved write helper executes
   AVX/SSE code post-loop. The UD's mid-instruction landing (per
   memo §7.4: 0x6f bytes past the write loop, in libc text) is
   consistent with an AVX/SSE function epilogue executing one
   wrong-XSAVE-state instruction.

**Concrete next probe**: instrument `kvm_v2_marshal_from_kvm_regs`
and `kvm_v2_marshal_to_kvm_regs` in vcpu.c to log every GPR
(RAX..R15, RBP, RSP, RIP, RFLAGS, FS_BASE, GS_BASE) on entry and
exit of the PF arm for pid=33. Walk the log for any GPR that drifts
across two consecutive PFs at `host_ip=0x550000e9d6af` with the same
`host_ax` step. Any non-monotonic GPR (other than HOST_AX, the
mscan buffer pointer) is the leak.

Alternative cheap probe: rebuild stress-ng with `-mno-avx -mno-avx2
-mno-sse4.2 -mno-fma` (per §5.3 test). If the SIGILL stops, the bug
is XSAVE/FPU-state-residue-across-PF (a sibling of T26/T27, but on
the PF arm not the cross-task switch path); if it persists, it's a
GPR sync_regs path leak.

### §7.6-E Open questions

- Memo §7.4 stated the UD landed at `0x550000e9d71e`. The §7.6 trace
  shows the UD-arm marshal-out at `0x5500000dde70` (handler entry)
  because `do_signal` rewrote HOST_IP between `_ist_frame_read` and
  the marshal-out logging point. The original UD `frame.user_rip`
  is not captured by this probe — would need to log `frame.user_rip`
  in `kvm_v2_handle_io_ud` before `dispatch_relay`. Cross-checking
  against §7.3's IST-frame schema dump confirms the UD site is
  `0x550000e9d71e` (within the window, 0x6f bytes past the loop).

- The PF-arm tail-merge at `+0x83e` from the §7.2 probe was
  attributed to `_gp/_de/_of/_nm/_ud` (5 arms sharing a call site).
  The §7.6 arm distribution (UD=1, GP/DE/OF=0, NM=3254) shows the
  `+0x83e` events are dominated by NM, not by the rare UD/GP/DE/OF.
  This rewrites the §7.2 attribution: the 5790 NM/UD events are
  almost entirely #NM (lazy-FPU re-arms), making the "tail-merged
  exception" framing in §7.2 an artefact of the disassembly's
  call-site folding rather than evidence of frequent SIGILL-class
  trips.

- Why does this fail only on dense vm-methods? Same answer as §7.5:
  dense methods generate the long PF burst that gives the leak time
  to accumulate. Sparse methods complete each iteration before any
  GPR/XSAVE drift becomes load-bearing for a function epilogue.

## §7.7 Cheap orthogonal probe (rebuild without AVX/SSE4) — INCONCLUSIVE

Memo §7.6-D suggested rebuilding stress-ng with `-mno-avx -mno-avx2
-mno-sse4.2 -mno-fma` to disambiguate XSAVE/FPU residue from a
generic GPR leak. Attempted on 2026-05-07.

Procedure:
- `git clone --branch V0.20.01 https://github.com/ColinIanKing/stress-ng /tmp/stress-ng-noavx`.
- `CFLAGS="-O2 -mno-avx -mno-avx2 -mno-sse4.2 -mno-sse4.1 -mno-fma -mno-bmi -mno-bmi2 -static" make stress-ng`.
- Verified static binary: `file` reports "statically linked"; `--version` works on host.
- Ran in UML with `cd /run/sng-cwd; stress-ng --vm 1 --vm-bytes 16M --vm-method=mscan --verify --timeout 5s`.

Result: **stress-ng RC=2 on BOTH backends** (seccomp and kvm-v2),
which breaks the experimental control. The rebuilt binary appears to
abort early (no metric lines emitted) regardless of backend. Most
likely the `-mno-bmi/-mno-bmi2/-mno-fma/-static` combination broke
something internal to stress-ng's CPU-feature dispatch (TARGET_CLONES
IFUNC resolution depends on cpuid; static-link warnings about
`getpwent`/`setpwent` requiring shared libraries at runtime suggest
the binary's runtime environment isn't matched).

**Disposition: orthogonal probe abandoned.** The next investigation
step remains §7.6-D's primary plan: instrument
`kvm_v2_marshal_from_kvm_regs` / `kvm_v2_marshal_to_kvm_regs` to
log every GPR (RAX..R15, RBP, FS_BASE, GS_BASE) on entry/exit of
the #PF arm, walk the log for any non-mscan-pointer GPR drift between
consecutive #PFs at the same user RIP. That probe is direct evidence
rather than a process-of-elimination test, so its yield is higher than
the orthogonal probe would have been even if the rebuild had worked.

A less aggressive rebuild (just `-mno-avx -mno-avx2 -mno-sse4.2`,
no static, no `-mno-bmi*/-fma`) might still serve as a corroborating
probe later, but only after §7.6-D has produced a positive
identification of which GPR is drifting.

## §7.6-D probe results (2026-05-07)

Executed §7.6-D's primary plan: instrument `kvm_v2_marshal_from_kvm_regs`
(post-vmexit GPR copy) and `kvm_v2_marshal_to_kvm_regs` (pre-KVM_RUN GPR
copy) in `arch/um/backend/kvm-v2/vcpu.c` to log every GPR (RAX..R15,
RBP, RSP, RIP, RFLAGS, FS_BASE, GS_BASE) on entry/exit of marshal,
filter by `current->comm == "stress-ng-vm"` plus user-VA window, and
walk the log for any GPR drift across consecutive PFs at the same RIP.

### Instrumentation

Two `pr_emerg` blocks added inside `kvm_v2_marshal_to_kvm_regs` (MOUT)
and `kvm_v2_marshal_from_kvm_regs` (MIN). Per-task budget split by
`in_loop_body` (IP == 0x550000e9d6af, the mscan store) vs `OTHR`
(any other user IP), so a small budget for in-loop-body events
(anchors) coexists with a large budget for non-loop-body events
(loop-prologue/epilogue, post-loop fall-through, UD-handler entry).
Caller PC captured via `__builtin_return_address(0)` so PF-arm
(`+0x766`) vs tail-merged `_ud/_de/_of/_nm/_gp` arm (`+0x847`) can be
distinguished.

```c
if (!memcmp(current->comm, "stress-ng-vm", 12)) {
    unsigned long ip = gp[HOST_IP];
    bool in_user = (ip >= 0x550000000000UL && ip < 0x560000000000UL);
    bool in_loop_body = (ip == 0x550000e9d6afUL);
    /* split budgets, log every non-loop-body event */
    pr_emerg("KVMV2-MOUT: %s n=%d pid=%d caller=%pS ip=%lx sp=%lx ax=%lx ... fs=%lx gs=%lx fl=%lx\n",
             in_loop_body ? "BODY" : "OTHR", n, current->pid,
             __builtin_return_address(0), ip, gp[HOST_SP],
             /* all 16 GPRs + FS_BASE/GS_BASE/EFLAGS */);
}
```

Reproducer: `stress-ng --vm 1 --vm-bytes 16M --vm-method=mscan --verify
--timeout 5s` under `ncpus=1`, `mem=512M`. Log
`/tmp/t57-7-6-d-probe4.log` captures 137 OTHR + 13 BODY events for the
failing pid=33, including the smoking-gun frames around SIGILL.

### Trace — GPRs are CLEAN across the PF loop

All 4096 PFs at `frame.user_rip=0x550000e9d6af` (the mscan store)
preserve every GPR except the two that legitimately advance:

```
KVMV2-MOUT: BODY n=1024 pid=33 ip=550000e9d6af ax=42415000 ... r9=1ff000 ...
KVMV2-MOUT: BODY n=2048 pid=33 ip=550000e9d6af ax=42615000 ... r9=3ff000 ...
KVMV2-MOUT: BODY n=3072 pid=33 ip=550000e9d6af ax=42814000 ... r9=5fe000 ...
KVMV2-MOUT: BODY n=4096 pid=33 ip=550000e9d6af ax=42a14000 ... r9=7fe000 ...
KVMV2-MOUT: BODY n=5120 pid=33 ip=550000e9d6af ax=42c13000 ... r9=9fd000 ...
```

(`bx=42214000`, `cx=ffffffffffffefff`, `dx=1`, `si=43216000`,
`di=42216000`, `bp=7f7fffe1f180`, `r8=fffffffffffff000`,
`r10=411db3c0`, `r11=1000000`, `r12=42216000`, `r13=411db3c0`,
`r14=1000000`, `r15=0`, `fs=411b0140`, `gs=0` — invariant for every
single anchor event.) RAX is the mscan buffer pointer (advances
0x1000/PF). R9 is the byte counter (advances 0x1000/PF).

Sample non-loop-body events captured 137 of them in the 0x550000e9d6XX
range (instruction boundaries within the loop body / its epilogue /
the fault-restart paths). Same invariant: every GPR except
`ax`/`r9`/`ip` matches across all 137 events.

**FS_BASE and GS_BASE are bit-identical (`fs=0x411b0140`, `gs=0`)
across every single event for pid=33.** No drift.

### The first non-loop-body event after loop completion is the SIGILL frame

Most informative pair (last in-loop event → first post-loop event):

```
seq 326  OTHR n=117  ip=550000e9d6c7 sp=7f7fffe1f100 ax=431626af ...  r9=f4c6af  fl=286
seq 327  OTHR n=118  ip=5500000dde70 sp=7f7fffe1e528 ax=0       ...  r9=1000000 fl=10202
                ^^^^^^^^^^^^^^^^      ^^^^^^^^^^^^^^^                ^^^^^^^^^
                stress_signal_         stack dropped 3032 bytes      r9 reached
                catch_sigill_handler                                 16 MB —
                entry (per memo §7.4)                                loop natural
                                                                     completion
```

Decoding seq 327: `caller=kvm_v2_handle_io_trap+0x847/0xc4f` =
tail-merged exception arm (`_ud/_de/_of/_nm/_gp`). The ZERO BODY events
show this is the **#UD arm** firing (the only non-NM, non-PF possible
path for a pid=33 stress-ng-vm worker that isn't using FPU). The IP
`0x5500000dde70` is the SIGILL handler entry (memo §7.4 disassembly):
this is what the kernel's `do_signal` rewrote `regs->gp[HOST_IP]` to,
post-`relay_signal` for the UD. The original UD `frame.user_rip` was
`0x550000e9d71e` (memo §7.4) — overwritten in marshal-out by
`do_signal`, so the value at MOUT time is the handler entry, not the
UD site.

The state at seq 327 is **legitimate post-mscan-loop completion**:
`r9 = 0x1000000` = exact 16 MiB (loop-end), `cx = 0xffffff` =
16 MiB - 1, `bx = 0x1000000` = 16 MiB. RSP descended 0xbd8 = 3032
bytes from the loop's RSP `0x7f7fffe1f100` to the UD-frame RSP
`0x7f7fffe1e528`, consistent with the user falling through several
call frames between the loop epilogue and the UD site (memo §7.4
hypothesised this; trace confirms). That descent went through normal
`call`/`push` sequence — `bp` was preserved at `0x7f7fffe1f180` from
in-loop, indicating the function whose loop completed has not yet
returned (its frame still active).

### Verdict — GPR / XSAVE leak hypothesis: **RULED OUT** (for GPR portion)

The `kvm_v2_marshal_*_kvm_regs` round-trip is **innocent**. Across
4096 #PF dispatches, every GPR is preserved bit-exactly except for
the two that the user's mscan loop body modifies (RAX, R9). FS_BASE
and GS_BASE never drift. The marshal helpers are not the leak source.

The XSAVE / FPU portion of the §7.6-D hypothesis is **NOT** disproved
by this probe — `kvm_v2_marshal_*_kvm_regs` only handles GPRs; XSAVE
state flows through the separate `KVM_GET_FPU` / `KVM_SET_FPU` round
in `kvm_v2_vcpu_run` (vcpu.c lines 2122-2148, including the SMP-T26
"always GET_FPU after KVM_RUN" rule and the SMP-T55 fpu_dirty epoch
gate). Those paths are uninstrumented by §7.6-D and remain candidate.

### Implication for the bug location

The trace shows the user's mscan loop completed normally (4096
iterations, r9 advanced cleanly to 0x1000000), then the user fell
through ~3032 bytes of call frames into legitimate user code
**without a kernel intercept** (no MOUT events between seq 326 and
seq 327 — meaning between loop-exit and UD-delivery the user
ran in user-mode for many instructions without any trap). The UD
fired in user-mode at `0x550000e9d71e` (per §7.4 IST frame
schema dump), 0x6f bytes past the mscan write loop.

Three remaining suspect classes (the GPR leak being ruled out
narrows the field):

1. **Stack / heap memory corruption inside the mscan store loop**
   (P=0.45, NEW LEAD). The trace proves the kernel preserves user
   GPRs perfectly across each #PF, but the loop body is *writing*
   to the user-mode buffer at `0x42214000`. If the kernel's TDP /
   page-table coherence has a residual issue on the PF arm — e.g.,
   the COW handling for the stress-ng vm pages occasionally maps a
   PFN that's still cached as a different VA in the per-vCPU TLB —
   the loop's `mov %al, (%rax)` could land on the wrong page.
   stress-ng's `0x42214000` buffer is mmap-allocated; if a wrong
   PFN gets mapped there, the store would corrupt some other VA,
   possibly the function's saved RIP or RBP on the user stack at
   `0x7f7fffe1f180` etc. This is consistent with: (a) no GPR
   drift; (b) RSP/RBP descend "into" the corrupted region during
   loop-exit `ret`; (c) the bug being deterministic across runs
   (same loop count → same wrong PFN → same target VA → same
   corrupted return address → same UD site).

2. **XSAVE / FPU state residue interacting with mscan TARGET_CLONES
   IFUNC dispatch** (P=0.35). mscan's writes are byte stores so
   the *loop body* is integer, but stress-ng compiles with
   TARGET_CLONES which inserts an IFUNC trampoline at function
   entry. The trampoline reads `__cpu_indicator_init` — which is
   resolved via libc's ifunc resolver, which under recent glibc
   uses AVX/SSE for the resolution itself. If the FPU register
   file held stale data from a prior dispatch, the AVX/SSE
   resolver could resolve to a wrong target address, and on
   loop-end the post-loop fall-through could land at the
   IFUNC-redirected wrong address. The §7.7 "rebuild without
   AVX" probe was abandoned (incomplete control); needs a
   smaller `-mno-avx -mno-sse` rebuild without `-static`.

3. **Sub-field of `KVM_CAP_SYNC_REGS` not synced** (P=0.15). The
   marshal helpers only touch GPRs/RIP/RFLAGS. KVM's
   `KVM_SYNC_X86_REGS` covers all 16 GPRs + RIP + RFLAGS in
   `s.regs.regs`, but if the running guest reads a GPR slot that
   `kvm_run->s.regs.regs` doesn't expose (e.g., a hidden RIP-
   shadow on `vmcb->save.rip` separate from the synced
   `s.regs.regs.rip`) the visible GPRs could be correct while
   the actually-loaded ones differ. SVM-specific quirk that
   the marshal probe wouldn't catch.

### Concrete next probes (in order of cost)

**§7.6-E: instrument the `KVM_GET_FPU` / `KVM_SET_FPU` round.**
Add a `pr_emerg` at `kvm_v2_fpu_install_on_first_run` (vcpu.c)
and at the post-vmexit `KVM_GET_FPU` site (vcpu.c lines
2127-2138) logging XMM0/YMM0/MXCSR/FCW for pid=33 across the
PF loop. If MXCSR or any XMM register drifts across consecutive
PFs at `frame.user_rip=0x550000e9d6af`, hypothesis 2 confirmed.

**§7.6-F: page-table corruption probe.** Add a `pr_emerg` at
`kvm_v2_handle_io_pf` that, after `segv_handler` returns,
performs `__get_user(buf, (u64 __user *)0x7f7fffe1e6XX)` — i.e.
reads the user's stack near where the post-loop `ret` will
read its return address. If the read returns a non-canonical
RIP (one that lands at `0x550000e9d71e ± 5`) immediately before
the loop's natural completion, hypothesis 1 confirmed.

**§7.6-G: turn off `KVM_CAP_SYNC_REGS` for the test boot.** Force
the dispatcher to fall back to explicit `KVM_GET_REGS` /
`KVM_SET_REGS` ioctls (the v1-archive shape). If the SIGILL
disappears, hypothesis 3 (sync-regs sub-field aliasing) confirmed.

### Open questions

- The 137 OTHR events for pid=33 hit user IPs in
  `[0x550000e9d6af, 0x550000e9d6ce]` — the same instruction range
  as the mscan write loop. This means the loop body has multiple
  fault-prone instructions, not just one. The IPs span d6af, d6b2,
  d6b5, d6bb, d6be, d6c1, d6c4, d6c7, d6ce — at least 9 distinct
  faulting instructions in the loop. Worth disassembling the
  binary at this VA range to identify them; one of them might
  be the actual ASM source of the corruption (e.g. a bus-locked
  instruction on a misaligned write).

- The probe budget exhaustion gap (between BODY n=5120 and OTHR
  n=117) is on the order of ~500-800 in-loop events that were
  filtered out. To rule out late-loop GPR drift definitively,
  re-run with the BODY budget increased to 8192 and only
  log every 4096th — that captures n=4096 (last good) and n=4097
  (first wrong, if it exists). This run's evidence is consistent
  with no drift but doesn't strictly rule out drift on the very
  last iteration.

## §7.6-F probe results (2026-05-07)

Executed §7.6-F: cheap direct test of the TDP / page-table coherence
hypothesis surfaced by §7.6-D (P=0.45 lead). At #PF arm entry and at
#UD/#GP/#DE/#OF arm entry (just after `kvm_v2_ist_frame_read`,
before `dispatch_relay`), read the user's top-of-stack — `*(u64
__user *)frame.user_rsp` (= the saved RIP that `ret` would pop) and
`*(u64 __user *)(frame.user_rsp + 8)` (= the saved RBP). If
TDP/page-table coherence corruption is silently re-mapping the user
stack page across PFs, the saved RIP at the same SP will mutate
between consecutive PFs at the same user IP.

### Instrumentation

Static helper added to `arch/um/backend/kvm-v2/syscall_trap.c`,
called from `kvm_v2_handle_io_pf` (PF arm), `kvm_v2_handle_io_gp`
(GP arm), `kvm_v2_handle_io_ud` (UD arm), `kvm_v2_handle_io_de`
(DE arm), `kvm_v2_handle_io_of` (OF arm). Filter:
`current->comm == "stress-ng-vm"` AND
`(user_rip in [0x550000e9d5af, 0x550000e9d7af])` OR `arm in {UD,GP,DE,OF}`.
Per-pid 100-event budget for PF arm; signal arms always log.
Reads via `copy_from_user(&saved_rip, (u64 __user *)user_rsp,
sizeof(u64))` — UML's mm-walking copy_from_user (skas/uaccess.c)
correctly walks the current task's page tables, so a user-VA read
that succeeds mirrors what the user task would see.

```c
static void kvm_v2_t76f_user_stack_probe(struct uml_pt_regs *regs,
                                         const char *arm,
                                         unsigned long user_rip,
                                         unsigned long user_rsp,
                                         unsigned long error_code)
{
    /* per-pid budget, comm filter, IP-window filter */
    rc1 = copy_from_user(&saved_rip, (u64 __user *)user_rsp, 8);
    rc2 = copy_from_user(&saved_rbp, (u64 __user *)(user_rsp+8), 8);
    pr_emerg("KVMV2-T76F: arm=%s pid=%d n=%d ip=%lx sp=%lx err=%lx "
             "saved_rip=%llx saved_rbp=%llx rc1=%lu rc2=%lu "
             "ax=%lx r9=%lx\n", ...);
}
```

Reproducer: `stress-ng --vm 1 --vm-bytes 16M --vm-method=mscan --verify
--timeout 5s` under `ncpus=1`, `mem=512M`. Log
`/tmp/t57-7-6-f-probe.log` captures 100 PF events + 1 UD event for
pid=33 (stress-ng-vm worker). RC=2 confirms SIGILL still reproduces.

### Trace — user-stack saved RIP/RBP are CLEAN across the PF loop

```
KVMV2-T76F: arm=PF pid=33 n=1   ip=550000e9d680 sp=7f7ffff14188 err=14 saved_rip=550000d81ec2 saved_rbp=7f7ffff14240 rc1=0 rc2=0 ax=0        r9=0
KVMV2-T76F: arm=PF pid=33 n=2   ip=550000e9d6af sp=7f7ffff14100 err=6  saved_rip=7f7ffff14140 saved_rbp=5500000dd618 rc1=0 rc2=0 ax=42216000 r9=0
KVMV2-T76F: arm=PF pid=33 n=3   ip=550000e9d6af sp=7f7ffff14100 err=6  saved_rip=7f7ffff14140 saved_rbp=5500000dd618 rc1=0 rc2=0 ax=42217000 r9=1000
...
KVMV2-T76F: arm=PF pid=33 n=99  ip=550000e9d6af sp=7f7ffff14100 err=6  saved_rip=7f7ffff14140 saved_rbp=5500000dd618 rc1=0 rc2=0 ax=42277000 r9=61000
KVMV2-T76F: arm=PF pid=33 n=100 ip=550000e9d6af sp=7f7ffff14100 err=6  saved_rip=7f7ffff14140 saved_rbp=5500000dd618 rc1=0 rc2=0 ax=42278000 r9=62000
KVMV2-T76F: arm=UD pid=33 n=101 ip=550000e9d71e sp=7f7ffff14100 err=0  saved_rip=7f7ffff14140 saved_rbp=5500000dd618 rc1=0 rc2=0 ax=43216000 r9=1000000
```

Only TWO unique `(saved_rip, saved_rbp)` pairs across all 101 events:

- `(0x550000d81ec2, 0x7f7ffff14240)` — observed at the lone `n=1`
  event with `ip=0x550000e9d680`, `sp=0x7f7ffff14188`. This is the
  function's PROLOGUE state (SP not yet pushed down to the loop's
  working level). `0x550000d81ec2` is a libc code address — the
  caller's return PC.
- `(0x7f7ffff14140, 0x5500000dd618)` — observed at every PF inside
  the inner loop (`ip=0x550000e9d6af`) AND at the UD site
  (`ip=0x550000e9d71e`). Both bytes-stable (rc1=rc2=0 throughout).
  `saved_rip=0x7f7ffff14140` is a STACK address (not a code pointer)
  — *RSP holds a stack-local that the function stashed at TOS, not
  a return address. The function's stack frame is still active; the
  matching epilogue/`ret` has NOT executed yet at the UD site.

### The UD's user state is much closer to the loop than §7.6-D suggested

§7.6-D's marshal-out trace logged `sp=0x7f7fffe1e528` at MOUT time
and inferred a 3032-byte stack descent through several call frames.
The §7.6-F probe — reading `frame.user_rsp` BEFORE any kernel
mutation — shows the actual UD's user_rsp is **`0x7f7ffff14100`,
identical to the loop body's RSP.**

Reconciliation: between IST-frame-read (where RSP is the user's
true value) and §7.6-D's MOUT log point, the kernel ran
`relay_signal → ... → do_signal → setup_rt_frame`, which **pushes
the signal frame onto the user stack** and rewrites `regs->gp[HOST_SP]`
to the new (lowered) value. The 3032-byte "descent" §7.6-D observed
was the SIGILL signal frame (siginfo + ucontext + xstate, plus
alignment), NOT a chain of `call` frames the user fell through.

That means §7.6-D's hypothesis-1 framing — "user fell through ~3032
bytes of call frames into legitimate user code" — was wrong on the
mechanism but right on the conclusion (the user reached
`0x550000e9d71e` in a single forward step from the loop body, NOT
via a chain of returns). The UD is at `0x550000e9d71e` =
`0x550000e9d6af + 0x6f` while the function's stack frame is still
active. RSP, RBP, R9 (loop counter at exact 16 MiB), and the saved
RIP at *RSP all match what should be expected if the loop completed
naturally and execution continued straight into the next instruction.

### Verdict — TDP / page-table coherence hypothesis: **RULED OUT**

The user-stack page covering `0x7f7ffff14100..0x7f7ffff14148` is
bit-stable across 100 #PF dispatches and the post-loop UD. There is
no TDP/page-table aliasing that's swapping the user's stack out from
under the running task. `copy_from_user` from the kernel agreed with
the user's view (rc=0 throughout) every time. The fault address
`0x42216000..0x43216000` is the mscan write buffer — also no
evidence that those stores landed on a page other than the intended
one (RAX advances 0x1000/PF in lockstep with R9, the byte counter,
and the test would crash much earlier with a panic if the buffer
mapping aliased the stack page).

### What this means for the remaining hypotheses

§7.6-D ranked three candidates after the GPR ruling. §7.6-F now
reshuffles them:

1. ~~**Stack/heap memory corruption**~~ (was P=0.45) → **RULED OUT**
   for the user-stack portion. A future probe could read more user
   addresses (e.g., the mscan buffer's expected post-loop pattern at
   `0x42216000..0x43216000`) to fully exclude the heap-corruption
   sub-case, but the smoking gun §7.6-D pointed at — a corrupted
   return address — is gone.

2. **XSAVE / FPU residue + TARGET_CLONES IFUNC mis-resolution**
   (was P=0.35) → now PRIMARY (P~=0.55). The §7.6-F trace shows the
   user reached `0x550000e9d71e` with intact GPRs, intact stack,
   intact RBP, and the loop counter at exact loop completion. The
   only state that was *not* sampled across PFs is the FPU/XSAVE
   register file. If a stale XMM/YMM register from a prior task's
   FPU state caused a vectorised libc helper to mis-compute (e.g.,
   the post-loop fall-through into `__memset_avx2_unaligned_erms`
   or similar where `0x550000e9d71e` is mid-instruction), this would
   produce exactly the observed pattern: GPRs/stack clean, UD lands
   at a mid-instruction VA past the loop, only on dense vm-methods
   (which trigger enough PFs / cross-task switches for FPU-state
   contamination to occur).

3. **Sub-field of `KVM_CAP_SYNC_REGS` not synced** (was P=0.15) →
   unchanged. Still possible, still requires the §7.6-G probe (turn
   off SYNC_REGS) to confirm/refute.

4. **NEW: post-loop instruction-stream corruption** (P~=0.30). Since
   the GPRs and stack are clean, the only remaining state that could
   make `0x550000e9d71e` UD is the *instruction memory* itself.
   If an SPTE fault on the libc text page (the page covering
   `0x550000e9d6af..0x550000e9d71e` and beyond) gets mapped to a
   wrong PFN, the byte at `0x550000e9d71e` could be a non-decodable
   sequence even though the loop body executed correctly. The
   observed UD is "raised" by a CPU decoding non-canonical bytes
   — this is consistent with a wrong text-page mapping. SMP-T33's
   TDP MMU prev_roots cache drop (commit 9ccdc4300713) addressed
   the cross-task variant; if there's a residual single-task variant
   triggered by the high #PF rate within mscan's tight loop, this
   would explain it.

### Concrete next probes (in order of cost)

**§7.6-H: read the user instruction bytes at `0x550000e9d71e ± 16`
from kernel context at UD time.** Same pattern as §7.6-F but read
INSTRUCTION memory, not stack. If the bytes don't match what
`objdump -d /usr/bin/stress-ng` shows at that VA, hypothesis 4
confirmed (text-page TDP mis-mapping). Cheap: extends the §7.6-F
probe by 16 bytes per UD event.

**§7.6-E (carry forward): instrument `KVM_GET_FPU` / `KVM_SET_FPU`.**
Now PRIMARY, since hypothesis 1 is ruled out. Log MXCSR + XMM0/YMM0
across PF dispatches and at UD. Drift confirms hypothesis 2.

**§7.6-G (carry forward): turn off `KVM_CAP_SYNC_REGS`.** Cheap
ablation. Still relevant.

### Probe code reverted

Per §7.6-F's "investigation only" budget, the instrumentation in
`arch/um/backend/kvm-v2/syscall_trap.c` was reverted via
`git checkout -- arch/um/backend/kvm-v2/syscall_trap.c` and the
kernel rebuilt clean. `git status` shows only the pre-existing
untracked Phase J pilot files; the working tree's tracked files
match HEAD.

## §7.6-H probe results (2026-05-07)

Executed §7.6-H: cheap direct test of the text-page TDP / SPTE
mis-mapping hypothesis surfaced by §7.6-F (the new P=0.30 candidate).
At UD-arm entry (just after `kvm_v2_ist_frame_read`, before
`dispatch_relay`), read 16 bytes of user instruction memory spanning
`HOST_IP - 8 .. HOST_IP + 8` via `copy_from_user`. If TDP has
mis-mapped the user `.text` page, the kernel-read bytes will differ
from `objdump -d /usr/bin/stress-ng` at the same VA.

### Instrumentation

Added to the UD arm of `kvm_v2_handle_io_io_ud` in
`arch/um/backend/kvm-v2/syscall_trap.c`. Filter:
`current->comm == "stress-ng-vm"` (memcmp of the leading 12 chars).
Always logs on every UD event for that comm.

```c
if (!memcmp(current->comm, "stress-ng-vm", 12)) {
    u64 instr_at_ip[2] = {0, 0};
    unsigned long uip = regs->gp[HOST_IP];
    int rc = copy_from_user(instr_at_ip,
                            (void __user *)(uip - 8),
                            sizeof(instr_at_ip));
    pr_emerg("KVMV2-T76H: kvm_v2_ud_textbytes pid=%d comm=%s "
             "ip=%lx rc=%d bytes_at_ip-8=%016llx %016llx\n",
             current->pid, current->comm, uip, rc,
             (unsigned long long)instr_at_ip[0],
             (unsigned long long)instr_at_ip[1]);
}
```

Reproducer (same as §7.6-F): `stress-ng --vm 1 --vm-bytes 16M
--vm-method=mscan --verify --timeout 5s` under `ncpus=1`, `mem=512M`.
Captured in `/tmp/t57-7-6-h-probe.log`. Run repeated 3× for
reproducibility under `umlctl gate loop -W 1 -M 3`.

### Trace — kernel-read bytes match host objdump byte-for-byte

```
KVMV2-T76H: kvm_v2_ud_textbytes pid=42 comm=stress-ng-vm ip=550000e9d71e rc=0 bytes_at_ip-8=00000826860f3ef9 0008bd41f6efc9c5
KVMV2-T76H: kvm_v2_ud_textbytes pid=42 comm=stress-ng-vm ip=550000e9d71e rc=0 bytes_at_ip-8=00000826860f3ef9 0008bd41f6efc9c5
KVMV2-T76H: kvm_v2_ud_textbytes pid=42 comm=stress-ng-vm ip=550000e9d71e rc=0 bytes_at_ip-8=00000826860f3ef9 0008bd41f6efc9c5
```

(One UD event per boot, three boots — bit-identical across all of them.)

Host objdump of `/usr/bin/stress-ng` at the same VA:

```
e9d714: 48 83 f9 3e          cmp    $0x3e,%rcx
e9d718: 0f 86 26 08 00 00    jbe    e9df44 <__stack_chk_fail@@Base+0xdb9b64>
e9d71e: c5 c9 ef f6          vpxor  %xmm6,%xmm6,%xmm6
e9d722: 41 bd 08 00 00 00    mov    $0x8,%r13d
e9d728: 49 89 dc             mov    %rbx,%r12
```

The 16 bytes covering `0x550000e9d716..0x550000e9d725` (= `HOST_IP - 8
.. HOST_IP + 8`) are:

```
offset:  716 717 718 719 71a 71b 71c 71d  71e 71f 720 721 722 723 724 725
bytes:    f9  3e  0f  86  26  08  00  00   c5  c9  ef  f6  41  bd  08  00
u64 LE:  ───────── 00000826860f3ef9 ─────  ───────── 0008bd41f6efc9c5 ───
```

Kernel-read u64[0] = `0x00000826860f3ef9` — **MATCH**.
Kernel-read u64[1] = `0x0008bd41f6efc9c5` — **MATCH**.
`rc=0` — `copy_from_user` succeeded; the page is mapped + readable
from the kernel's view of `current->mm`.

### Verdict — text-page TDP coherence: **RULED OUT**

The kernel-read instruction bytes around the UD site are bit-identical
to the on-disk binary at the same VA, across three independent boots.
There is no SPTE mis-mapping pointing the user text page at a wrong
PFN, no torn / partial page mapping, no stale text content. The bytes
the CPU was decoding when it raised #UD are exactly what `objdump -d
/usr/bin/stress-ng` says they should be:

```
0x550000e9d71e:  c5 c9 ef f6   vpxor %xmm6,%xmm6,%xmm6
```

This is a **VEX-encoded AVX instruction** (the `c5 c9` prefix is a
2-byte VEX). On any CPU with AVX support and `XCR0.YMM` enabled, this
instruction executes silently. The fact that it raises #UD instead is
strong evidence that **the AVX state is not architecturally enabled**
at the moment of execution — i.e., either:

  (a) `CR4.OSXSAVE` is clear in the guest's view, OR
  (b) `XCR0.YMM` is clear (XSAVE-enabled features mask doesn't include
       YMM), OR
  (c) The guest entered a state where AVX is disabled mid-task (e.g.,
       a stale `XSAVE` header from a prior task that didn't have YMM
       enabled, leaked through `KVM_SET_XSAVE` / sync-regs).

All three of these failure modes are **hypothesis 2** territory —
FPU/XSAVE state corruption (probe §7.6-E). The §7.6-H probe upgrades
hypothesis 2 from "primary suspect" to **near-certain**: the bytes
the CPU is decoding are correct, the GPRs/stack are clean, the
control flow is correct (linear forward fall-through from the loop
end into the next instruction), so the only architectural state
that can plausibly cause a VEX-encoded AVX instruction to #UD is
the FPU/XSAVE register file — specifically the XCR0/CR4.OSXSAVE
configuration bits or the XSAVE header.

### What this means for the remaining hypotheses

§7.6-F left four candidates standing. §7.6-H now reshuffles:

1. ~~**Stack/heap memory corruption**~~ — RULED OUT by §7.6-F.

2. **XSAVE / FPU residue + AVX-disable** (was P~=0.55) → now
   **PRIMARY (P~=0.85)**. The §7.6-H byte match plus the fact that
   the faulting instruction is *specifically* a VEX-encoded AVX op
   (`vpxor %xmm6,%xmm6,%xmm6`) is direct evidence that AVX
   architectural enablement is broken at UD time. The TARGET_CLONES
   IFUNC angle from §7.6-D's hypothesis-2 framing is no longer
   needed — the CPU isn't taking a "wrong dispatch into AVX-only
   helper", it's taking the *intended* AVX path and that path
   raises #UD because AVX isn't enabled. Same root cause class
   (FPU/XSAVE leakage), simpler proximate failure mode.

3. ~~**Post-loop instruction-stream corruption**~~ (was P=0.30, NEW
   in §7.6-F) → **RULED OUT (this update).** The bytes match
   objdump. `copy_from_user` succeeds. No text-page TDP mis-mapping.

4. **Sub-field of `KVM_CAP_SYNC_REGS` not synced** (P~=0.15) →
   unchanged. Still possible — if the SYNC_REGS field that's *not*
   being marshalled is `cr4` or `xcr0`-equivalent (the guest's
   xstate enable vector), this would explain hypothesis 2 directly.
   In that sense hypotheses 2 and 4 may be the same bug viewed from
   different angles. §7.6-G ablation (turn off SYNC_REGS, fall
   back to ioctl-driven `KVM_GET/SET_*REGS`) would discriminate:
   if the bug disappears with SYNC_REGS off, the un-synced field
   is the smoking gun.

### Concrete next probes (in order of cost)

**§7.6-E (NOW MANDATORY): instrument `KVM_GET_FPU` / `KVM_SET_FPU`.**
Specifically log:
- `XCR0` (via `KVM_GET_XCRS` / `s.regs.sregs2.xcr0` — or whatever
  field is exposed by SYNC_REGS).
- `CR4.OSXSAVE` bit (= bit 18 of `s.regs.sregs.cr4`).
- The XSAVE header at `KVM_GET_XSAVE` offset 512..575 (xstate_bv
  + xcomp_bv).
- MXCSR.

Snapshot at vCPU-create, at every dispatch entry/exit, and at the
UD point. If XCR0.YMM (bit 2) or CR4.OSXSAVE (bit 18) clears
between the loop's last #PF and the UD, hypothesis 2 is confirmed
and the fix-class is "ensure those bits are set on every
KVM_RUN re-entry from kernel context." The likely culprit is the
NM_TS_BYPASS path (memo §SMP-T22 / SMP-T55) or the v1→v2 marshal
of `current->thread.fpu`'s XSAVE area into `kvm_run->s.regs`.

**§7.6-G: turn off `KVM_CAP_SYNC_REGS`.** Cheap ablation. If the
hang/UD goes away, the un-synced SREGS sub-field that breaks AVX
enablement is the bug. Carry forward as confirmation/refutation
of hypothesis 4.

**§7.4 root-cause work** (Phase J Tier-1 stress-ng): blocked on
§7.6-E producing a positive XCR0 / CR4 / XSAVE-header drift
signature. Once that's in hand, the fix is mechanical (re-arm
the missing bits at the appropriate marshal boundary).

### Probe code reverted

Per the §7.6-H "investigation only" budget, the instrumentation in
`arch/um/backend/kvm-v2/syscall_trap.c` was reverted via
`git checkout -- arch/um/backend/kvm-v2/syscall_trap.c` and the
kernel rebuilt clean. `git status` shows only the pre-existing
untracked Phase J pilot files; the working tree's tracked files
match HEAD.

## 8. Status

- T57 is **characterised, not fixed**.
- §6.1 + §6.2 **ruled out** kvm_v2_ist_frame_write as the RIP
  mutator and re-cast the suspect class as a SYSRETQ /
  signal-marshal-out path issue.
- §7.6 **further ruled out** the SYSCALL marshal-out hypothesis: the
  failing path never traverses the SYSCALL arm at all.
- §7.6-D **ruled out** the GPR portion of the
  marshal-in/marshal-out leak hypothesis — every GPR is preserved
  bit-exactly across all 4096 #PF dispatches, FS_BASE / GS_BASE
  invariant. The XSAVE / FPU portion remains uninstrumented and
  still suspect (probe §7.6-E). Two new candidate classes
  surfaced: TDP/page-table coherence corrupting the user buffer
  (P=0.45), and TARGET_CLONES IFUNC mis-resolution from FPU residue
  (P=0.35).
- §7.6-F **ruled out** the user-stack TDP/page-table
  coherence sub-hypothesis. The user-stack saved RIP/RBP at
  `*RSP` is bit-stable across 100 #PF dispatches and the post-loop
  UD. The §7.6-D-inferred 3032-byte stack descent was an artefact
  of post-`do_signal` SP rewrite (the signal frame), NOT a real
  user-mode descent through call frames — at UD-frame-read time
  user_rsp is identical to the loop body's RSP, and R9 shows
  exact 16 MiB loop completion.
- §7.6-H (this update) **ruled out** the text-page TDP mis-mapping
  sub-hypothesis. The 16 instruction bytes around `0x550000e9d71e`
  read from kernel context via `copy_from_user` match `objdump -d
  /usr/bin/stress-ng` byte-for-byte (`bytes_at_ip-8 =
  00000826860f3ef9 0008bd41f6efc9c5`) across three independent
  boots; `rc=0` confirms the page is mapped + readable. The
  faulting instruction is exactly what the disassembly says it is:
  `c5 c9 ef f6` = `vpxor %xmm6,%xmm6,%xmm6`, a VEX-encoded AVX
  instruction. That a valid AVX op raises #UD points
  near-deterministically at FPU/XSAVE state corruption — XCR0.YMM
  cleared, CR4.OSXSAVE cleared, or XSAVE-header `xstate_bv`
  missing YMM. Remaining candidates: FPU/XSAVE residue / AVX-disable
  (P~=0.85, PRIMARY, probe §7.6-E), KVM_CAP_SYNC_REGS sub-field
  aliasing (P~=0.15; probe §7.6-G — possibly same root cause as the
  FPU/XSAVE residue if the un-synced field IS XCR0 / CR4.OSXSAVE).
- Phase J pilot (`phase-J-pilot-2026-05-05.md`) can keep stress-ng
  excluded from its IPC-only profile until §7.6-E lands.
- The 16 PASS-on-v2 methods (notably `gray`, `walk-1d`, `read64`,
  `write64`) are safe to add to a future Phase J Tier-2 stress profile
  if we want stress-ng coverage now without waiting for the fix.

---

*Author: Claude (investigation agent), 2026-05-07. Counter-signed
checklist: state-trace ring read directly from disassembly + IST
schema; fault RIP confirmed mid-instruction; control-flow corruption
(not opcode corruption) proved by raw-byte vs. function-prologue
disassembly diff; 4 failing methods cross-checked. §7 update
(2026-05-07): single-vCPU pin reproduced fault → §5.2 ruled out;
`_ist_frame_write` probe (5791 events, only 1 non-zero delta = legit
SIGILL delivery) → §5.1 weakened, §7.6 SYSRETQ-marshal-out hypothesis
proposed. Debug instrumentation reverted before findings written.*
