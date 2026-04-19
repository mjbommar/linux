// SPDX-License-Identifier: GPL-2.0
/*
 * UML dynamic ftrace patching (workstream C-05).
 *
 * The compiler emits a 5-byte NOP at every traced function's entry
 * via -fpatchable-function-entry=5,0 (see arch/um/Makefile and
 * decisions-log D29). This file implements the arch hooks that
 * patch those NOPs to `call ftrace_caller` and back.
 *
 * Patch mechanism (decisions-log D28 + D30):
 *   1. text_mutex is held by the generic ftrace core across the
 *      arch_ftrace_update_code() callback (see
 *      ftrace_arch_code_modify_prepare()).
 *   2. arch_ftrace_update_code() runs inside stop_machine_cpuslocked()
 *      so no peer UML vCPU host thread runs kernel code during the
 *      patch window.
 *   3. Inside the stop_machine callback we mprotect the kernel text
 *      image [_text, _etext) RW **once**, call ftrace_modify_all_code()
 *      which iterates every record and invokes our per-site memcpy
 *      helpers, then mprotect back to RX **once**. Two mprotect
 *      syscalls per enable/disable, regardless of how many records
 *      are patched. Per-site mprotect would turn one enable into
 *      2 * N syscalls (N ≈ 21000) and cumulative VMA fragmentation
 *      hangs the host process (D30).
 *
 * UML cannot replicate arm/arm64/riscv's fixmap-alias or x86's
 * text_poke_mm pattern because UML has exactly one mm (its host
 * process's). stop_machine + bulk mprotect is the closest analogue:
 * no peer kernel path observes the transient RW window.
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
#include <asm/sections.h>

#include <os.h>

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

/*
 * Per-record patchers. Invoked from ftrace_modify_all_code() which
 * we run inside arch_ftrace_update_code()'s stop_machine callback,
 * where [_text, _etext) is already mprotect'd RW. Plain memcpy.
 */

int ftrace_init_nop(struct module *mod, struct dyn_ftrace *rec)
{
	/*
	 * -fpatchable-function-entry=5,0 places the 5-byte NOP at
	 * compile time, so init has nothing to write. Skipping this
	 * avoids per-record work during ftrace_init() entirely; any
	 * drift (corrupted build, in-tree poisoning) will be caught
	 * by the generic verification path on first ftrace_make_call.
	 */
	return 0;
}

int ftrace_make_nop(struct module *mod, struct dyn_ftrace *rec,
		    unsigned long addr)
{
	memcpy((void *)rec->ip, um_ftrace_nop5, CALL_INSN_SIZE);
	return 0;
}

int ftrace_make_call(struct dyn_ftrace *rec, unsigned long addr)
{
	u8 new[CALL_INSN_SIZE];

	um_ftrace_build_call(new, rec->ip, addr);
	memcpy((void *)rec->ip, new, CALL_INSN_SIZE);
	return 0;
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
	memcpy((void *)site, new, CALL_INSN_SIZE);
	return 0;
}

static int um_ftrace_update_code_cb(void *data)
{
	int *command = data;
	unsigned long text_len = (unsigned long)_etext - (unsigned long)_text;
	int err;

	/*
	 * stop_machine dispatches to every online CPU in parallel; only
	 * one needs to do the work. cpumask_first() picks a stable CPU
	 * regardless of caller. Other CPUs' callback invocations return
	 * 0 immediately but stay frozen until the chosen CPU returns,
	 * preserving the no-peer-execution property.
	 */
	if (smp_processor_id() != cpumask_first(cpu_online_mask))
		return 0;

	err = os_protect_memory(_text, text_len, 1, 1, 1);
	if (err)
		return err;

	ftrace_modify_all_code(*command);

	return os_protect_memory(_text, text_len, 1, 0, 1);
}

void arch_ftrace_update_code(int command)
{
	stop_machine_cpuslocked(um_ftrace_update_code_cb, &command,
				cpu_online_mask);
}
