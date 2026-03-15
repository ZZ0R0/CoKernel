/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COKERNEL_PAGETABLE_H
#define _COKERNEL_PAGETABLE_H

#include <linux/types.h>

/*
 * Build a standalone set of page tables (PGD→P4D→PUD→PMD→PTE)
 * that maps the co-kernel's physical pages at COKERNEL_VIRT_BASE.
 *
 * Also maps the trampoline page at its Linux VA so the PMI handler
 * can execute seamlessly across CR3 switches.
 *
 * Also maps the LAPIC MMIO page for EOI.
 *
 * Returns 0 on success.
 */
int ck_build_pagetables(void *region_base, phys_addr_t phys_base,
                        unsigned long size,
                        void *trampoline_va, phys_addr_t trampoline_phys);

/* Get the physical address of the PGD — for loading into CR3 */
phys_addr_t ck_get_pgd_phys(void);

#endif /* _COKERNEL_PAGETABLE_H */
