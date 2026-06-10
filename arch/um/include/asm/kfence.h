/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_UM_KFENCE_H
#define __ASM_UM_KFENCE_H

/*
 * UML KFENCE support.
 *
 * KFENCE is a sampling allocator that places some kmalloc'd objects
 * into a separate "pool" of pages interleaved with guard pages. The
 * guard pages are made unreadable so any out-of-bounds access from
 * the object page faults into kfence_handle_page_fault(), which
 * reports the error and then makes the page readable again so the
 * kernel can continue.
 *
 * Arch contract (from Documentation/dev-tools/kfence.rst):
 *   - arch_kfence_init_pool(): one-time check that the pool is
 *     backed by small (PAGE_SIZE) pages. UML only has 4 KiB pages
 *     (CONFIG_HAVE_PAGE_SIZE_4KB is always selected in
 *     arch/um/Kconfig) and never promotes to hugepages for kernel
 *     .data, so this is unconditionally true.
 *   - kfence_protect_page(addr, protect): set the given page
 *     unreadable (protect=true) or readable+writable (protect=false).
 *     On UML the kernel address space is just a host mmap; UML uses
 *     os_protect_memory() / mprotect() to toggle PROT_NONE to
 *     PROT_READ|PROT_WRITE on a single page.
 *
 * Fault integration is in arch/um/kernel/trap.c::segv; see that
 * file for the kfence_handle_page_fault call.
 */

#ifndef MODULE

#include <linux/compiler.h>
#include <linux/kfence.h>
#include <os.h>

static inline bool arch_kfence_init_pool(void)
{
	/*
	 * UML uses only 4 KiB pages; no arch-level hugepage promotion
	 * to worry about. KFENCE's requirement that pool pages be
	 * PAGE_SIZE-granular is therefore always met.
	 */
	return true;
}

static inline bool kfence_protect_page(unsigned long addr, bool protect)
{
	int r = protect ? 0 : 1;
	int w = protect ? 0 : 1;
	int err;

	/*
	 * PROT_NONE (r=w=x=0) makes subsequent access fault; the
	 * resulting SIGSEGV is delivered into segv() which hands off
	 * to kfence_handle_page_fault(). protect=false restores
	 * PROT_READ|PROT_WRITE so KFENCE can use the page as a normal
	 * object slot. The executable bit stays off in both cases;
	 * KFENCE pool pages never hold code.
	 */
	err = os_protect_memory((void *)addr, PAGE_SIZE, r, w, 0);
	return err == 0;
}

#endif /* !MODULE */

#endif /* __ASM_UM_KFENCE_H */
