# KVM-only FPU isolation reproducers

Standalone userspace KVM tests verifying that **upstream KVM
correctly preserves vcpu FPU across IO-trap exits** under various
patterns. Used to attribute the H.1b XMM-clobber bug to UML's
environment vs. upstream KVM (memo §H.1b).

All four tests PASS on standard kernels — the bug observed in
UML+KVM-v2's mt-mmap-stress / mt-xmmprobe is therefore in UML's
FPU isolation logic, not in upstream KVM.

## Tests

- `kvm-fpu-repro.c` — basic load-XMM, IO-trap, readback
- `kvm-fpu-uml-pattern.c` — UML's dispatch pattern (SYNC_REGS dirty
  bits, CR4.PGE toggle between runs)
- `kvm-fpu-task-switch.c` — multiple "tasks" sharing one vcpu with
  explicit FPU save/load + host XMM clobber between RUNs
- `kvm-fpu-sigalrm.c` — SIGALRM-driven KVM_RUN -EINTR + host XMM
  clobber on each -EINTR

## Build & run

    gcc -O2 -static -o kvm-fpu-repro kvm-fpu-repro.c
    ./kvm-fpu-repro

Exits 0 on PASS, non-zero on FAIL.

## What we learned

KVM's `kvm_load_guest_fpu` / `kvm_put_guest_fpu` pair (called once per
KVM_RUN ioctl) correctly saves/restores guest FPU around the userspace
exit boundary. UML's gap is at a different layer — multiple UML "tasks"
sharing one vcpu need per-task FPU snapshotting, which the existing
context-switch-only mechanism doesn't fully cover. Our v2 fix at
vcpu.c:1474+ adds per-dispatch save/restore as a brute-force safety
net.
