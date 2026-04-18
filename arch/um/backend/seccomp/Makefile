# SPDX-License-Identifier: GPL-2.0
#
# seccomp backend (workstream A-03).
#
# Mirror of arch/um/backend/ptrace/Makefile. Most ops point at the
# same shared host-side helpers as ptrace; the truly seccomp-specific
# ones (probe, run_userspace, mm_attach/detach, set/get_stub_state)
# live in dedicated TUs here.
#
# Files ending in _user.o are USER TUs (libc available, USER_CFLAGS
# applied) per arch/um/scripts/Makefile.rules.

obj-y += seccomp_backend.o
obj-y += debug_user.o
obj-y += lifecycle.o
obj-y += mm.o
obj-y += thread.o
obj-y += time.o
obj-y += trap_user.o

include $(srctree)/arch/um/scripts/Makefile.rules
