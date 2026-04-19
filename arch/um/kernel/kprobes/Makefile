# SPDX-License-Identifier: GPL-2.0

# Do not instrument kprobes itself. The machinery writes to kernel
# text (int3 install) and runs in a trap context where sanitizer
# recursion would be awkward.
KASAN_SANITIZE := n
KCSAN_SANITIZE := n
KCOV_INSTRUMENT := n
CFLAGS_REMOVE_core.o = $(CC_FLAGS_FTRACE)

obj-$(CONFIG_KPROBES) += core.o
