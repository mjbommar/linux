/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_SEGMENT_H
#define __UM_SEGMENT_H

extern int host_gdt_entry_tls_min;

#define GDT_ENTRY_TLS_ENTRIES 3
#define GDT_ENTRY_TLS_MIN host_gdt_entry_tls_min
#define GDT_ENTRY_TLS_MAX (GDT_ENTRY_TLS_MIN + GDT_ENTRY_TLS_ENTRIES - 1)

/*
 * Segment-selector constants. UML doesn't program the GDT — it
 * runs in host userspace with host-managed segmentation — so the
 * numeric values are never loaded into CS/DS/SS at runtime. They
 * exist only so that x86 headers (nospec-branch.h's
 * x86_clear_cpu_buffers, ptrace.h's user_64bit_mode, ftrace.h's
 * ftrace_regs_cs fabrication) compile when the UML build pulls
 * arch/x86/net/bpf_jit_comp.c and its transitively-included x86
 * headers. Those sites are gated at runtime by cpu_feature_enabled
 * flags UML never sets, or by register-value comparisons against
 * regs UML never constructs.
 *
 * Values match the x86_64 GDT layout in arch/x86/include/asm/
 * segment.h so that any accidental cross-reference (e.g. a trace
 * dump) reads consistently with native x86_64.
 */
#define GDT_ENTRY_KERNEL32_CS		1
#define GDT_ENTRY_KERNEL_CS		2
#define GDT_ENTRY_KERNEL_DS		3
#define GDT_ENTRY_DEFAULT_USER32_CS	4
#define GDT_ENTRY_DEFAULT_USER_DS	5
#define GDT_ENTRY_DEFAULT_USER_CS	6

#define __KERNEL32_CS			(GDT_ENTRY_KERNEL32_CS*8)
#define __KERNEL_CS			(GDT_ENTRY_KERNEL_CS*8)
#define __KERNEL_DS			(GDT_ENTRY_KERNEL_DS*8)
#define __USER32_CS			(GDT_ENTRY_DEFAULT_USER32_CS*8 + 3)
#define __USER_DS			(GDT_ENTRY_DEFAULT_USER_DS*8 + 3)
#define __USER_CS			(GDT_ENTRY_DEFAULT_USER_CS*8 + 3)

#endif
