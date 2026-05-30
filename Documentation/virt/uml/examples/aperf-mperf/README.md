# APERF/MPERF MSR passthrough — runnable example

This directory ships a small freestanding init program that proves the
UML kvm-v2 backend correctly plumbs
`KVM_CAP_X86_DISABLE_EXITS` / `KVM_X86_DISABLE_EXITS_APERFMPERF` to
the KVM kernel.  Boot UML with the demo as `init=`, and the demo
reads `/sys/kernel/debug/um/kvm_v2/aperf_mperf` and emits a PASS/FAIL
verdict to stdout.  Captured run-output logs are included in this
directory so the expected behavior is visible without a UML build.

For the architectural background (why the feature exists, the
QEMU/libvirt gap that motivated exposing it, the strict pre-vCPU
ordering KVM enforces), see
[`Documentation/virt/uml/aperf-mperf.rst`](../../aperf-mperf.rst).


## Is this actually useful?  Honest assessment

Short answer: **yes for two narrow purposes, no for a third you might
expect.**

The feature plumbs a per-VM KVM cap.  But the cap only changes the
behavior of `rdmsr 0xE7/0xE8` *when that instruction executes at
guest CPL=0 inside the KVM guest*.  For UML, almost no code runs
there:

* UML's "kernel" code runs as a host userspace process at host
  CPL=3 — it issues KVM ioctls but never executes inside the KVM
  guest itself.
* Guest userspace runs at guest CPL=3 inside the KVM guest.
  `rdmsr` `#GP`s at CPL=3 unconditionally, cap or no cap.
* The only code at guest CPL=0 inside the KVM guest is the LSTAR
  trampoline / gadget and the IDT exception handlers — small,
  hand-written assembly stubs that do not currently `rdmsr` on
  APERF/MPERF.

So three uses, ranked by honesty:

1. **Useful (yes): a faithful-VMM reference.** UML's kvm-v2 now
   correctly plumbs `KVM_X86_DISABLE_EXITS_APERFMPERF`.  That makes
   it a known-good userspace VMM against which to compare
   behaviors — e.g. when chasing why a different userspace VMM
   (QEMU + libvirt is the current case) returns zero on these MSRs
   despite advertising the feature via CPUID.  The demo + status
   probe isolate "did the userspace ioctl get issued and accepted"
   from "did the guest read the right value" — which is the exact
   delineation Maintainers debugging the QEMU/libvirt gap need.

2. **Useful (yes): future-proofing for a gadget consumer.** When
   someone adds an `rdmsr 0xE7/0xE8` to the LSTAR gadget body (or a
   custom syscall NR routed through it), the bit will already be
   set per-VM and that code will read real host counters at guest
   CPL=0.  Without the cap-enable now, that future work would also
   have to land the ioctl plumbing.  We've split the two changes
   so the assembly-level gadget work can land independently.

3. **NOT useful (no): reading APERF/MPERF values from inside the
   running UML guest today.**  There is no consumer yet.  A guest
   userspace program that wants the counters cannot get them
   without one of the three adaptations under "Adapting for
   guest-side counter reads" below.  If your goal is to see the
   numbers, this feature alone is insufficient.

If point 3 is what you actually want and you don't need (1) or (2),
the honest answer is "this feature doesn't help you yet."  The
gadget extension under "Adapting" path 2 is the smallest gap to
close — roughly 20 lines of assembly in `lstar_gadget.S` plus a
custom NR, plus a few lines in the demo to call it via inline `asm
volatile("syscall")`.  Happy to land that as a follow-up if you
want the counters surfaced to guest userspace.


## What the demo proves

1. UML's kvm-v2 backend issued `KVM_ENABLE_CAP` for
   `KVM_CAP_X86_DISABLE_EXITS` with the
   `KVM_X86_DISABLE_EXITS_APERFMPERF` bit during VM creation.
2. The KVM kernel accepted the cap (`ioctl_rc == 0`).
3. The verdict reflects the operator's intent (`toggle=on` with the
   boot param, `toggle=off` when explicitly disabled).

That sequence is the architectural plumbing the upstream QEMU +
libvirt chain is missing today.  The demo here proves the
kernel-side path works when a userspace VMM does request the cap
correctly.


## What the demo does NOT prove

The demo does **not** read actual APERF/MPERF counter values via
`rdmsr`.  UML's "kernel" code runs as a host userspace process at
host CPL=3 — `rdmsr` would `#GP` there, regardless of the
disable-exits bit.  The KVM passthrough only affects code running at
**guest** CPL=0 inside the KVM guest (real guest kernels under QEMU;
for kvm-v2 specifically, only the LSTAR gadget and exception stubs).
For a regular Linux guest under QEMU (the upstream-VMM case), the
guest kernel runs at guest CPL=0 and reads the MSRs natively once
the cap is set.  See the bridging notes in
[`../../aperf-mperf.rst`](../../aperf-mperf.rst) under
"Bridging back to QEMU / libvirt".


## Files

| File | Purpose |
|------|---------|
| `aperf-mperf-demo.c` | Freestanding init binary that reads the status probe and emits PASS/FAIL.  No libc, no rootfs needed — runs under hostfs root with the host's own toolchain. |
| `Makefile`           | `make` builds `aperf-mperf-demo`; `make capture` runs it under UML and rewrites the captured logs.  See "Targets" below. |
| `run-output-on.log`  | Captured boot+demo output with `kvm_v2_aperfmperf=on`.  Expected PASS verdict. |
| `run-output-off.log` | Captured boot+demo output with `kvm_v2_aperfmperf=off`.  Expected FAIL verdict (`reason=passthrough_off_by_cmdline`). |


## Prerequisites

* A UML kernel built with:
  - `CONFIG_UM_BACKEND_KVM_V2=y`
  - `CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y`
  - `CONFIG_DEBUG_FS=y` (needed for the status probe at
    `/sys/kernel/debug/um/kvm_v2/aperf_mperf`)
* `/dev/kvm` readable by the running user.
* Host CPU with `aperfmperf` in `/proc/cpuinfo` (Intel Nehalem-and-
  later, AMD Bulldozer-and-later).  KVM's
  `kvm_get_allowed_disable_exits()` masks the bit off otherwise.
* `gcc` (or any `-nostdlib -static -ffreestanding` capable compiler)
  for building the demo binary.


## Build the demo

```bash
$ cd Documentation/virt/uml/examples/aperf-mperf
$ make
cc -Wall -Wextra -O2 -static -nostdlib -ffreestanding \
   -fno-asynchronous-unwind-tables -fno-stack-protector \
   -o aperf-mperf-demo aperf-mperf-demo.c
```


## Run

The Makefile assumes a kernel at
`$(HOME)/src/uml-builds/uml-aperf-test/linux` — override with
`UML_BINARY=...` to point elsewhere.

```bash
$ make run-on                  # boot UML with passthrough enabled
APERF_MPERF_DEMO: toggle=on
APERF_MPERF_DEMO: ioctl_attempted=1
APERF_MPERF_DEMO: ioctl_rc=0
APERF_MPERF_DEMO: host_feature_aperfmperf=1
APERF_MPERF_DEMO: status=enabled
APERF_MPERF_DEMO: PASS plumbing_ok=1

$ make run-off                 # boot UML with passthrough disabled
APERF_MPERF_DEMO: toggle=off
APERF_MPERF_DEMO: ioctl_attempted=0
APERF_MPERF_DEMO: ioctl_rc=0
APERF_MPERF_DEMO: host_feature_aperfmperf=1
APERF_MPERF_DEMO: status=disabled
APERF_MPERF_DEMO: FAIL plumbing_ok=0 reason=passthrough_off_by_cmdline
```

Each run boots a tiny UML instance, the demo runs as PID 1, reads
the status probe, prints the verdict, then `exit_group()`s.  Init
exiting panics the kernel (normal for PID 1) and `panic=-1` reboots
the UML host process so the launcher exits cleanly with code 134
(`SIGABRT` from `panic -> reboot -> SIGABRT`).  That exit code is
expected — the only signal of success/failure is the
`APERF_MPERF_DEMO: PASS|FAIL` line.


## Captured output

Two captured logs from a real run on a Zen 4 host
(AMD Ryzen 7 7840HS, Linux 7.1-rc5+, kvm-v2 backend) are checked in
alongside the demo:

* [`run-output-on.log`](run-output-on.log) — boot with
  `kvm_v2_aperfmperf=on`.  Kernel logs the
  `um: kvm-v2 vm_create: APERF/MPERF MSR passthrough enabled ...`
  message; demo prints the PASS verdict.
* [`run-output-off.log`](run-output-off.log) — boot with
  `kvm_v2_aperfmperf=off`.  Kernel skips the ioctl entirely (no
  passthrough-enabled message in the log); demo prints the FAIL
  verdict with `reason=passthrough_off_by_cmdline`.

The logs include the full UML boot + the demo output + the
init-exit panic at the end.  Search for `APERF_MPERF_DEMO` for the
demo-only lines, or for `MSR passthrough` to confirm the kernel
side issued the ioctl.


## Targets

| Target | What it does |
|--------|-------------|
| `make`            | Build `aperf-mperf-demo`. |
| `make clean`      | Remove the binary and the captured logs. |
| `make run-on`     | Build, boot UML with `kvm_v2_aperfmperf=on`, show demo output. |
| `make run-off`    | Build, boot UML with `kvm_v2_aperfmperf=off`, show demo output. |
| `make capture`    | Run both `run-on` and `run-off` and overwrite the captured logs. |


## How the demo works (5 lines)

```c
mount_debugfs();              /* mount debugfs at /tmp/debug */
rc = read_probe(&p);          /* read /tmp/debug/um/kvm_v2/aperf_mperf */
parse_probe(buf, len, &p);    /* parse 5 key=value lines */
plumbing_ok = p.toggle_on && p.ioctl_attempted && p.ioctl_rc == 0;
sys3(__NR_write, 1, msg, n);  /* emit PASS or FAIL */
```

The probe is the file `/sys/kernel/debug/um/kvm_v2/aperf_mperf`
populated by `arch/um/backend/kvm-v2/aperfmperf.c`.  The kernel
records `ioctl_attempted` and `ioctl_rc` from the `KVM_ENABLE_CAP`
call in `kvm_v2_vm_create()` (`arch/um/backend/kvm-v2/context.c`),
so the probe always reports the precise outcome of the cap-enable
ioctl.


## Adapting for guest-side counter reads

If you want to actually see APERF/MPERF *values* (not just confirm
the plumbing), you need code running at guest CPL=0 inside KVM.
Three options:

1. **Real Linux guest under QEMU.** Apply a QEMU patch to
   plumb `KVM_X86_DISABLE_EXITS_APERFMPERF`, then run a regular
   Linux guest with a small kernel module that calls
   `rdmsrl_safe(MSR_IA32_APERF, ...)`.  This is the production
   scenario; UML serves as evidence the kernel API works.

2. **UML LSTAR gadget extension.** Add a new gadget syscall NR to
   `arch/um/backend/kvm-v2/lstar_gadget.S` that executes
   `rdmsr` on 0xE7/0xE8 at guest CPL=0 (which is where the gadget
   body runs) and returns the values in `%rax` / `%rdx`.  Demo
   userspace would then call the new NR via inline asm.  Invasive
   (touches the assembled gadget; needs the
   `kvm_v2_byteshape` KUnit suite updated).

3. **In-kernel guest kernel under nested KVM.** Not currently
   supported by kvm-v2 (one VM per UML instance per D57).


## See also

* [`Documentation/virt/uml/aperf-mperf.rst`](../../aperf-mperf.rst)
  — operator-facing doc with the QEMU bridging notes.
* `arch/um/backend/kvm-v2/aperfmperf.c` — the Kconfig-gated TU
  that owns the toggle, the `__uml_setup` cmdline parser, the
  ioctl-rc record, and the debugfs probe.
* `arch/um/backend/kvm-v2/context.c::kvm_v2_vm_create` — the
  `KVM_ENABLE_CAP` call site.
* `include/uapi/linux/kvm.h` — flag definitions
  (`KVM_X86_DISABLE_EXITS_APERFMPERF` = bit 4).
* `arch/x86/kvm/x86.c::kvm_vm_ioctl_enable_cap` — KVM-side handler
  with the `kvm->created_vcpus == 0` ordering constraint.
* `tools/testing/selftests/um/aperf-mperf-smoke/` — the kselftest
  that runs the same demo as a regression gate.
