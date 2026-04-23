# SPDX-License-Identifier: GPL-2.0
#
# KVM backend (workstream D).
#
# D-02 scaffold: ops-table singleton + scaffold lifecycle +
# stubs for every hot op.
# D-03a: real /dev/kvm probe + init + shutdown in lifecycle.o.
# Later D-03..D-06 add their own TUs (mm.o, thread.o, etc.) as
# the data path lands.

obj-y += kvm_backend.o
obj-y += lifecycle.o
obj-y += stubs.o

include $(srctree)/arch/um/scripts/Makefile.rules
