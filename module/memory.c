/* SPDX-License-Identifier: GPL-2.0 */
/*
 * memory.c — Invisible memory allocation for the co-kernel
 *
 * Strategy:
 *   1. alloc_pages() — allocate from buddy allocator (no vmap_area)
 *   2. set_direct_map_invalid_noflush() — remove from Linux direct map
 *   3. flush_tlb_kernel_range() — invalidate cached TLB entries
 *   4. SetPageReserved() — prevent buddy from reclaiming
 *
 * After this, the pages are physically present but completely
 * inaccessible from any Linux virtual address.
 *
 * For the loader to copy data into these pages, we create a temporary
 * manual mapping using vmap(), which we destroy after initialization.
 */

#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/set_memory.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <asm/tlbflush.h>

#include "../include/shared.h"
#include "memory.h"
#include "resolve.h"

typedef int (*set_direct_map_fn)(struct page *page);
static set_direct_map_fn fn_set_direct_map_invalid;
static set_direct_map_fn fn_set_direct_map_default;

static struct page *ck_pages;
static int ck_order;
static unsigned long ck_nr_pages;
static void *ck_temp_vmap;
static void *ck_trampoline_va;
static phys_addr_t ck_trampoline_phys;

/*
 * ck_alloc_invisible_memory — Allocate and hide pages from Linux.
 *
 * Returns 0 on success, negative errno on failure.
 */
int ck_alloc_invisible_memory(void)
{
    unsigned long i;
    unsigned long vaddr;

    /* Resolve unexported symbols at runtime */
    fn_set_direct_map_invalid = (set_direct_map_fn)
        ck_resolve_symbol("set_direct_map_invalid_noflush");
    fn_set_direct_map_default = (set_direct_map_fn)
        ck_resolve_symbol("set_direct_map_default_noflush");
    if (!fn_set_direct_map_invalid || !fn_set_direct_map_default) {
        pr_err("cokernel: failed to resolve direct map functions\n");
        return -ENOENT;
    }

    ck_order = get_order(COKERNEL_TOTAL_SIZE);
    ck_nr_pages = 1UL << ck_order;

    pr_info("cokernel: allocating %lu pages (order %d, %lu MB)\n",
            ck_nr_pages, ck_order,
            (ck_nr_pages * PAGE_SIZE) / (1024 * 1024));

    /* Step 1: Allocate contiguous pages via buddy allocator */
    ck_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, ck_order);
    if (!ck_pages) {
        pr_err("cokernel: alloc_pages failed for order %d\n", ck_order);
        return -ENOMEM;
    }

    pr_info("cokernel: pages allocated at phys 0x%llx\n",
            (unsigned long long)page_to_phys(ck_pages));

    /*
     * Step 2: Remove pages from Linux's direct map.
     * After this, any access via the direct map window
     * (ffff888000000000 + phys) will page fault.
     */
    vaddr = (unsigned long)page_address(ck_pages);

    for (i = 0; i < ck_nr_pages; i++) {
        int ret = fn_set_direct_map_invalid(ck_pages + i);
        if (ret) {
            pr_err("cokernel: set_direct_map_invalid_noflush failed "
                   "for page %lu: %d\n", i, ret);
            /* On failure, restore pages we've already invalidated */
            while (i > 0) {
                i--;
                fn_set_direct_map_default(ck_pages + i);
            }
            __free_pages(ck_pages, ck_order);
            ck_pages = NULL;
            return ret;
        }
    }

    /* Step 3: Flush TLB to ensure no stale direct map entries */
    {
        unsigned long cr3_val;
        cr3_val = __read_cr3();
        asm volatile("mov %0, %%cr3" :: "r"(cr3_val) : "memory");
    }

    /* Step 4: Mark pages as reserved — buddy allocator won't reclaim */
    for (i = 0; i < ck_nr_pages; i++)
        SetPageReserved(ck_pages + i);

    pr_info("cokernel: %lu pages hidden from direct map\n", ck_nr_pages);

    return 0;
}

/*
 * ck_get_pages — Return the base struct page.
 */
struct page *ck_get_pages(void)
{
    return ck_pages;
}

/*
 * ck_get_phys_base — Return the physical base address.
 */
phys_addr_t ck_get_phys_base(void)
{
    if (!ck_pages)
        return 0;
    return page_to_phys(ck_pages);
}

/*
 * ck_get_temp_mapping — Create a temporary vmap of the hidden pages
 * so the loader can copy the co-kernel binary and set up structures.
 *
 * This mapping is removed after setup by ck_remove_temp_mapping().
 */
void *ck_get_temp_mapping(void)
{
    struct page **page_array;
    unsigned long i;

    if (ck_temp_vmap)
        return ck_temp_vmap;

    page_array = kmalloc_array(ck_nr_pages, sizeof(struct page *),
                               GFP_KERNEL);
    if (!page_array)
        return NULL;

    for (i = 0; i < ck_nr_pages; i++)
        page_array[i] = ck_pages + i;

    ck_temp_vmap = vmap(page_array, ck_nr_pages, VM_MAP,
                        PAGE_KERNEL_EXEC);
    kfree(page_array);

    if (!ck_temp_vmap) {
        pr_err("cokernel: vmap for temp mapping failed\n");
        return NULL;
    }

    pr_info("cokernel: temp mapping at %px\n", ck_temp_vmap);
    return ck_temp_vmap;
}

/*
 * ck_remove_temp_mapping — Destroy the temporary mapping.
 * After this, only the co-kernel's own CR3 can access these pages.
 */
void ck_remove_temp_mapping(void)
{
    if (ck_temp_vmap) {
        vunmap(ck_temp_vmap);
        ck_temp_vmap = NULL;
        pr_info("cokernel: temp mapping removed\n");
    }
}

/*
 * ck_alloc_trampoline_page — Allocate a single executable page
 * for the PMI handler trampoline.
 *
 * This page stays in the normal Linux direct map because:
 *   - The IDT must point to a valid VA under Linux's CR3
 *   - The handler runs initially under Linux's CR3
 *   - We'll also map this page in the co-kernel's page tables
 *     at the same VA so the CR3 switch is transparent
 *
 * A single random kernel page is indistinguishable from any
 * other kernel allocation — effectively invisible.
 */
int ck_alloc_trampoline_page(void)
{
    unsigned long page;

    page = __get_free_page(GFP_KERNEL | __GFP_ZERO);
    if (!page) {
        pr_err("cokernel: trampoline page alloc failed\n");
        return -ENOMEM;
    }

    ck_trampoline_va = (void *)page;
    ck_trampoline_phys = virt_to_phys(ck_trampoline_va);

    pr_info("cokernel: trampoline page at VA %px, phys 0x%llx\n",
            ck_trampoline_va,
            (unsigned long long)ck_trampoline_phys);

    return 0;
}

void *ck_get_trampoline_va(void)
{
    return ck_trampoline_va;
}

phys_addr_t ck_get_trampoline_phys(void)
{
    return ck_trampoline_phys;
}
