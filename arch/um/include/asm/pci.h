/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_UM_PCI_H
#define __ASM_UM_PCI_H
#include <linux/types.h>
#include <asm/io.h>

/* Generic PCI */
#include <asm-generic/pci.h>

#ifdef CONFIG_PCI_MSI
/*
 * UML only has the virt-pci root, so one arch-level fwnode hook is
 * enough for MSI support.
 */
void *pci_root_bus_fwnode(struct pci_bus *bus);
#define pci_root_bus_fwnode	pci_root_bus_fwnode
#endif

#endif  /* __ASM_UM_PCI_H */
