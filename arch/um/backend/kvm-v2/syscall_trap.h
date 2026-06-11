/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KVM backend syscall-trap ABI.
 *
 * The trampoline page is owned by syscall_trap.c and referenced from the
 * per-VM state in struct kvm_v2_vm. Guest stubs use fixed IO ports as
 * trap-class tags; kvm_v2_handle_io_trap() switches on those ports after
 * KVM reports KVM_EXIT_IO.
 */
#ifndef __ARCH_UM_BACKEND_KVM_V2_SYSCALL_TRAP_H
#define __ARCH_UM_BACKEND_KVM_V2_SYSCALL_TRAP_H

#include <linux/threads.h>
#include <linux/types.h>

struct kvm_run;
struct kvm_v2_vcpu;
struct kvm_v2_vm;
struct uml_pt_regs;

/*
 * LSTAR trampoline bodies assembled in lstar_gadget.S. syscall_trap.c copies
 * them into the guest trampoline page, vcpu.c sizes the EINTR recovery range
 * against them, and KUnit locks their byte shape.
 */
extern const u8 kvm_v2_lstar_fallback_start[];
extern const u8 kvm_v2_lstar_fallback_end[];
extern const u8 kvm_v2_lstar_gadget_start[];
extern const u8 kvm_v2_lstar_gadget_end[];

/*
 * Host-side trap-class enum. The IO port number identifies the class on
 * the wire. Exception stubs use out %al, $port ; iretq; the host
 * dispatcher decodes the port and calls the matching handler.
 */
enum um_kvm_iotrap {
	UM_KVM_TRAP_SYSCALL = 0xf4,
	UM_KVM_TRAP_PF      = 0xf6,	/* #PF (vector 14) */
	UM_KVM_TRAP_GP      = 0xf9,	/* #GP (vector 13) */
	UM_KVM_TRAP_UD      = 0xfa,	/* #UD (vector  6) */
	UM_KVM_TRAP_DE      = 0xfb,	/* #DE (vector  0) */
	UM_KVM_TRAP_OF      = 0xfc,	/* #OF (vector  4) */
	UM_KVM_TRAP_NM      = 0xfd,	/* #NM (vector  7) */
	UM_KVM_TRAP_BP      = 0xfe,	/* #BP (vector  3); breakpoint (INT3) */
	UM_KVM_TRAP_DB      = 0xf5,	/* #DB (vector  1); debug exception */
	UM_KVM_TRAP_DF      = 0xf2,	/* #DF (vector  8); double fault */
	UM_KVM_TRAP_SS      = 0xf7,	/* #SS (vector 12); stack segment */
	UM_KVM_TRAP_AC      = 0xf3,	/* #AC (vector 17); alignment check */
	UM_KVM_TRAP_TS      = 0xf1,	/* #TS (vector 10); invalid TSS */
	UM_KVM_TRAP_NP      = 0xef,	/* #NP (vector 11); segment not present */
	UM_KVM_TRAP_MF      = 0xee,	/* #MF (vector 16); x87 FP exception */
	UM_KVM_TRAP_XM      = 0xed,	/* #XM (vector 19); SIMD FP exception */
	UM_KVM_TRAP_PANIC   = 0xf8,	/* unhandled-vector stub */
};

/*
 * Guest VA where the trampoline page is mapped in the kernel half. A
 * CPL=3 guest walking a user-half VA never reaches this slot, and the
 * trampoline page is mapped with user access disabled.
 */
#define KVM_V2_TRAMPOLINE_GVA		0xffffe00000000000ULL

/*
 * Offset within the trampoline page where MSR_LSTAR points. 0x40 gives
 * the LSTAR body cache-line alignment while leaving low page offsets
 * free for other trampoline metadata if needed.
 */
#define KVM_V2_TRAMPOLINE_LSTAR_OFFSET	0x40

/*
 * Guest VA the host writes to MSR_LSTAR. Keep it derived from the page
 * base and offset so MSR programming and trampoline layout cannot drift.
 */
#define KVM_V2_LSTAR_GVA		(KVM_V2_TRAMPOLINE_GVA + \
					 KVM_V2_TRAMPOLINE_LSTAR_OFFSET)

/*
 * Per-vCPU gadget state pages.
 *
 * Each vCPU owns a dedicated 4 KB gadget state page containing the
 * fields the LSTAR fast-path reads via swapgs ; mov %gs:OFFSET, %eax.
 * Per-vCPU isolation is required because v2's per-host-CPU vCPU pool
 * runs different tasks on different vCPUs simultaneously; a shared state
 * page would race and could hand the gadget another vCPU's values.
 *
 * Layout in the trampoline PTE table:
 *   PTE[0]                              trampoline (per-VM, +0x000)
 *   PTE[1]                              IDT       (per-VM, +0x1000)
 *   PTE[2]                              handlers  (per-VM, +0x2000)
 *   PTE[3]                              GDT       (per-VM, +0x3000)
 *   PTE[KVM_V2_IST_BASE_SLOT + 2*cpu]   IST stack (per-vCPU)
 *   PTE[KVM_V2_IST_BASE_SLOT + 2*cpu+1] TSS body  (per-vCPU)
 *   PTE[KVM_V2_GADGET_BASE_SLOT + cpu]  gadget state page (per-vCPU,
 *                                                          this region)
 *
 * Per-vCPU MSR_KERNEL_GS_BASE = KVM_V2_GADGET_STATE_GVA(cpu) so each
 * vCPU's swapgs lands on its own state page. load_user_sregs refreshes
 * the fields from the active task before KVM_RUN.
 *
 * The gadget reads via %gs:disp32 against KERNEL_GS_BASE, so the state
 * needs a single per-vCPU base. Keep it out of the IST and TSS pages:
 * those pages are architecturally written during exception delivery and
 * task-register handling.
 *
 * The offset constants below are the ABI between lstar_gadget.S and
 * the host-side refresh path. Keep save slots 8-byte aligned and update
 * test_byteshape.c when changing this layout.
 *
 * Slot arithmetic leaves room for the worst-case UML CPU capacity
 * inside one 512-entry PTE table.
 */
#define KVM_V2_MAX_VCPUS		CONFIG_NR_CPUS

/*
 * UML-private syscall NR for the APERF/MPERF gadget consumer.
 *
 * 0xc0de is well above the upstream Linux x86_64 syscall range. The
 * high bits of the NR mean the gadget's upper-byte guard would reject
 * it, so the APERF/MPERF pre-check runs before the common NR guard.
 *
 * The interface, documented in
 * Documentation/virt/uml/examples/aperf-mperf/README.md:
 *
 *   long uml_aperfmperf(struct um_aperfmperf *out);
 *
 *   struct um_aperfmperf { u64 aperf; u64 mperf; };
 *
 *   Returns 0 and writes raw IA32_APERF / IA32_MPERF host-counter
 *   values into *out. An out-of-range pointer falls back to the host
 *   trap path. A NULL pointer is a silent no-op that returns 0.
 *
 * The rdmsr instructions execute at guest CPL=0 inside the KVM
 * guest; with CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y +
 * kvm_v2_aperfmperf=on the cap-enable in vm_create lets KVM pass the
 * reads through to hardware. Without it, KVM intercepts and returns zero.
 */
#define KVM_V2_NR_UML_APERFMPERF	0xc0de

#define KVM_V2_GADGET_BASE_SLOT		(KVM_V2_IST_BASE_SLOT + 2 * KVM_V2_MAX_VCPUS)
#define KVM_V2_GADGET_STATE_GVA(cpu)	(KVM_V2_TRAMPOLINE_GVA + \
					 (KVM_V2_GADGET_BASE_SLOT + (cpu)) * 0x1000ULL)

#define KVM_V2_GADGET_OFF_SEQ		0x00	/* reserved seqlock */
#define KVM_V2_GADGET_OFF_CPU_ID	0x04	/* getcpu */
#define KVM_V2_GADGET_OFF_TGID		0x08	/* getpid */
#define KVM_V2_GADGET_OFF_TID		0x0c	/* gettid */
#define KVM_V2_GADGET_OFF_PPID		0x10	/* getppid */
#define KVM_V2_GADGET_OFF_UID		0x14	/* getuid */
#define KVM_V2_GADGET_OFF_EUID		0x18	/* geteuid */
#define KVM_V2_GADGET_OFF_GID		0x1c	/* getgid */
#define KVM_V2_GADGET_OFF_EGID		0x20	/* getegid */
#define KVM_V2_GADGET_OFF_RECORD	0x24	/* record/replay bypass */
/* +0x25..+0x27 padding aligns the 8-byte TASK_SIZE_CAP field. */
/*
 * u64, set once at install: task_size - 16, a 16-byte safety margin for
 * getcpu user-pointer bounds checks before storing cpu/node ids.
 */
#define KVM_V2_GADGET_OFF_TASK_SIZE_CAP	0x28
/*
 * s64 CLOCK_REALTIME seconds. Refreshed in load_user_sregs via
 * ktime_get_real_ts64(). No seqlock: one-second resolution makes a torn
 * read at worst off-by-one, identical to native vDSO behavior.
 */
#define KVM_V2_GADGET_OFF_REAL_SEC	0x30
#define KVM_V2_GADGET_OFF_MONO_SEC	0x38	/* s64 CLOCK_MONOTONIC seconds */
#define KVM_V2_GADGET_OFF_MONO_NSEC	0x40	/* s64 CLOCK_MONOTONIC ns-within-sec */
/*
 * s32 gadget call budget. Decrement + js falls back when negative. Reset
 * on every host refresh so vvar staleness is bounded while SIGALRM is masked
 * during KVM_RUN.
 */
#define KVM_V2_GADGET_OFF_BUDGET	0x48
/* +0x4c is a 4-byte gap for 8-byte SAVE_* alignment. */
/*
 * User RDX, saved at gadget entry and restored at every gadget exit. The
 * Linux x86_64 syscall ABI preserves all GPRs except RAX/RCX/R11, so gadget
 * scratch use must be undone before SYSRETQ.
 */
#define KVM_V2_GADGET_OFF_SAVE_RDX	0x50
#define KVM_V2_GADGET_OFF_SAVE_R8	0x58	/* User R8 scratch save */
#define KVM_V2_GADGET_OFF_SAVE_R10	0x60	/* User R10 scratch save */
/*
 * h_aperfmperf scratch: user RCX is saved before rdmsr, which reads ECX as
 * the MSR index input and clobbers it, then restored before tail. Only the
 * APERF/MPERF path uses this slot.
 */
#define KVM_V2_GADGET_OFF_SAVE_RCX	0x68
/*
 * u8 set to 1 after a successful APERFMPERF disable-exits ioctl. The
 * h_aperfmperf gadget checks this byte before rdmsr and falls back to the
 * host trap path when the cap is unavailable.
 */
#define KVM_V2_GADGET_OFF_APERF_CAP	0x70
/* Approximate clock_gettime work before falling back to a host refresh. */
#define KVM_V2_VVAR_BUDGET_INITIAL	10000

/*
 * Compile-time invariants on the gadget state-page layout. These catch:
 *   - SAVE_RDX/R8/R10 slot misalignment (must be 8B-aligned for
 *     a clean qword move with no #AC trap risk under SMAP/SMEP).
 *   - Slot collisions if a new field is inserted between existing offsets
 *     without bumping subsequent ones.
 *   - State-page overflow against PAGE_SIZE.
 *
 * These are static_assert (compile-time), guarded by __KERNEL__ so an
 * asm-safe sub-header can reuse the layout constants without breaking GAS
 * (which doesn't grok _Static_assert).
 */
#ifdef __KERNEL__
#include <linux/build_bug.h>

static_assert(KVM_V2_GADGET_OFF_SAVE_RDX % 8 == 0,
	      "SAVE_RDX must be 8-byte aligned for movq %gs:disp32 stores");
static_assert(KVM_V2_GADGET_OFF_SAVE_R8  % 8 == 0,
	      "SAVE_R8 must be 8-byte aligned");
static_assert(KVM_V2_GADGET_OFF_SAVE_R10 % 8 == 0,
	      "SAVE_R10 must be 8-byte aligned");

static_assert(KVM_V2_GADGET_OFF_SAVE_RDX + 8 <= KVM_V2_GADGET_OFF_SAVE_R8,
	      "SAVE_RDX overlaps SAVE_R8");
static_assert(KVM_V2_GADGET_OFF_SAVE_R8 + 8 <= KVM_V2_GADGET_OFF_SAVE_R10,
	      "SAVE_R8 overlaps SAVE_R10");
static_assert(KVM_V2_GADGET_OFF_SAVE_R10 + 8 <= KVM_V2_GADGET_OFF_SAVE_RCX,
	      "SAVE_R10 overlaps SAVE_RCX");
static_assert(KVM_V2_GADGET_OFF_SAVE_RCX % 8 == 0,
	      "SAVE_RCX must be 8-byte aligned");
static_assert(KVM_V2_GADGET_OFF_SAVE_RCX + 8 <= KVM_V2_GADGET_OFF_APERF_CAP,
	      "SAVE_RCX overlaps APERF_CAP");
static_assert(KVM_V2_GADGET_OFF_APERF_CAP + 1 <= 4096,
	      "APERF_CAP spills past state-page PAGE_SIZE");

/* Pre-existing fields must not collide with the SAVE block. */
static_assert(KVM_V2_GADGET_OFF_BUDGET + 4 <= KVM_V2_GADGET_OFF_SAVE_RDX,
	      "BUDGET overlaps SAVE_RDX");
static_assert(KVM_V2_GADGET_OFF_MONO_NSEC + 8 <= KVM_V2_GADGET_OFF_BUDGET,
	      "MONO_NSEC overlaps BUDGET");
static_assert(KVM_V2_GADGET_OFF_RECORD + 1 <= KVM_V2_GADGET_OFF_TASK_SIZE_CAP,
	      "RECORD overlaps TASK_SIZE_CAP");
#endif /* __KERNEL__ */

/*
 * IDT, handler stubs, and GDT pages live in the same PML4[448] subtree
 * as the trampoline. A full 256-entry IDT consumes one 4 KB page, so
 * the stubs and GDT use separate pages behind adjacent PTEs.
 *
 * Guest virtual layout:
 *   trampoline: PTE[0] 0xffffe00000000000
 *   IDT:        PTE[1] 0xffffe00000001000
 *   handlers:   PTE[2] 0xffffe00000002000
 *   GDT:        PTE[3] 0xffffe00000003000
 */
#define KVM_V2_IDT_GVA			(KVM_V2_TRAMPOLINE_GVA + 0x1000)
#define KVM_V2_HANDLERS_GVA		(KVM_V2_TRAMPOLINE_GVA + 0x2000)
#define KVM_V2_GDT_GVA			(KVM_V2_TRAMPOLINE_GVA + 0x3000)

/*
 * Shared guest descriptor layout. The GDT image is built in exception.c,
 * while vcpu.c programs selectors into SREGS and MSR_STAR.
 */
#define KVM_V2_GDT_SLOT_KERNEL_CS	1
#define KVM_V2_GDT_SLOT_KERNEL_DS	2
#define KVM_V2_GDT_SLOT_SYSRET_PAD	3
#define KVM_V2_GDT_SLOT_USER_DS		4
#define KVM_V2_GDT_SLOT_USER_CS		5
#define KVM_V2_GDT_SLOT_TSS		6
#define KVM_V2_GDT_NR_SLOTS		8
#define KVM_V2_GDT_LIMIT		(KVM_V2_GDT_NR_SLOTS * 8 - 1)

#define KVM_V2_KERNEL_CS_SEL		(KVM_V2_GDT_SLOT_KERNEL_CS * 8)
#define KVM_V2_KERNEL_DS_SEL		(KVM_V2_GDT_SLOT_KERNEL_DS * 8)
#define KVM_V2_SYSRET_BASE_SEL		(KVM_V2_GDT_SLOT_SYSRET_PAD * 8)
#define KVM_V2_USER_DS_SEL		((KVM_V2_GDT_SLOT_USER_DS * 8) | 3)
#define KVM_V2_USER_CS_SEL		((KVM_V2_GDT_SLOT_USER_CS * 8) | 3)

#define KVM_V2_IDT_NR_VECTORS		256
#define KVM_V2_IDT_LIMIT		(KVM_V2_IDT_NR_VECTORS * 16 - 1)

/*
 * Within the handlers page, each in-guest exception stub is 64 bytes
 * apart. The largest body is the #PF stub, which captures CR2/RDX before
 * exiting to the host; fixed slots keep IDT-gate-offset arithmetic to
 * KVM_V2_HANDLERS_GVA + slot * KVM_V2_HANDLER_SLOT_STRIDE with no
 * per-handler size table.
 */
#define KVM_V2_HANDLER_SLOT_STRIDE	64
#define KVM_V2_HANDLER_SLOT_DE		0	/* #DE (vec 0) */
#define KVM_V2_HANDLER_SLOT_BP		1	/* #BP (vec 3), DPL=3 */
#define KVM_V2_HANDLER_SLOT_OF		2	/* #OF (vec 4), DPL=3 */
#define KVM_V2_HANDLER_SLOT_UD		3	/* #UD (vec 6) */
#define KVM_V2_HANDLER_SLOT_GP		4	/* #GP (vec 13) */
#define KVM_V2_HANDLER_SLOT_PF		5	/* #PF (vec 14) */
#define KVM_V2_HANDLER_SLOT_PANIC	6	/* fallback for any other vector */
#define KVM_V2_HANDLER_SLOT_NM		7	/* #NM (vec 7) */
#define KVM_V2_HANDLER_SLOT_DB		8	/* #DB (vec 1) */
#define KVM_V2_HANDLER_SLOT_SS		9	/* #SS (vec 12) */
#define KVM_V2_HANDLER_SLOT_AC		10	/* #AC (vec 17) */
#define KVM_V2_HANDLER_SLOT_DF		11	/* #DF (vec 8) */
#define KVM_V2_HANDLER_SLOT_TS		12	/* #TS (vec 10) */
#define KVM_V2_HANDLER_SLOT_NP		13	/* #NP (vec 11) */
#define KVM_V2_HANDLER_SLOT_MF		14	/* #MF (vec 16) */
#define KVM_V2_HANDLER_SLOT_XM		15	/* #XM (vec 19) */
#define KVM_V2_HANDLER_NR_SLOTS		16

/*
 * Per-vCPU IST stack and TSS pages. Each vCPU needs a private IST1 stack
 * because exception delivery pushes the iretq frame there. Each vCPU
 * also needs a private TSS because TR caches the TSS descriptor and the
 * TSS body carries that vCPU's IST1 stack pointer.
 *
 *   PTE[4 + cpu*2]:     IST stack page (RW, kernel-only)
 *   PTE[5 + cpu*2]:     TSS page (RW, kernel-only)
 *
 * The guest handler stubs only execute out and return through iretq,
 * so one 4 KB IST page per vCPU is enough for the pushed exception
 * frame and optional error code.
 */
#define KVM_V2_IST_BASE_SLOT		4
#define KVM_V2_IST_GVA(cpu)		(KVM_V2_TRAMPOLINE_GVA + \
					 (KVM_V2_IST_BASE_SLOT + (cpu) * 2) * 0x1000ULL)
#define KVM_V2_TSS_GVA(cpu)		(KVM_V2_TRAMPOLINE_GVA + \
					 (KVM_V2_IST_BASE_SLOT + (cpu) * 2 + 1) * 0x1000ULL)

/*
 * Stacks grow down on x86_64, so the TSS IST1 pointer is the byte just
 * above the mapped IST page.
 */
#define KVM_V2_IST_STACK_TOP_GVA(cpu)	(KVM_V2_IST_GVA(cpu) + 0x1000ULL)

/*
 * TR selector and long-mode TSS limit. Selector 0x30 is GDT slot 6.
 * A long-mode TSS body is 104 bytes, so the segment limit is 103.
 */
#define KVM_V2_TSS_SEL			(KVM_V2_GDT_SLOT_TSS * 8)
#define KVM_V2_TSS_BODY_SIZE		104
#define KVM_V2_TSS_LIMIT		(KVM_V2_TSS_BODY_SIZE - 1)

/*
 * Allocate the per-VM trampoline page, write the 5 LSTAR bytes at the
 * documented offset, and stash the kernel VA + GPA on vm. Idempotent:
 * a successful prior call short-circuits. Allocation can fail before the
 * buddy allocator is available; the late-install path completes the install
 * after mm_init.
 */
int  kvm_v2_trampoline_alloc_and_install(struct kvm_v2_vm *vm);

/*
 * Free the trampoline page and clear the VM fields. Safe on a
 * never-installed VM.
 */
void kvm_v2_trampoline_free(struct kvm_v2_vm *vm);

/*
 * Upgrade the LSTAR body from the 5-byte fallback to the stay-in-guest
 * gadget. Call only after every vCPU's gadget state page has been
 * mapped, because the gadget reads from KVM_V2_GADGET_STATE_GVA(cpu)
 * through %gs.
 *
 * Called from kvm_v2_exception_install at the end of its per-vCPU
 * loop, gated on every loop iteration succeeding. Idempotent.
 */
int kvm_v2_trampoline_upgrade_to_gadget(struct kvm_v2_vm *vm);

/*
 * KVM_EXIT_IO dispatch for guest trap stubs. Called from
 * kvm_v2_vcpu_run's exit-reason switch when the guest traps on a known
 * IO port.
 *
 * regs is already populated from KVM sync regs. run is the mmap'd
 * shared struct kvm_run for the firing vCPU. vcpu is the per-host-CPU
 * pool member and provides access to the IST stack frame for exception
 * dispatch.
 *
 * Returns 0 on success, -EINVAL if vcpu is NULL. Unknown ports are
 * handled by the panic dispatcher.
 */
int kvm_v2_handle_io_trap(struct uml_pt_regs *regs,
			  struct kvm_run *run,
			  struct kvm_v2_vcpu *vcpu);

/*
 * Install the IDT, exception handler stubs, and GDT in the trampoline
 * subtree. Allocates one page for each structure, populates the layouts
 * documented above, installs PTE[1..3], and updates existing vCPUs'
 * IDT/GDT bases through KVM_SET_SREGS.
 *
 * Without this install KVM has no GDT to walk for code-descriptor
 * resolution during exception delivery (the IDT gate selector 0x08
 * indexes into the GDT base) and no IDT to dispatch any vector at
 * all.
 *
 * Idempotent: a successful prior install short-circuits via the
 * vm->idt_kva sentinel.
 *
 * Returns 0 on success / already-installed; -EINVAL if prerequisites
 * are unmet; -ENOMEM if page allocation fails; other negative errno on
 * KVM_SET_SREGS or related ioctl failure.
 */
int  kvm_v2_exception_install(struct kvm_v2_vm *vm);

/*
 * Clear PTE[1..3], free the IDT/handler/GDT pages, and clear the VM
 * fields. Safe on a never-installed VM.
 */
void kvm_v2_exception_free(struct kvm_v2_vm *vm);

#endif /* __ARCH_UM_BACKEND_KVM_V2_SYSCALL_TRAP_H */
