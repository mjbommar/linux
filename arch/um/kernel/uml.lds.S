/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/vmlinux.lds.h>
#include <asm/page.h>

OUTPUT_FORMAT(ELF_FORMAT)
OUTPUT_ARCH(ELF_ARCH)
ENTRY(_start)
jiffies = jiffies_64;

VERSION {
  {
    local: *;
  };
}

SECTIONS
{
  /* This must contain the right address - not quite the default ELF one.*/
  PROVIDE (__executable_start = START);
  /* Static binaries stick stuff here, like the sigreturn trampoline,
   * invisibly to objdump.  So, just make __binary_start equal to the very
   * beginning of the executable, and if there are unmapped pages after this,
   * they are forever unusable.
   */
  __binary_start = START;

  . = START + SIZEOF_HEADERS;
  . = ALIGN(PAGE_SIZE);

  _text = .;
  INIT_TEXT_SECTION(0)

  .text      :
  {
    _stext = .;
    __start_text_frozen = .;
    TEXT_TEXT
    SCHED_TEXT
    LOCK_TEXT
    IRQENTRY_TEXT
    SOFTIRQENTRY_TEXT
    *(.fixup)
    /* .gnu.warning sections are handled specially by elf32.em.  */
    *(.gnu.warning)
    *(.gnu.linkonce.t*)
    __end_text_frozen = .;
  }

  /*
   * .um_patch_text holds UML runtime-patchable code: static_branch
   * call sites, future ftrace mcount stubs, kprobe insertion points.
   * Named with a leading `.um` so the generic `.text.*` wildcard does
   * not claim it. Given its own page range so mprotect-writable windows
   * during patching don't expose the bulk of .text. Functions land here
   * via the __patchable_function attribute
   * (arch/um/include/asm/patchable.h). See
   * Documentation/virt/uml/section-split.rst (workstream B-04).
   */
  . = ALIGN(PAGE_SIZE);
  .um_patch_text : {
    __start_um_patch_text = .;
    *(.um_patch_text)
    *(.um_patch_text.*)
    __end_um_patch_text = .;
    . = ALIGN(PAGE_SIZE);
  }

  . = ALIGN(PAGE_SIZE);
  .syscall_stub : {
	__syscall_stub_start = .;
	*(.__syscall_stub*)
	__syscall_stub_end = .;
  }

  /*
   * These are needed even in a static link, even if they wind up being empty.
   * Newer glibc needs these __rel{,a}_iplt_{start,end} symbols.
   */
  .rel.plt : {
	*(.rel.plt)
	PROVIDE_HIDDEN(__rel_iplt_start = .);
	*(.rel.iplt)
	PROVIDE_HIDDEN(__rel_iplt_end = .);
  }
  .rela.plt : {
	*(.rela.plt)
	PROVIDE_HIDDEN(__rela_iplt_start = .);
	*(.rela.iplt)
	PROVIDE_HIDDEN(__rela_iplt_end = .);
  }

  #include <asm/common.lds.S>

  __init_begin = .;
  init.data : { INIT_DATA }
  __init_end = .;

  .data    :
  {
    INIT_TASK_DATA(KERNEL_STACK_SIZE)
    DATA_DATA
    *(.gnu.linkonce.d*)
    CONSTRUCTORS
  }
  .data1   : { *(.data1) }
  .ctors         :
  {
    *(.ctors)
  }
  .dtors         :
  {
    *(.dtors)
  }

  .got           : { *(.got.plt) *(.got) }
  .eh_frame       : { KEEP (*(.eh_frame)) }
  .dynamic       : { *(.dynamic) }
  .tdata	  : { *(.tdata .tdata.* .gnu.linkonce.td.*) }
  .tbss		  : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) }
  /* We want the small data sections together, so single-instruction offsets
     can access them all, and initialized data all before uninitialized, so
     we can shorten the on-disk segment size.  */
  .sdata     : { *(.sdata) }
  _edata  =  .;
  PROVIDE (edata = .);
  . = ALIGN(PAGE_SIZE);
  __bss_start = .;
  PROVIDE(_bss_start = .);
  SBSS(0)
  BSS(0)
   __bss_stop = .;
  _end = .;
  PROVIDE (end = .);

  STABS_DEBUG
  DWARF_DEBUG
  MODINFO
  ELF_DETAILS

  DISCARDS
}
