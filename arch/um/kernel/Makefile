# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux,intel}.com)
#

# Don't instrument UML-specific code; without this, we may crash when
# accessing the instrumentation buffer for the first time from the
# kernel.
KCOV_INSTRUMENT                := n

CPPFLAGS_vmlinux.lds := -DSTART=$(LDS_START)		\
                        -DELF_ARCH=$(LDS_ELF_ARCH)	\
                        -DELF_FORMAT=$(LDS_ELF_FORMAT)	\
			$(LDS_EXTRA)
always-$(KBUILD_BUILTIN) := vmlinux.lds

obj-y = backend.o config.o exec.o exitcode.o hooks.o irq.o ksyms.o mem.o \
	physmem.o process.o ptrace.o reboot.o section_split.o sigio.o \
	signal.o sysrq.o time.o tlb.o trap.o \
	um_arch.o umid.o kmsg_dump.o capflags.o skas/

obj-$(CONFIG_KPROBES) += kprobes/
obj-$(CONFIG_RETHOOK) += rethook.o
obj-$(CONFIG_UM_SNAPSHOT_FORKSERVER) += snapshot.o
# rethook's trampoline runs on every probed function's return;
# it must not itself be traced (same discipline as kprobes).
KASAN_SANITIZE_rethook.o  := n
KCSAN_SANITIZE_rethook.o  := n
KCOV_INSTRUMENT_rethook.o := n
CFLAGS_REMOVE_rethook.o   := $(CC_FLAGS_FTRACE)
obj-$(CONFIG_DEBUG_FS) += um_debugfs.o hooks_bench.o
obj-y += load_file.o

obj-$(CONFIG_FUNCTION_TRACER) += mcount.o
obj-$(CONFIG_DYNAMIC_FTRACE)  += ftrace.o

# Prevent recursion: ftrace's own machinery must not be traced.
CFLAGS_REMOVE_ftrace.o        := $(CC_FLAGS_FTRACE)
# section_split.c backs um_kernel_text_patch_{begin,end}, which
# ftrace calls under stop_machine. Must not itself be traced.
CFLAGS_REMOVE_section_split.o := $(CC_FLAGS_FTRACE)
# early_printk runs before ftrace_init().
CFLAGS_REMOVE_early_printk.o  := $(CC_FLAGS_FTRACE)

obj-$(CONFIG_BLK_DEV_INITRD) += initrd.o
obj-$(CONFIG_GPROF)	+= gprof_syms.o
obj-$(CONFIG_OF) += dtb.o
obj-$(CONFIG_EARLY_PRINTK) += early_printk.o
obj-$(CONFIG_STACKTRACE) += stacktrace.o
obj-$(CONFIG_SMP) += smp.o

USER_OBJS := config.o

include $(srctree)/arch/um/scripts/Makefile.rules

targets := config.c config.tmp capflags.c

# Be careful with the below Sed code - sed is pitfall-rich!
# We use sed to lower build requirements, for "embedded" builders for instance.

$(obj)/config.tmp: $(objtree)/.config FORCE
	$(call if_changed,quote1)

quiet_cmd_quote1 = QUOTE   $@
      cmd_quote1 = sed -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n",/' \
		   $< > $@

$(obj)/config.c: $(src)/config.c.in $(obj)/config.tmp FORCE
	$(call if_changed,quote2)

quiet_cmd_mkcapflags = MKCAP   $@
      cmd_mkcapflags = $(CONFIG_SHELL) $(src)/../../x86/kernel/cpu/mkcapflags.sh $@ $^

cpufeature = $(src)/../../x86/include/asm/cpufeatures.h
vmxfeature = $(src)/../../x86/include/asm/vmxfeatures.h

$(obj)/capflags.c: $(cpufeature) $(vmxfeature) $(src)/../../x86/kernel/cpu/mkcapflags.sh FORCE
	$(call if_changed,mkcapflags)

quiet_cmd_quote2 = QUOTE   $@
      cmd_quote2 = sed -e '/CONFIG/{'          \
		  -e 's/"CONFIG"//'            \
		  -e 'r $(obj)/config.tmp'     \
		  -e 'a \'                     \
		  -e '""'                      \
		  -e '}'                       \
		  $< > $@
