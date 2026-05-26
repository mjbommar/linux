// SPDX-License-Identifier: GPL-2.0
/*
 * UML-local definition of the x86 NOP-sequence table.
 *
 * arch/x86/kernel/alternative.c defines x86_nops[] for bare-metal
 * x86 builds. UML doesn't compile arch/x86/kernel/ (it has its
 * own entry path), but consumers like arch/x86/net/bpf_jit_comp.c
 * (BPF JIT, workstream C-06) reference the table to emit padding
 * between BPF instructions. Provide a UML-local copy matching the
 * upstream sequences so the JIT emits correct code.
 *
 * Per D43 fifth-view + D45 fork-scope policy.
 */
#include <linux/export.h>
#include <linux/types.h>
#include <asm/nops.h>

/*
 * Single-byte NOP (0x90) up to 8-byte NOP. These are the classic
 * sequences intel recommends for code padding. Table layout
 * matches arch/x86/kernel/alternative.c's x86_nops[] so JIT
 * consumers that index by length get byte-for-byte-equivalent
 * output on UML.
 */
static const unsigned char nop1[]  = { 0x90 };
static const unsigned char nop2[]  = { 0x66, 0x90 };
static const unsigned char nop3[]  = { 0x0f, 0x1f, 0x00 };
static const unsigned char nop4[]  = { 0x0f, 0x1f, 0x40, 0x00 };
static const unsigned char nop5[]  = { 0x0f, 0x1f, 0x44, 0x00, 0x00 };
static const unsigned char nop6[]  = { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 };
static const unsigned char nop7[]  = { 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00 };
static const unsigned char nop8[]  = { 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };

const unsigned char * const x86_nops[ASM_NOP_MAX + 1] = {
	NULL,
	nop1, nop2, nop3, nop4, nop5, nop6, nop7, nop8,
};
EXPORT_SYMBOL_GPL(x86_nops);
