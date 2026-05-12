/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pagetable.c — Build standalone page tables for the co-kernel
 *
 * Creates a complete 4-level (PGD→P4D→PUD→PMD→PTE) hierarchy that maps
 * the co-kernel's physical pages at COKERNEL_VIRT_BASE. The resulting
 * PGD can be loaded into CR3 to give the co-kernel its own address space.
 *
 * We also identity-map the trampoline and the LAPIC MMIO region
 * so that the PMI handler can execute and send EOI.
 *
 * The page tables themselves are built inside the co-kernel's allocated
 * region at COKERNEL_PGT_OFFSET to keep everything self-contained.
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/pgtable_types.h>
#include <asm/page.h>

#include "../include/shared.h"
#include "pagetable.h"

/*
 * We build page tables manually by writing raw PTE values.
 * This avoids using any Linux page table API that might leave traces.
 *
 * x86-64 4-level paging:
 *   PGD (512 entries, indexed by bits 47:39)
 *   → P4D (mapped 1:1 with PGD on non-5level systems)
 *   → PUD (512 entries, indexed by bits 38:30)
 *   → PMD (512 entries, indexed by bits 29:21)
 *   → PTE (512 entries, indexed by bits 20:12)
 */

#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITABLE    (1ULL << 1)
#define PTE_ACCESSED    (1ULL << 5)
#define PTE_DIRTY       (1ULL << 6)
#define PTE_PSE         (1ULL << 7)     /* Page Size — 2 MB page in PMD */
#define PTE_GLOBAL      (1ULL << 8)
#define PTE_NX          (1ULL << 63)

#define PT_FLAGS_RWX    (PTE_PRESENT | PTE_WRITABLE | PTE_ACCESSED | PTE_DIRTY)
#define PT_FLAGS_RW     (PT_FLAGS_RWX | PTE_NX)
#define PT_FLAGS_RX     (PTE_PRESENT | PTE_ACCESSED)

/* Page table allocation within the co-kernel region */
static uint64_t *pgt_base;     /* Virtual base of page table area */
static phys_addr_t pgt_phys;   /* Physical base of page table area */
static unsigned long pgt_offset; /* Current allocation offset */
static phys_addr_t pgd_phys;   /* Physical address of PGD */

/*
 * Allocate a page-table page from the co-kernel's PGT area.
 * Returns the virtual address and sets *phys to the physical address.
 */
static uint64_t *pgt_alloc_page(phys_addr_t *phys)
{
    uint64_t *va;

    if (pgt_offset + PAGE_SIZE > COKERNEL_PGT_SIZE) {
        pr_err("cokernel: page table area exhausted\n");
        return NULL;
    }

    va = (uint64_t *)((unsigned long)pgt_base + pgt_offset);
    *phys = pgt_phys + pgt_offset;
    pgt_offset += PAGE_SIZE;

    memset(va, 0, PAGE_SIZE);
    return va;
}

/*
 * Map a single 4KB page in the co-kernel's page tables.
 * virt: virtual address in the co-kernel's address space
 * phys: physical address of the page
 * flags: PTE flags
 */
static int map_page(uint64_t *pgd, unsigned long virt,
                    phys_addr_t phys, uint64_t flags)
{
    unsigned int pgd_idx = (virt >> 39) & 0x1FF;
    unsigned int pud_idx = (virt >> 30) & 0x1FF;
    unsigned int pmd_idx = (virt >> 21) & 0x1FF;
    unsigned int pte_idx = (virt >> 12) & 0x1FF;
    uint64_t *pud, *pmd, *pte;
    phys_addr_t tbl_phys;

    /*
     * On x86-64 without 5-level paging, P4D is folded into PGD.
     * We treat PGD entries as pointing directly to PUD.
     */

    /* PGD → PUD */
    if (!(pgd[pgd_idx] & PTE_PRESENT)) {
        pud = pgt_alloc_page(&tbl_phys);
        if (!pud) return -ENOMEM;
        pgd[pgd_idx] = tbl_phys | PT_FLAGS_RWX;
    } else {
        /* Follow existing entry — find virtual address */
        phys_addr_t pud_phys = pgd[pgd_idx] & ~0xFFFULL;
        pud = (uint64_t *)((unsigned long)pgt_base +
                           (pud_phys - pgt_phys));
        /* Validate the pointer is within our region */
        if (pud_phys < pgt_phys ||
            pud_phys >= pgt_phys + COKERNEL_PGT_SIZE)
            return -EINVAL;
    }

    /* PUD → PMD */
    if (!(pud[pud_idx] & PTE_PRESENT)) {
        pmd = pgt_alloc_page(&tbl_phys);
        if (!pmd) return -ENOMEM;
        pud[pud_idx] = tbl_phys | PT_FLAGS_RWX;
    } else {
        phys_addr_t pmd_phys = pud[pud_idx] & ~0xFFFULL;
        pmd = (uint64_t *)((unsigned long)pgt_base +
                           (pmd_phys - pgt_phys));
        if (pmd_phys < pgt_phys ||
            pmd_phys >= pgt_phys + COKERNEL_PGT_SIZE)
            return -EINVAL;
    }

    /* PMD → PTE */
    if (!(pmd[pmd_idx] & PTE_PRESENT)) {
        pte = pgt_alloc_page(&tbl_phys);
        if (!pte) return -ENOMEM;
        pmd[pmd_idx] = tbl_phys | PT_FLAGS_RWX;
    } else {
        phys_addr_t pte_phys = pmd[pmd_idx] & ~0xFFFULL;
        pte = (uint64_t *)((unsigned long)pgt_base +
                           (pte_phys - pgt_phys));
        if (pte_phys < pgt_phys ||
            pte_phys >= pgt_phys + COKERNEL_PGT_SIZE)
            return -EINVAL;
    }

    /* PTE → Physical page */
    pte[pte_idx] = (phys & ~0xFFFULL) | flags;

    return 0;
}

/*
 * Map a single 2 MB huge page in the co-kernel's page tables.
 * Walks PGD → PUD → PMD, then sets a 2 MB page entry (PSE bit).
 *
 * If the PMD entry already exists (e.g. a 4KB PTE table from the
 * trampoline mapping), the entry is skipped to avoid conflicts.
 */
static int map_page_2m(uint64_t *pgd, unsigned long virt,
                       phys_addr_t phys, uint64_t flags)
{
    unsigned int pgd_idx = (virt >> 39) & 0x1FF;
    unsigned int pud_idx = (virt >> 30) & 0x1FF;
    unsigned int pmd_idx = (virt >> 21) & 0x1FF;
    uint64_t *pud, *pmd;
    phys_addr_t tbl_phys;

    /* PGD → PUD */
    if (!(pgd[pgd_idx] & PTE_PRESENT)) {
        pud = pgt_alloc_page(&tbl_phys);
        if (!pud) return -ENOMEM;
        pgd[pgd_idx] = tbl_phys | PT_FLAGS_RWX;
    } else {
        phys_addr_t pud_phys = pgd[pgd_idx] & ~0xFFFULL;
        pud = (uint64_t *)((unsigned long)pgt_base +
                           (pud_phys - pgt_phys));
        if (pud_phys < pgt_phys ||
            pud_phys >= pgt_phys + COKERNEL_PGT_SIZE)
            return -EINVAL;
    }

    /* PUD → PMD */
    if (!(pud[pud_idx] & PTE_PRESENT)) {
        pmd = pgt_alloc_page(&tbl_phys);
        if (!pmd) return -ENOMEM;
        pud[pud_idx] = tbl_phys | PT_FLAGS_RWX;
    } else {
        phys_addr_t pmd_phys = pud[pud_idx] & ~0xFFFULL;
        pmd = (uint64_t *)((unsigned long)pgt_base +
                           (pmd_phys - pgt_phys));
        if (pmd_phys < pgt_phys ||
            pmd_phys >= pgt_phys + COKERNEL_PGT_SIZE)
            return -EINVAL;
    }

    /* PMD → 2 MB page (skip if entry already occupied) */
    if (pmd[pmd_idx] & PTE_PRESENT)
        return 0;

    pmd[pmd_idx] = (phys & ~0x1FFFFFULL) | flags;

    return 0;
}

/*
 * ck_build_pagetables — Build the co-kernel's page table hierarchy.
 *
 * @region_base:     temp vmap of the entire co-kernel region
 * @phys_base:       physical address of the region
 * @size:            total region size
 * @trampoline_va:   Linux VA of the trampoline page
 * @trampoline_phys: physical address of the trampoline page
 */
int ck_build_pagetables(void *region_base, phys_addr_t phys_base,
                        unsigned long size,
                        void *trampoline_va, phys_addr_t trampoline_phys)
{
    uint64_t *pgd;
    unsigned long offset;
    int ret;

    /* Set up the page table allocator within the region */
    pgt_base = (uint64_t *)((unsigned long)region_base +
                            COKERNEL_PGT_OFFSET);
    pgt_phys = phys_base + COKERNEL_PGT_OFFSET;
    pgt_offset = 0;

    /* Allocate PGD (top-level directory) */
    pgd = pgt_alloc_page(&pgd_phys);
    if (!pgd)
        return -ENOMEM;

    pr_info("cokernel: PGD at phys 0x%llx\n", (unsigned long long)pgd_phys);

    /*
     * Map the entire co-kernel region at COKERNEL_VIRT_BASE.
     * Code region (.text) is mapped RX, everything else RW.
     */
    for (offset = 0; offset < size; offset += PAGE_SIZE) {
        unsigned long virt = COKERNEL_VIRT_BASE + offset;
        phys_addr_t phys = phys_base + offset;
        uint64_t flags;

        /* Text region: read+execute */
        if (offset < COKERNEL_TEXT_SIZE)
            flags = PT_FLAGS_RX;
        /* Trampoline: read+execute */
        else if (offset >= COKERNEL_TRAMPOLINE_OFFSET &&
                 offset < COKERNEL_TRAMPOLINE_OFFSET +
                          COKERNEL_TRAMPOLINE_SIZE)
            flags = PT_FLAGS_RWX;
        else
            flags = PT_FLAGS_RW;

        ret = map_page(pgd, virt, phys, flags);
        if (ret) {
            pr_err("cokernel: map_page failed at offset 0x%lx: %d\n",
                   offset, ret);
            return ret;
        }
    }

    /*
     * Map LAPIC MMIO page (0xFEE00000) so the handler can send EOI.
     * Map it at the same physical address, using uncacheable flags.
     */
    ret = map_page(pgd, LAPIC_BASE, LAPIC_BASE,
                   PTE_PRESENT | PTE_WRITABLE | PTE_ACCESSED |
                   PTE_DIRTY | PTE_NX | (1ULL << 4) /* PCD */);
    if (ret) {
        pr_err("cokernel: failed to map LAPIC: %d\n", ret);
        return ret;
    }

    /*
     * Map the trampoline page at its Linux VA.
     * This is critical: the PMI handler code lives here and must
     * be accessible both before and after the CR3 switch. By
     * mapping it at the same VA as Linux sees it, the instruction
     * flow is uninterrupted across the CR3 switch.
     */
    if (trampoline_va && trampoline_phys) {
        ret = map_page(pgd, (unsigned long)trampoline_va,
                       trampoline_phys, PT_FLAGS_RWX);
        if (ret) {
            pr_err("cokernel: failed to map trampoline: %d\n", ret);
            return ret;
        }
        pr_info("cokernel: trampoline mapped at VA %px in co-kernel PGD\n",
                trampoline_va);
    }

    pr_info("cokernel: page tables built, %lu bytes used\n", pgt_offset);
    return 0;
}

/*
 * ck_map_linux_physmem — Map physical RAM into the co-kernel's CR3
 * at LINUX_DIRECT_MAP_BASE using 2 MB huge pages.
 *
 * Must be called AFTER ck_build_pagetables() (uses the same PGD
 * and page table allocator state).
 *
 * The 2 MB region containing the trampoline is mapped RWX so the
 * PMI handler can execute after CR3 switch. All other regions are RW+NX.
 */
int ck_map_linux_physmem(unsigned long ram_size, phys_addr_t trampoline_phys)
{
    unsigned long offset;
    uint64_t *pgd;
    int ret;

    /* Reconstruct PGD virtual pointer from statics */
    pgd = (uint64_t *)((unsigned long)pgt_base + (pgd_phys - pgt_phys));

    for (offset = 0; offset < ram_size; offset += (2UL << 20)) {
        uint64_t flags = PT_FLAGS_RW | PTE_PSE;

        /* The 2 MB region containing the trampoline must be executable */
        if (trampoline_phys >= offset &&
            trampoline_phys < offset + (2UL << 20))
            flags = PT_FLAGS_RWX | PTE_PSE;

        ret = map_page_2m(pgd, LINUX_DIRECT_MAP_BASE + offset,
                          offset, flags);
        if (ret) {
            pr_err("cokernel: map_page_2m failed at offset 0x%lx: %d\n",
                   offset, ret);
            return ret;
        }
    }

    pr_info("cokernel: mapped %lu MB of physical RAM at 0x%lx\n",
            ram_size / (1024 * 1024), (unsigned long)LINUX_DIRECT_MAP_BASE);
    pr_info("cokernel: page tables now using %lu bytes\n", pgt_offset);

    return 0;
}

/*
 * ck_get_pgd_phys — Return the physical address of the PGD.
 * This is loaded into CR3 by the PMI handler.
 */
phys_addr_t ck_get_pgd_phys(void)
{
    return pgd_phys;
}
