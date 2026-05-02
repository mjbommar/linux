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
