# SPDX-License-Identifier: GPL-2.0
#
# KVM backend v2 (in-progress reimplementation).
# See Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
# 25-v2-restart-guide.md and 26-v2-implementation-plan.md.

obj-$(CONFIG_UM_BACKEND_KVM_V2) += init.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += ops.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += context.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += vcpu.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += memslot.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += region.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += syscall_trap.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += exception.o

# Snapshot / forkserver primitives (Time-machine #168 Phase 1).
# Port of arch/um/backend/kvm-v1-archive/snapshot.c onto v2's
# vCPU pool + memslot shape. See
# Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
# 26-snapshot-v2-port.md for the design memo + Phase sub-sequencing.
obj-$(CONFIG_UM_BACKEND_KVM_V2) += snapshot.o

# Phase H gadget LSTAR trampoline body — assembled blob, position-
# independent x86_64 stub copied at runtime into the per-VM
# trampoline page. Pattern: arch/x86/kernel/ftrace_64.S +
# arch/x86/kernel/ftrace.c (memcpy of ftrace_caller..end). Lives
# in .rodata so objtool treats the bytes as DATA from the host's
# POV — only the guest executes them after the install path
# memcpy's them into the trampoline page at GVA 0xffffe000_00000040.
obj-$(CONFIG_UM_BACKEND_KVM_V2) += lstar_gadget.o

# Per-CPU state-snapshot trace ring. Compiled out entirely when
# CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE=n; runtime gated via
# static_branch when =y. See state_trace.h for the design.
obj-$(CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE) += state_trace.o

# Marshal-shape KUnit suite (D.2 + task #74). Pure data-transform
# tests; no kernel state assumed. Build only when explicitly
# requested.
obj-$(CONFIG_UM_BACKEND_KVM_V2_KUNIT) += test_marshal.o

# Byte-shape KUnit suite (memo 26 §I.2). Locks the SDM-prescribed
# byte layouts for the IDT-pushed exception frame, GDT segment
# descriptors, LSTAR trampoline, and IDT handler stubs. Pure data
# tests; no kernel state assumed. Catches the off-by-8 IST-frame
# read class of bugs that bit E.5 boot smoke + adjacent regressions.
obj-$(CONFIG_UM_BACKEND_KVM_V2_KUNIT) += test_byteshape.o

# Snapshot KUnit suite (memo 26-snapshot §Phase 2). Exercises the
# Phase 1 capture/restore primitives against a live pool vCPU; the
# suite_init fixture drives the lazy first-dispatch arming sequence
# (CPUID + CR4.OSXSAVE + XCR0) on vcpus[0] so the snapshot ioctls
# accept at KUnit run-time, before any user task has dispatched.
obj-$(CONFIG_UM_BACKEND_KVM_V2_KUNIT) += test_snapshot.o
