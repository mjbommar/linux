/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __MEM_H__
#define __MEM_H__

extern int phys_mapping(unsigned long phys, unsigned long long *offset_out);

/*
 * UML address-space anchors.
 *
 * UML tracks two address-space bases:
 *
 * - uml_physmem: kernel direct-map base in the *guest pgd*.
 *   Used by __pa()/__va() and PAGE_OFFSET. Backends that run directly
 *   in the host process can use the low host VA here; KVM-backed
 *   execution can use a guest-pgd direct map independent of the host
 *   mapping address.
 *
 * - __binary_start_hva: host VA where UML's pages physically live.
 *   Used by call sites that compute offsets into the host process's
 *   mmap'd physmem region (mmap targets, vhost-user/VFIO offsets,
 *   range checks against the host process layout).
 *
 * Sites that mean "host VA where UML's pages live" should use
 * __binary_start_hva. Sites that mean "kernel pgd direct-map base"
 * should use uml_physmem / PAGE_OFFSET.
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
