# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2002 - 2007 Jeff Dike (jdike@{linux.intel,addtoit}.com)
#

# USER-side TUs; mirror os-Linux/Makefile's sanitizer opt-outs so
# the kernel sanitizer flags don't get injected here.
KCOV_INSTRUMENT                := n
KASAN_SANITIZE                 := n
KCSAN_SANITIZE                 := n
UBSAN_SANITIZE                 := n

obj-y := mem.o process.o

USER_OBJS := $(obj-y)

include $(srctree)/arch/um/scripts/Makefile.rules
