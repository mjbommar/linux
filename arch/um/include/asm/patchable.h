/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_PATCHABLE_H
#define __ASM_UM_PATCHABLE_H

/*
 * UML section split (workstream B-04): runtime-patchable code lives
 * in the dedicated .um_patch_text section so that mprotect-writable
 * windows used for text poking do not expose the bulk of .text.
 *
 * Mark a function as patchable by decorating its definition:
 *
 *   void __patchable_function my_fn(void) { ... }
 *
 * The attribute is a layout hint only — the function still behaves
 * like any other kernel function. When a future workstream enables
 * jump-label JIT patching, mcount/ftrace, or kprobes, marked
 * functions are the ones whose text the patching machinery will
 * modify.
 *
 * See Documentation/virt/uml/section-split.rst for the layout and
 * the constraints on what can be placed here.
 */

#include <linux/compiler.h>

#define __patchable_function __section(".um_patch_text")

/* Linker-provided bounds of the frozen and patchable regions. For the
 * patchable region, in a build where no function is decorated yet the
 * region is empty and the two symbols are equal.
 */
extern char __start_text_frozen[];
extern char __end_text_frozen[];
extern char __start_um_patch_text[];
extern char __end_um_patch_text[];

/* mprotect helpers. Scope is a [start, end) range that must lie
 * entirely within [__start_um_patch_text, __end_um_patch_text). The
 * begin/end calls are not re-entrant; callers must serialize on
 * text_mutex or an equivalent lock.
 *
 * Returns 0 on success, -errno on mprotect failure.
 */
int um_text_patch_begin(void *addr, unsigned long len);
int um_text_patch_end(void *addr, unsigned long len);

/* Called once from mark_rodata_ro() after initcalls complete. Logs the
 * frozen + patchable boundaries via pr_info; a belt-and-suspenders
 * mprotect(RX) is already applied by the host ELF loader, so this
 * function is informational today. Future work (full jump-label JIT)
 * will use it to transition patchable text from RX → RWX → RX across
 * the initial batch of boot-time patches.
 */
void um_section_split_finalize(void);

#endif /* __ASM_UM_PATCHABLE_H */
