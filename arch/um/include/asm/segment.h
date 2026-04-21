/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML has no segmentation (runs as a host-userspace ELF). The
 * x86 __KERNEL_DS / __KERNEL_CS segment selectors the native
 * kernel uses (e.g. for the BHB-mitigation verw sequence in
 * arch/x86/include/asm/nospec-branch.h) are meaningless here.
 * Stub to zero so consumers compile; UML's nospec code paths
 * are no-ops because UML doesn't take hardware ring
 * transitions.
 *
 * Workstream C-06 shim per D43 fifth-view.
 */
#ifndef _ASM_UM_SEGMENT_H
#define _ASM_UM_SEGMENT_H

#define __KERNEL_CS	0
#define __KERNEL_DS	0
#define __USER_CS	0
#define __USER_DS	0

#endif /* _ASM_UM_SEGMENT_H */
