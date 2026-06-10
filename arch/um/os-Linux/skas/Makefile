# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2002 - 2007 Jeff Dike (jdike@{linux.intel,addtoit}.com)
#

# Host-built objects; mirror os-Linux/Makefile's sanitizer opt-outs so
# kernel sanitizer flags do not get injected here.
KCOV_INSTRUMENT                := n
KASAN_SANITIZE                 := n
KCSAN_SANITIZE                 := n
UBSAN_SANITIZE                 := n

obj-y := mem.o process.o

USER_OBJS := $(obj-y)

# process.c here is the skas dispatch loop; its functions run as
# part of UML's userspace-entry path and, like the os-Linux/
# signal.c handlers, are entered via signal delivery / longjmp
# rather than a normal call/ret. Strip CC_FLAGS_FTRACE so graph
# tracing doesn't push shadow-stack entries that never pop.
CFLAGS_REMOVE_process.o := $(CC_FLAGS_FTRACE)

include $(srctree)/arch/um/scripts/Makefile.rules
