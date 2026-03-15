/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COKERNEL_MEMORY_H
#define _COKERNEL_MEMORY_H

#include <linux/types.h>
#include <linux/mm.h>

/* Allocate COKERNEL_TOTAL_SIZE of invisible memory */
int ck_alloc_invisible_memory(void);

/* Allocate a separate executable page for the PMI trampoline.
 * This page stays in the normal direct map (needed for IDT). */
int ck_alloc_trampoline_page(void);

/* Get the base struct page pointer */
struct page *ck_get_pages(void);

/* Get the physical address of the allocated region */
phys_addr_t ck_get_phys_base(void);

/* Get a temporary kernel mapping (for loader-time copy operations) */
void *ck_get_temp_mapping(void);

/* Remove temporary mapping after setup is complete */
void ck_remove_temp_mapping(void);

/* Get trampoline page virtual address (in Linux VA space) */
void *ck_get_trampoline_va(void);

/* Get trampoline page physical address */
phys_addr_t ck_get_trampoline_phys(void);

#endif /* _COKERNEL_MEMORY_H */
