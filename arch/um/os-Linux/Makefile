# SPDX-License-Identifier: GPL-2.0
# 
# Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
#

# Don't instrument UML-specific code: this directory is USER-side
# TUs that compile against host headers and don't participate in
# the kernel sanitizer/coverage machinery. Disable every sanitizer
# explicitly to keep the USER/KERNEL TU boundary clean.
KCOV_INSTRUMENT                := n
KASAN_SANITIZE                 := n
KCSAN_SANITIZE                 := n
UBSAN_SANITIZE                 := n

obj-y = elf_aux.o execvp.o file.o helper.o irq.o main.o mem.o process.o \
	registers.o sigio.o signal.o start_up.o time.o tty.o \
	umid.o user_syms.o util.o skas/

CFLAGS_signal.o += -Wframe-larger-than=4096

CFLAGS_main.o += -Wno-frame-larger-than

obj-$(CONFIG_SMP) += smp.o

USER_OBJS := $(user-objs-y) elf_aux.o execvp.o file.o helper.o irq.o \
	main.o mem.o process.o registers.o sigio.o signal.o start_up.o time.o \
	tty.o umid.o util.o smp.o

include $(srctree)/arch/um/scripts/Makefile.rules
