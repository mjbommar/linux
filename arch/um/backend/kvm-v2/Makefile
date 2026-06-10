# SPDX-License-Identifier: GPL-2.0
#
# KVM backend v2.

obj-$(CONFIG_UM_BACKEND_KVM_V2) += init.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += ops.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += context.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += vcpu.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += memslot.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += region.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += syscall_trap.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += exception.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += snapshot.o
obj-$(CONFIG_UM_BACKEND_KVM_V2) += snapshot_elf.o

# LSTAR gadget trampoline body. This position-independent x86_64 blob
# lives in .rodata so objtool treats the bytes as host data; the install
# path copies it into the per-VM trampoline page for guest execution.
obj-$(CONFIG_UM_BACKEND_KVM_V2) += lstar_gadget.o

# APERF/MPERF MSR passthrough: boot-param parser + debugfs probe.
# Compiled out entirely when the feature Kconfig is n; when y the TU
# adds (a) the kvm_v2_aperfmperf_enabled() predicate consulted by
# context.c::kvm_v2_vm_create and (b) a debugfs read file at
# /sys/kernel/debug/um/kvm_v2/aperf_mperf that reports the cap-plumbing
# state. See Documentation/virt/uml/aperf-mperf.rst.
obj-$(CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH) += aperfmperf.o

# Marshal-shape KUnit suite. Pure data-transform tests; no kernel state
# assumed. Build only when explicitly requested.
obj-$(CONFIG_UM_BACKEND_KVM_V2_KUNIT) += test_marshal.o

# Byte-shape KUnit suite. Locks the SDM-prescribed byte layouts for the
# IDT-pushed exception frame, GDT segment descriptors, LSTAR trampoline,
# and IDT handler stubs. Pure data tests; no kernel state assumed.
obj-$(CONFIG_UM_BACKEND_KVM_V2_KUNIT) += test_byteshape.o
