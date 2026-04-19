// SPDX-License-Identifier: GPL-2.0
/*
 * UML dynamic ftrace patching (workstream C-05).
 *
 * The compiler emits a 5-byte NOP at every traced function's entry
 * via -fpatchable-function-entry=5,0 (see arch/um/Makefile and
 * decisions-log D29). This file implements the arch hooks that
 * patch those NOPs to `call ftrace_caller` and back.
 *
 * The patch mechanism is deliberately simple (decisions-log D28):
 *   1. text_mutex is already held by the generic ftrace core.
 *   2. arch_ftrace_update_code() wraps ftrace_modify_all_code() in
 *      stop_machine_cpuslocked() so no peer UML vCPU host thread
 *      runs kernel code during the window.
 *   3. Each per-record patch uses um_kernel_text_patch_begin/end()
 *      (workstream B-04 extension) to mprotect exactly the one
 *      PAGE_SIZE page containing the patch site RW, memcpy 5 bytes,
 *      and mprotect it back to RX.
 *
 * UML cannot replicate the arm/arm64/riscv fixmap-alias or x86
 * text_poke_mm pattern because UML has exactly one mm (its host
 * process's). stop_machine is what guarantees no peer kernel path
 * observes the transient RW window.
 *
 * Function graph is deferred to a follow-up per D27; this file
 * intentionally does not implement prepare_ftrace_return() or any
 * return-trampoline machinery.
 */

#include <linux/ftrace.h>
#include <linux/memory.h>
#include <linux/stop_machine.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <asm/patchable.h>

#define CALL_INSN_OPCODE	0xe8
#define CALL_INSN_SIZE		5	/* = MCOUNT_INSN_SIZE */

/* Standard 5-byte x86_64 NOP: nopl 0(%rax,%rax,1). Same encoding
 * emitted by GCC under -fpatchable-function-entry=5,0.
 */
static const u8 um_ftrace_nop5[CALL_INSN_SIZE] = {
	0x0f, 0x1f, 0x44, 0x00, 0x00
};

static void um_ftrace_build_call(u8 buf[CALL_INSN_SIZE],
				 unsigned long site, unsigned long target)
{
	s32 disp = (s32)(target - (site + CALL_INSN_SIZE));

	buf[0] = CALL_INSN_OPCODE;
	memcpy(&buf[1], &disp, sizeof(disp));
}

static int um_ftrace_write(unsigned long site, const u8 buf[CALL_INSN_SIZE])
{
	int err;

	err = um_kernel_text_patch_begin((void *)site, CALL_INSN_SIZE);
	if (err)
		return err;

	memcpy((void *)site, buf, CALL_INSN_SIZE);

	err = um_kernel_text_patch_end((void *)site, CALL_INSN_SIZE);
	return err;
}

int ftrace_make_nop(struct module *mod, struct dyn_ftrace *rec,
		    unsigned long addr)
{
	return um_ftrace_write(rec->ip, um_ftrace_nop5);
}

int ftrace_make_call(struct dyn_ftrace *rec, unsigned long addr)
{
	u8 new[CALL_INSN_SIZE];

	um_ftrace_build_call(new, rec->ip, addr);
	return um_ftrace_write(rec->ip, new);
}

/*
 * ftrace_modify_call() is only required when CONFIG_DYNAMIC_FTRACE_WITH_REGS
 * or *_WITH_CALL_OPS or *_WITH_DIRECT_CALLS is set. UML enables none of
 * those in C-05; the generic static-inline stub in <linux/ftrace.h>
 * returns -EINVAL and is never reached.
 */

int ftrace_update_ftrace_func(ftrace_func_t func)
{
	unsigned long site = (unsigned long)&ftrace_call;
	u8 new[CALL_INSN_SIZE];

	um_ftrace_build_call(new, site, (unsigned long)func);
	return um_ftrace_write(site, new);
}

static int um_ftrace_update_code_cb(void *data)
{
	int *command = data;

	ftrace_modify_all_code(*command);
	return 0;
}

void arch_ftrace_update_code(int command)
{
	/*
	 * Freeze every peer UML vCPU host thread while we patch, so no
	 * other kernel path observes the transient RW window on the
	 * target page. See decisions-log D28 for why this is required
	 * on UML even though the single-mm bare-metal analogues
	 * (fixmap, text_poke_mm) are unavailable here.
	 *
	 * text_mutex is held by the generic ftrace core across this
	 * callback (see ftrace_arch_code_modify_prepare()).
	 */
	stop_machine_cpuslocked(um_ftrace_update_code_cb, &command, NULL);
}
