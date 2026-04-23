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
obj-y += thread.o
obj-y += stubs.o

include $(srctree)/arch/um/scripts/Makefile.rules
