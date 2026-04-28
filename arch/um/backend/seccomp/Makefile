# SPDX-License-Identifier: GPL-2.0
#
# seccomp backend (workstream A-03; ops renamed by memo 25 R2).
#
# Most ops point at shared host-side helpers (the formerly-shared
# arch/um/backend/ptrace/ counterpart was removed by memo 25 R11).
# The truly seccomp-specific ones (probe, vcpu_run, mm_create/destroy,
# set/get_stub_state) live in dedicated TUs here.
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
