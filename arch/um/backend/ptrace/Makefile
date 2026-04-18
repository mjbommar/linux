# SPDX-License-Identifier: GPL-2.0
#
# ptrace backend (formerly arch/um/{kernel,os-Linux}/skas ptrace path).
# Ops migrate in one at a time per A-02 priority order.
#
# Files ending in _user.o are USER TUs (libc available, USER_CFLAGS
# applied) per arch/um/scripts/Makefile.rules.

obj-y += ptrace_backend.o
obj-y += debug.o
obj-y += lifecycle.o
obj-y += mm.o
obj-y += thread.o
obj-y += time.o
obj-y += trap_user.o

include $(srctree)/arch/um/scripts/Makefile.rules
