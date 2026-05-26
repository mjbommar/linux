/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __MEM_H__
#define __MEM_H__

extern int phys_mapping(unsigned long phys, unsigned long long *offset_out);

/*
 * UML address-space anchors (memo 25 R1 abstraction).
 *
 * Two distinct concepts that today happen to be equal but will
 * diverge under the v2 KVM backend (memo 26 Phase B):
 *
 * - `uml_physmem`: kernel direct-map base in the *guest pgd*.
 *   Used by __pa()/__va() and PAGE_OFFSET. Today equals
 *   `__binary_start & PAGE_MASK` (low host VA) because seccomp's
 *   kernel runs directly in the host process. Under v2 (when the
 *   per-mm pgd's kernel half lives at PML4[256+]) this becomes a
 *   high constant — the guest CPU walks `mm->pgd` and finds kernel
 *   pages there, while the host process keeps its memory wherever.
 *
 * - `__binary_start_hva`: host VA where UML's pages physically live.
 *   Used by call sites that compute offsets into the host process's
 *   mmap'd physmem region (mmap targets, vhost-user / VFIO offsets,
 *   range checks against the host process layout). Today equals
 *   uml_physmem; under v2 it stays at the low host VA while
 *   uml_physmem moves high.
 *
 * Sites that mean "host VA where UML's pages live" should use
 * `__binary_start_hva`. Sites that mean "kernel pgd direct-map base"
 * should use `uml_physmem` / `PAGE_OFFSET`. Today the rename is the
 * abstraction; the runtime split lands when v2 flips the Kconfig
 * gate in arch/um/include/asm/page.h.
 */
extern unsigned long uml_physmem;
extern unsigned long __binary_start_hva;

static inline unsigned long uml_to_phys(void *virt)
{
	return(((unsigned long) virt) - uml_physmem);
}

static inline void *uml_to_virt(unsigned long phys)
{
	return((void *) uml_physmem + phys);
}

#endif
