# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
#

# Don't instrument UML-specific code: this directory is built against
# host headers and does not participate in the kernel sanitizer or
# coverage machinery. Disable every sanitizer explicitly to keep that
# boundary clean.
KCOV_INSTRUMENT                := n
KASAN_SANITIZE                 := n
KCSAN_SANITIZE                 := n
KMSAN_SANITIZE                 := n
UBSAN_SANITIZE                 := n

obj-y = elf_aux.o execvp.o file.o helper.o io_uring.o irq.o main.o mem.o process.o \
	registers.o sigio.o signal.o start_up.o time.o tty.o \
	umid.o user_syms.o util.o skas/

CFLAGS_signal.o += -Wframe-larger-than=4096

# Strip CC_FLAGS_FTRACE from the USER_OBJS that dispatch host
# signals into UML's in-kernel handlers. Without this strip,
# graph-tracing these TUs leaks one shadow-stack entry per signal
# delivery because the signal frame is torn down via rt_sigreturn
# rather than a function `ret`.
CFLAGS_REMOVE_signal.o := $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_sigio.o := $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_irq.o := $(CC_FLAGS_FTRACE)

CFLAGS_main.o += -Wno-frame-larger-than

obj-$(CONFIG_SMP) += smp.o
obj-$(CONFIG_UM_WORKER_PROCESS) += worker_user.o
obj-$(CONFIG_UM_TEMPLATE_PAUSE) += template_pause.o

USER_OBJS := $(user-objs-y) elf_aux.o execvp.o file.o helper.o io_uring.o irq.o \
	main.o mem.o process.o registers.o sigio.o signal.o start_up.o time.o \
	tty.o umid.o util.o smp.o worker_user.o template_pause.o

include $(srctree)/arch/um/scripts/Makefile.rules
