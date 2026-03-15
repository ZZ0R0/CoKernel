/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COKERNEL_SHARED_H
#define _COKERNEL_SHARED_H

/*
 * Shared definitions between the loader module and the co-kernel binary.
 *
 * Memory layout of the 32 MB allocated region:
 *
 *   Offset          Size        Content
 *   ──────────────────────────────────────────
 *   0x00000000      512 KB      Co-kernel .text + .rodata
 *   0x00080000      64  KB      Co-kernel .data + .bss
 *   0x00090000      64  KB      Page tables (PGD→PTE)
 *   0x000A0000      4   KB      IDT (256 entries × 16 bytes)
 *   0x000A1000      64  KB      Stack (grows down from top)
 *   0x000B1000      4   KB      Trampoline code
 *   0x000B2000      4   KB      Shared communication page
 *   0x000B3000      ~2  MB      Heap / buffers
 *   0x002B3000      ~1  MB      Reserved / unused
 */

#define COKERNEL_TOTAL_SIZE     (4UL * 1024 * 1024)   /* 4 MB */
#define COKERNEL_TEXT_OFFSET    0x00000000UL
#define COKERNEL_TEXT_SIZE      (512UL * 1024)
#define COKERNEL_DATA_OFFSET    0x00080000UL
#define COKERNEL_DATA_SIZE      (64UL * 1024)
#define COKERNEL_PGT_OFFSET     0x00090000UL
#define COKERNEL_PGT_SIZE       (64UL * 1024)
#define COKERNEL_IDT_OFFSET     0x000A0000UL
#define COKERNEL_IDT_SIZE       (4UL * 1024)
#define COKERNEL_STACK_OFFSET   0x000A1000UL
#define COKERNEL_STACK_SIZE     (64UL * 1024)
#define COKERNEL_TRAMPOLINE_OFFSET 0x000B1000UL
#define COKERNEL_TRAMPOLINE_SIZE   (4UL * 1024)
#define COKERNEL_COMM_OFFSET    0x000B2000UL
#define COKERNEL_COMM_SIZE      (4UL * 1024)
#define COKERNEL_HEAP_OFFSET    0x000B3000UL
#define COKERNEL_HEAP_SIZE      (2UL * 1024 * 1024)

/* Stack top: stack grows downward */
#define COKERNEL_STACK_TOP      (COKERNEL_STACK_OFFSET + COKERNEL_STACK_SIZE)

/* PMI configuration */
#define PMI_VECTOR              0x42
#define PMI_INTERVAL            1000000ULL     /* ~1M cycles between invocations */
#define PMI_RELOAD_VALUE        ((uint64_t)(-(int64_t)PMI_INTERVAL))

/* Virtual base address for co-kernel mapping in its own CR3 */
#define COKERNEL_VIRT_BASE      0xFFFF800000000000UL

/*
 * Communication page structure — shared between co-kernel and
 * any future verification mechanism.
 */
struct cokernel_comm {
    volatile uint64_t tick_count;      /* Incremented each PMI */
    volatile uint64_t last_tsc;        /* TSC at last invocation */
    volatile uint64_t status;          /* 0=idle, 1=running, 2=error */
    volatile uint64_t magic;           /* Signature for verification */
    uint8_t  reserved[4064];           /* Pad to page size */
};

#define COKERNEL_MAGIC          0xC0C0BABE00C0FFEEULL

/* MSR definitions (for use in assembly and C) */
#define MSR_IA32_FIXED_CTR_CTRL     0x38D
#define MSR_IA32_FIXED_CTR0         0x309
#define MSR_IA32_PERF_GLOBAL_CTRL   0x38F
#define MSR_IA32_PERF_GLOBAL_OVF    0x390

/* LAPIC registers (MMIO offsets from LAPIC base 0xFEE00000) */
#define LAPIC_BASE                  0xFEE00000UL
#define LAPIC_EOI_OFFSET            0xB0
#define LAPIC_LVTPC_OFFSET          0x340

#endif /* _COKERNEL_SHARED_H */
