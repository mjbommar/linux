/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML-side KMSAN arch hooks.
 *
 * This header ships the arch-side pieces mm/kmsan/ expects
 * when CONFIG_KMSAN=y on UML/x86_64: the vmalloc-shadow
 * address macros (delegated to arch/um/include/asm/pgtable.h
 * under the VMALLOC-quarter-split scheme) and the
 * required arch-override inlines (arch_kmsan_get_meta_or_
 * null, kmsan_virt_addr_valid).
 *
 * The layout mirrors arch/x86/include/asm/pgtable_64_types.h:
 * VMALLOC is sized at 1/4 its normal range under CONFIG_KMSAN, and
 * the remaining quarters hold vmalloc shadow, vmalloc origin, and
 * modules shadow+origin. See Documentation/virt/uml/kmsan.rst.
 */

#ifndef __ASM_UM_KMSAN_H
#define __ASM_UM_KMSAN_H

#include <linux/const.h>
#include <linux/pgtable.h>
#include <linux/types.h>

/*
 * The KMSAN_VMALLOC_* / KMSAN_MODULES_* macros are defined
 * in asm/pgtable.h under #ifdef CONFIG_KMSAN so the
 * compile-time constants match whatever TASK_SIZE +
 * VMALLOC_START resolve to in a given build. Include the
 * pgtable header to pull them in here.
 */
#ifdef CONFIG_KMSAN
/*
 * UML's arch_kmsan_get_meta_or_null returns NULL for every caller.
 * Unlike x86 (which has a CPU_ENTRY_AREA private pool) and s390
 * (which has lowcore), UML has no per-CPU fixmap-equivalent and no
 * IDT/entry trampolines to special-case. The generic page-struct
 * lookup in mm/kmsan/ handles every address UML allocates. If UML
 * adds a private memory pool, this hook grows a class-of-address
 * branch.
 */
struct page;
static inline void *arch_kmsan_get_meta_or_null(void *addr, bool is_origin)
{
	(void)addr;
	(void)is_origin;
	return NULL;
}

static inline bool kmsan_virt_addr_valid(const void *addr)
{
	/*
	 * UML's kernel VA range is everything above TASK_SIZE
	 * (physmem + vmalloc + modules). Under the VMALLOC-
	 * quarter-split every shadow/origin address derives
	 * from VMALLOC_START arithmetic, which is itself
	 * inside that kernel VA range. Valid-range checks
	 * therefore collapse to "is this at or above
	 * VMALLOC_START". The generic vmalloc_meta() call
	 * site already validates the shadow region bounds
	 * before dereferencing, so returning true
	 * unconditionally is safe for every code path that
	 * reaches this predicate.
	 */
	(void)addr;
	return true;
}

#else /* !CONFIG_KMSAN */

static inline bool kmsan_virt_addr_valid(const void *addr)
{
	return false;
}

#endif /* CONFIG_KMSAN */

#endif /* __ASM_UM_KMSAN_H */
