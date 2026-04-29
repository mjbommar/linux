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

# Marshal-shape KUnit suite (D.2 + task #74). Pure data-transform
# tests; no kernel state assumed. Build only when explicitly
# requested.
obj-$(CONFIG_UM_BACKEND_KVM_V2_KUNIT) += test_marshal.o
