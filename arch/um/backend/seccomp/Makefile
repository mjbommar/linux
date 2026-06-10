# SPDX-License-Identifier: GPL-2.0
#
# seccomp backend.
#
# Most ops point at shared host-side helpers. The seccomp-specific
# ones (probe, vcpu_run, mm_create/destroy, set/get_stub_state) live
# in dedicated files here.
#
# Files ending in _user.o are built with libc available per
# arch/um/scripts/Makefile.rules.

obj-y += seccomp_backend.o
obj-y += debug_user.o
obj-y += lifecycle.o
obj-y += mm.o
obj-y += thread.o
obj-y += time.o
obj-y += trap_user.o

include $(srctree)/arch/um/scripts/Makefile.rules
