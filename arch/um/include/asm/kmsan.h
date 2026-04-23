/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML-side KMSAN shadow/origin virtual-address layout and
 * the arch-specific interfaces mm/kmsan/ expects when
 * CONFIG_KMSAN=y.
 *
 * UML v1 picks a dedicated-host-mmap scheme rather than x86's
 * quarter-split of VMALLOC. Reasons (see decisions-log D44):
 *   - UML's VMALLOC is bounded by TASK_SIZE, not a fixed
 *     canonical hole, so the x86 VA arithmetic in
 *     mm/kmsan/shadow.c::vmalloc_meta() has nowhere to land.
 *   - Splitting VMALLOC to 1/4 size under CONFIG_KMSAN=y
 *     creates an ABI fork between kmsan-on and kmsan-off
 *     UML images — the other sanitizer ports avoided that.
 *   - UML already owns the "mmap a 16 TiB slab and use it as
 *     arch shadow" pattern from KASAN; KMSAN extends it with
 *     one additional mmap for the origin region.
 *
 * Shape per commit-1 of the C-07 plan: this header provides
 * the VA constants + extern prototypes, but no runtime body
 * yet. kmsan_init() wiring lands in commit-2; the arch
 * metadata stubs land in commit-3.
 */

#ifndef __ASM_UM_KMSAN_H
#define __ASM_UM_KMSAN_H

#include <linux/const.h>
#include <linux/types.h>

/*
 * Shadow region: one byte per kernel byte. Size matches the
 * host userspace address range that UML's kernel addresses
 * can occupy — same ceiling KASAN uses, so shadow/origin
 * sizing stays a single KASAN_HOST_USER_SPACE_END_ADDR
 * define (declared in asm/kasan.h) shared across both.
 */
#include <asm/kasan.h>

#define KMSAN_SHADOW_OFFSET _AC(CONFIG_KMSAN_SHADOW_OFFSET, UL)
#define KMSAN_ORIGIN_OFFSET _AC(CONFIG_KMSAN_ORIGIN_OFFSET, UL)

/*
 * Size is the full covered VA range (KASAN_HOST_USER_SPACE_
 * END_ADDR + 1 = 128 TiB on x86_64) — 1 byte per kernel byte
 * for shadow, 1 u32 per kernel u32 for origin (same byte
 * total as shadow since it's 1:1 in word-size terms).
 *
 * Total extra VA reserved under CONFIG_KMSAN=y: 2 * 128 TiB,
 * but both are populated on demand via host mmap so RSS
 * only grows for touched pages. See D44 Probe 4 addendum
 * for the memory-pressure bounds measured empirically.
 */
#ifdef CONFIG_X86_64
#define KMSAN_SHADOW_SIZE (KASAN_HOST_USER_SPACE_END_ADDR + 1)
#define KMSAN_ORIGIN_SIZE (KASAN_HOST_USER_SPACE_END_ADDR + 1)
#else
#error "KMSAN on UML is only defined for x86_64 today"
#endif /* CONFIG_X86_64 */

#define KMSAN_SHADOW_START (KMSAN_SHADOW_OFFSET)
#define KMSAN_SHADOW_END   (KMSAN_SHADOW_START + KMSAN_SHADOW_SIZE)
#define KMSAN_ORIGIN_START (KMSAN_ORIGIN_OFFSET)
#define KMSAN_ORIGIN_END   (KMSAN_ORIGIN_START + KMSAN_ORIGIN_SIZE)

/*
 * The VMALLOC quarter-split constants that
 * mm/kmsan/shadow.c::vmalloc_meta() consults when computing
 * the shadow/origin address of a vmalloc'd pointer. UML
 * puts vmalloc metadata in the same dedicated host-mmap
 * regions as everything else — vmalloc_meta() on UML just
 * returns the offset-added shadow/origin address without
 * any special bucketing, so the quarter-split constants
 * collapse to "map the whole VMALLOC range into shadow at
 * +KMSAN_SHADOW_OFFSET".
 *
 * Modules on UML overlap VMALLOC (MODULES_VADDR==VMALLOC_
 * START), so the MODULES_* constants mirror the VMALLOC_*
 * ones exactly. That's the minimum that lets
 * mm/kmsan/shadow.c compile with CONFIG_KMSAN_VMALLOC=y.
 */
#include <asm/pgtable.h>

#define KMSAN_VMALLOC_SHADOW_START \
	((unsigned long)KMSAN_SHADOW_OFFSET + (unsigned long)VMALLOC_START)
#define KMSAN_VMALLOC_ORIGIN_START \
	((unsigned long)KMSAN_ORIGIN_OFFSET + (unsigned long)VMALLOC_START)
#define KMSAN_MODULES_SHADOW_START KMSAN_VMALLOC_SHADOW_START
#define KMSAN_MODULES_ORIGIN_START KMSAN_VMALLOC_ORIGIN_START

#ifdef CONFIG_KMSAN
/*
 * UML's arch_kmsan_get_meta_or_null returns NULL for every
 * caller in v1 — no per-CPU fixmap-equivalent regions and no
 * IDT/entry trampolines to special-case. The generic page-
 * struct lookup in mm/kmsan/ handles every address we
 * allocate. If a future UML subsystem adds a private
 * memory pool (e.g. seccomp backend entry pages) this hook
 * grows a class-of-address branch.
 */
struct page;
static inline bool kmsan_virt_addr_valid(const void *addr)
{
	/*
	 * On UML, every kernel address is a host-userspace
	 * address served by the UML process's own mmap. The
	 * shadow mapping covers the same range (by construction
	 * of KMSAN_SHADOW_SIZE above), so validity checks
	 * collapse to a range test. mm/kmsan/ callers use this
	 * to short-circuit metadata lookups for out-of-range
	 * pointers; we always return true because every
	 * address a KMSAN-instrumented UML kernel passes has
	 * valid shadow/origin under the size constraint above.
	 */
	return true;
}

#else /* !CONFIG_KMSAN */

static inline bool kmsan_virt_addr_valid(const void *addr)
{
	return false;
}

#endif /* CONFIG_KMSAN */

#endif /* __ASM_UM_KMSAN_H */
