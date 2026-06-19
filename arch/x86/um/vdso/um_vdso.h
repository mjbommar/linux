/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UM_VDSO_DATA_H
#define _ASM_X86_UM_VDSO_DATA_H

/*
 * Shared between the UML kernel (writer, arch/x86/um/vdso/vma.c, updated each
 * timer tick) and the vDSO (reader, arch/x86/um/vdso/um_vdso.c). Lets the
 * guest answer clock_gettime(CLOCK_*_COARSE) from a mapped page with no
 * syscall/trap at all. Coarse clocks are defined at tick resolution, so a
 * per-tick snapshot is exact. Lives in the page directly below the vDSO code.
 */
struct um_vdso_data {
	unsigned int seq;	/* seqlock; odd = update in progress */
	unsigned int valid;	/* 1 once published (and not time-travel) */
	long mono_coarse_sec;
	long mono_coarse_nsec;
	long real_coarse_sec;
	long real_coarse_nsec;
};

#endif
