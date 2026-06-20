#
# This file is included by the global makefile so that you can add your own
# architecture-specific flags and dependencies.
#
# Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
# Licensed under the GPL
#

# select defconfig based on actual architecture
ifeq ($(SUBARCH),x86)
  ifeq ($(shell uname -m),x86_64)
        KBUILD_DEFCONFIG := x86_64_defconfig
  else
        KBUILD_DEFCONFIG := i386_defconfig
  endif
else
        KBUILD_DEFCONFIG := $(SUBARCH)_defconfig
endif

ARCH_DIR := arch/um
# We require bash because the vmlinux link and loader script cpp use bash
# features.
SHELL := bash

MODE_INCLUDE	+= -I$(srctree)/$(ARCH_DIR)/include/shared/skas

HEADER_ARCH 	:= $(SUBARCH)

ifneq ($(filter $(SUBARCH),x86 x86_64 i386),)
	HEADER_ARCH := x86
endif

ifdef CONFIG_64BIT
	KBUILD_CFLAGS += -mcmodel=large
endif

# Ftrace call-site emission.
# Under -mcmodel=large, GCC's -pg -mfentry expands to a 33-byte
# indirect call sequence that recordmcount cannot recognize. Use
# -fpatchable-function-entry=5,0 instead: the compiler emits a
# 5-byte NOP at function entry and records the address in
# __patchable_function_entries. Matches arm64/riscv/parisc.
ifeq ($(CONFIG_DYNAMIC_FTRACE),y)
  KBUILD_CPPFLAGS += -DCC_USING_PATCHABLE_FUNCTION_ENTRY
  CC_FLAGS_FTRACE := -fpatchable-function-entry=5,0
endif

HOST_DIR := arch/$(HEADER_ARCH)

include $(srctree)/$(ARCH_DIR)/Makefile-skas
include $(srctree)/$(HOST_DIR)/Makefile.um

core-y += $(HOST_DIR)/um/

# Pull arch/x86/net/ into the UML build when BPF JIT is configured.
# UML-local shim headers in
# arch/um/include/asm/ + arch/um/kernel/x86_nops.c +
# bpf_jit_comp.c's CONFIG_UM reg2pt_regs[] branch satisfy the
# x86-host divergences.
core-$(CONFIG_BPF_JIT) += $(HOST_DIR)/net/

SHARED_HEADERS	:= $(ARCH_DIR)/include/shared
ARCH_INCLUDE	:= -I$(srctree)/$(SHARED_HEADERS)
ARCH_INCLUDE	+= -I$(srctree)/$(HOST_DIR)/um/shared
KBUILD_CPPFLAGS += -I$(srctree)/$(HOST_DIR)/um

# -Dstrrchr=kernel_strrchr (as well as the various in6addr symbols) prevents
#  anything from referencing
# libc symbols with the same name, which can cause a linker error.
#
# -Dlongjmp=kernel_longjmp prevents anything from referencing the libpthread.a
# embedded copy of longjmp, same thing for setjmp.
#
# These apply to USER_CFLAGS too.

KBUILD_CFLAGS += $(CFLAGS) $(CFLAGS-y) -D__arch_um__ \
	$(ARCH_INCLUDE) $(MODE_INCLUDE)	\
	-Dlongjmp=kernel_longjmp -Dsetjmp=kernel_setjmp \
	-Din6addr_loopback=kernel_in6addr_loopback \
	-Din6addr_any=kernel_in6addr_any -Dstrrchr=kernel_strrchr \
	-D__close_range=kernel__close_range

KBUILD_RUSTFLAGS += -Crelocation-model=pie

KBUILD_AFLAGS += $(ARCH_INCLUDE)

USER_CFLAGS = $(patsubst $(KERNEL_DEFINES),,$(patsubst -I%,,$(KBUILD_CFLAGS))) \
		$(ARCH_INCLUDE) $(MODE_INCLUDE) $(filter -I%,$(CFLAGS)) \
		-D_FILE_OFFSET_BITS=64 -idirafter $(srctree)/include \
		-idirafter $(objtree)/include -D__KERNEL__ -D__UM_HOST__ \
		-include $(srctree)/include/linux/compiler-version.h \
		-include $(srctree)/include/linux/kconfig.h \
		-idirafter $(ARCH_DIR)/include/generated

#This will adjust *FLAGS accordingly to the platform.
include $(srctree)/$(ARCH_DIR)/Makefile-os-Linux

KBUILD_CPPFLAGS += -I$(srctree)/$(HOST_DIR)/include \
		   -I$(srctree)/$(HOST_DIR)/include/uapi \
		   -I$(objtree)/$(HOST_DIR)/include/generated \
		   -I$(objtree)/$(HOST_DIR)/include/generated/uapi

# -Derrno=kernel_errno - This turns all kernel references to errno into
# kernel_errno to separate them from the libc errno.  This allows -fno-common
# in KBUILD_CFLAGS.  Otherwise, it would cause ld to complain about the two different
# errnos.
# These apply to kernelspace only.
#
# strip leading and trailing whitespace to make the USER_CFLAGS removal of these
# defines more robust

KERNEL_DEFINES = $(strip -Derrno=kernel_errno -Dsigprocmask=kernel_sigprocmask \
			 -Dmktime=kernel_mktime $(ARCH_KERNEL_DEFINES))
KBUILD_CFLAGS += $(KERNEL_DEFINES)

PHONY += linux

all: linux

linux: vmlinux
	@echo '  LINK $@'
	$(Q)ln -f $< $@

# --- uml/<profile> configure targets -------------------------------
#
# Merge arch/um/configs/base_defconfig with
# arch/um/configs/profiles/<profile>.config into .config, then let
# the user run `make ARCH=um -j N` to build. Same workflow as the
# kernel's standard defconfig path.

UML_PROFILES := prod-fast prod-with-hooks research research-kmsan \
		fuzz fuzz-deep race sandbox embedded time-travel

PHONY += $(addprefix uml/,$(UML_PROFILES)) uml/list-profiles

$(addprefix uml/,$(UML_PROFILES)): uml/%:
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/kconfig/merge_config.sh \
		-m -O $(objtree) \
		$(srctree)/arch/um/configs/base_defconfig \
		$(srctree)/arch/um/configs/profiles/$*.config
	$(Q)$(MAKE) -f $(srctree)/Makefile ARCH=um olddefconfig
	@echo
	@echo 'Configured for UML profile: $*'
	@echo 'See Documentation/virt/uml/profiles/$*.rst for details.'
	@echo 'Now run:  make ARCH=um -j$$(nproc)'

uml/list-profiles:
	@echo 'Available UML profiles (use: make ARCH=um uml/<profile>):'
	@for p in $(UML_PROFILES); do echo "    uml/$$p"; done
	@echo
	@echo 'Per-profile documentation in Documentation/virt/uml/profiles/.'

define archhelp
  echo '* linux		  - Binary kernel image (./linux) - for backward'
  echo '		    compatibility only, this creates a hard link to the'
  echo '		    real kernel binary, the "vmlinux" binary you'
  echo '		    find in the kernel root.'
  echo '  uml/<profile>   - Configure for a named UML profile'
  echo '                    (prod-fast, prod-with-hooks, research,'
  echo '                    research-kmsan, fuzz, fuzz-deep, race,'
  echo '                    sandbox, embedded, time-travel).'
  echo '                    See Documentation/virt/uml/profiles/.'
  echo '  uml/list-profiles - List available UML profile targets.'
endef

archheaders:
	$(Q)$(MAKE) -f $(srctree)/Makefile ARCH=$(HEADER_ARCH) asm-generic archheaders

archprepare:
	$(Q)$(MAKE) $(build)=$(HOST_DIR)/um include/generated/user_constants.h

LINK-$(CONFIG_LD_SCRIPT_STATIC) += -static
LINK-$(CONFIG_LD_SCRIPT_DYN) += -no-pie
LINK-$(CONFIG_LD_SCRIPT_DYN_RPATH) += -Wl,-rpath,/lib

CFLAGS_NO_HARDENING := $(call cc-option, -fno-PIC,) $(call cc-option, -fno-pic,) \
	-fno-stack-protector $(call cc-option, -fno-stack-protector-all)

# Options used by linker script
export LDS_START      := $(START)
export LDS_ELF_ARCH   := $(ELF_ARCH)
export LDS_ELF_FORMAT := $(ELF_FORMAT)

# The wrappers will select whether using "malloc" or the kernel allocator.
LINK_WRAPS = -Wl,--wrap,malloc -Wl,--wrap,free -Wl,--wrap,calloc

# Avoid binutils 2.39+ warnings by marking the stack non-executable and
# ignorning warnings for the kallsyms sections.
LDFLAGS_EXECSTACK = -z noexecstack
ifeq ($(CONFIG_LD_IS_BFD),y)
LDFLAGS_EXECSTACK += $(call ld-option,--no-warn-rwx-segments)
endif

LD_FLAGS_CMDLINE = $(foreach opt,$(KBUILD_LDFLAGS) $(LDFLAGS_EXECSTACK),-Wl,$(opt))

# Used by link-vmlinux.sh which has special support for um link
export CFLAGS_vmlinux := $(LINK-y) $(LINK_WRAPS) $(LD_FLAGS_CMDLINE) $(CC_FLAGS_LTO)

# When cleaning we don't include .config, so we don't include
# TT or skas makefiles and don't clean skas_ptregs.h.
CLEAN_FILES += linux x.i gmon.out
MRPROPER_FILES += $(HOST_DIR)/include/generated

archclean:
	@find . \( -name '*.bb' -o -name '*.bbg' -o -name '*.da' \
		-o -name '*.gcov' \) -type f -print | xargs rm -f
	$(Q)$(MAKE) -f $(srctree)/Makefile ARCH=$(HEADER_ARCH) clean

export HEADER_ARCH SUBARCH USER_CFLAGS CFLAGS_NO_HARDENING DEV_NULL_PATH
