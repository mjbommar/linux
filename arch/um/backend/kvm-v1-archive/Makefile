# SPDX-License-Identifier: GPL-2.0
#
# KVM backend (workstream D).
#
# D-02 scaffold: ops-table singleton + stubs for every hot op.
# D-03a: real /dev/kvm probe + init + shutdown in lifecycle.o.
# D-03b: per-UML-process kvm_um + mm_attach/mm_detach in mm.o.
# D-04a: vCPU creation + thread ops + KVM_RUN loop in thread.o.
# Later D-05..D-06 add their own TUs as the data path lands.

obj-y += kvm_backend.o
obj-y += lifecycle.o
obj-y += mm.o
obj-y += sregs.o
obj-y += syscall_class.o
obj-y += thread.o
obj-y += time.o
obj-y += stubs.o

obj-$(CONFIG_UM_BACKEND_KVM_INTEGRATED) += shadow_sync.o
obj-$(CONFIG_UM_BACKEND_KVM_INTEGRATED) += snapshot.o
obj-$(CONFIG_UM_BACKEND_KVM_INTEGRATED) += record.o

# harness.o was the D-04b.1b diagnostic scaffold (1526 LoC, never
# ran outside bring-up). Deleted by memo 25 refactor 10. The other
# obj-y / obj-$(CONFIG_*) entries above are dead too (the parent
# Makefile no longer routes here and CONFIG_UM_BACKEND_KVM_* is
# gone) but preserved verbatim so this directory remains a faithful
# v1 reference snapshot for v2 implementers.

include $(srctree)/arch/um/scripts/Makefile.rules
