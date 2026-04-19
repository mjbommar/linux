# SPDX-License-Identifier: GPL-2.0

# Do not instrument kprobes itself.
KCSAN_SANITIZE := n
CFLAGS_REMOVE_core.o = $(CC_FLAGS_FTRACE)

obj-$(CONFIG_KPROBES) += core.o
