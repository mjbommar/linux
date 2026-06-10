# UML KVM Backend V2

The KVM v2 backend runs UML user tasks in a KVM vCPU while keeping the
existing UML backend contract. It builds beside the seccomp backend and is
selected at runtime with the backend command line selector.

## Architecture

KVM v2 uses one VM for the UML kernel invocation, a per-host-CPU vCPU pool,
and a single memslot covering `uml_physmem`. Guest page tables contain
UML-physical offsets, so KVM's TDP walk resolves through that physmem slot.

Guest syscalls enter through the guest LSTAR target. The default path exits
to the host with `KVM_EXIT_IO` and is decoded by `syscall_trap.c`. When the
LSTAR gadget is enabled, a small in-guest dispatch body handles selected
cheap syscalls without a vmexit and falls back to the host path for all other
numbers.

Exception delivery uses backend-owned IDT, handler, GDT, IST, and TSS pages
mapped in the kernel-half trampoline region. Per-vCPU descriptor state is
installed through SREGS so exception delivery and lazy-FPU handling can run
without depending on guest boot pages.

## File Layout

| File | Contents |
| --- | --- |
| `init.c` | `/dev/kvm` probe, capability checks, backend registration |
| `ops.c` | `struct um_backend_ops` table and seccomp delegations |
| `context.c` | VM fd lifecycle, TSS/identity-map ioctls, physmem memslot |
| `vcpu.c` | vCPU pool, `KVM_RUN`, SREGS/MSR/CPUID programming, FPU state |
| `memslot.c` | memslot id allocation and record management |
| `region.c` | UML memory-region hooks for the slot-0 KVM model |
| `syscall_trap.c` | LSTAR trampoline install and I/O-trap dispatch |
| `exception.c` | IDT/GDT/TSS/IST pages and exception handler stubs |
| `lstar_gadget.S` | position-independent guest syscall gadget |

## Build And Run

Build KVM v2 with `CONFIG_UM_BACKEND_KVM_V2=y`. The backend remains a runtime
choice; use `backend=force=kvm-v2` to force it for a UML boot.

```bash
make ARCH=um O=$HOME/src/uml-builds/uml-kvm-v2 -j$(nproc)

$HOME/src/uml-builds/uml-kvm-v2/linux backend=force=kvm-v2 mem=256M \
    rootfstype=hostfs root=/dev/root rw \
    con=null con0=fd:0,fd:1 init=/bin/true
```

Enable `CONFIG_UM_BACKEND_KVM_V2_KUNIT` for the backend marshal and byte-shape
KUnit suites.

## Tracing

KVM v2 tracepoints live in `arch/um/include/trace/events/um_backend.h`.

```bash
trace-cmd record -e 'um_backend_kvm_v2:*' \
  $HOME/src/uml-builds/uml-kvm-v2/linux backend=force=kvm-v2 ...
```
