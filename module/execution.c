/* SPDX-License-Identifier: GPL-2.0 */
/*
 * execution.c — PMI/LAPIC-based hardware execution for the co-kernel
 *
 * Configures the Performance Monitoring Unit (PMU) to generate
 * interrupts (PMI) every N CPU cycles. The interrupt fires vector
 * 0x42 which jumps to our handler via the IDT.
 *
 * No kernel function is hooked. No text is patched. The execution
 * source is purely hardware-driven.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/msr.h>
#include <asm/io.h>
#include <asm/desc.h>
#include <asm/irq.h>

#include "../include/shared.h"
#include "execution.h"

/*
 * Direct LAPIC register write — avoids apic_write() which uses
 * unexported static calls. Supports both x2APIC (MSR) and
 * xAPIC (MMIO) modes.
 */
#define MSR_X2APIC_BASE        0x800
#define XAPIC_DEFAULT_PHYS     0xFEE00000UL

static void ck_lapic_write(u32 reg, u32 val)
{
    u64 apic_base;

    rdmsrl(MSR_IA32_APICBASE, apic_base);

    if (apic_base & (1ULL << 10)) {
        /* x2APIC mode — MSR access */
        wrmsrl(MSR_X2APIC_BASE + (reg >> 4), val);
    } else {
        /* xAPIC mode — direct MMIO */
        void __iomem *base = ioremap(XAPIC_DEFAULT_PHYS, PAGE_SIZE);
        if (base) {
            writel(val, base + reg);
            iounmap(base);
        }
    }
}

/*
 * IDT gate descriptor format (x86-64, 16 bytes):
 *
 *   Bytes 0-1:  offset[15:0]
 *   Bytes 2-3:  segment selector (usually __KERNEL_CS = 0x10)
 *   Byte  4:    IST (bits 2:0) | reserved
 *   Byte  5:    type (0xE = interrupt gate) | DPL | Present
 *   Bytes 6-7:  offset[31:16]
 *   Bytes 8-11: offset[63:32]
 *   Bytes 12-15: reserved (must be 0)
 */
struct idt_gate {
    uint16_t offset_low;
    uint16_t segment;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

/*
 * ck_install_idt_handler — Write a gate descriptor into the IDT.
 *
 * We read the current IDTR to find the IDT base, then overwrite
 * the entry at the given vector index.
 */
int ck_install_idt_handler(unsigned int vector, void *handler)
{
    struct desc_ptr idtr;
    struct idt_gate *idt;
    struct idt_gate gate;
    unsigned long addr = (unsigned long)handler;

    if (vector > 255) {
        pr_err("cokernel: invalid IDT vector %u\n", vector);
        return -EINVAL;
    }

    /* Read IDTR — gives us base and limit of IDT */
    store_idt(&idtr);

    if ((vector * sizeof(struct idt_gate)) > idtr.size) {
        pr_err("cokernel: vector %u exceeds IDT limit\n", vector);
        return -EINVAL;
    }

    idt = (struct idt_gate *)idtr.address;

    /* Build interrupt gate descriptor */
    memset(&gate, 0, sizeof(gate));
    gate.offset_low  = (uint16_t)(addr & 0xFFFF);
    gate.segment     = __KERNEL_CS;
    gate.ist         = 0;    /* No IST — use current stack */
    gate.type_attr   = 0x8E; /* Present, DPL=0, Type=Interrupt Gate */
    gate.offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    gate.offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    gate.reserved    = 0;

    /* Write the gate — must bypass W/P since IDT is read-only mapped.
     * Use raw asm to avoid CR pinning checks in native_write_cr0(). */
    {
        unsigned long cr0;
        unsigned long flags;

        local_irq_save(flags);
        asm volatile("mov %%cr0, %0" : "=r"(cr0));
        asm volatile("mov %0, %%cr0" :: "r"(cr0 & ~X86_CR0_WP) : "memory");
        memcpy(&idt[vector], &gate, sizeof(gate));
        asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
        local_irq_restore(flags);
    }

    pr_info("cokernel: IDT[0x%02x] → %px\n", vector, handler);
    return 0;
}

/*
 * ck_setup_pmi_execution — Configure PMU + LAPIC for periodic PMI.
 *
 * Uses IA32_FIXED_CTR0 (instruction retired or unhalted cycles,
 * depending on the CPU model — we use it as cycle counter).
 *
 * The counter is preset to -(PMI_INTERVAL) so it overflows after
 * PMI_INTERVAL cycles, generating a PMI on the configured vector.
 */
int ck_setup_pmi_execution(void *handler_addr)
{
    int ret;
    uint64_t val;

    /* Step 1: Install IDT handler */
    ret = ck_install_idt_handler(PMI_VECTOR, handler_addr);
    if (ret)
        return ret;

    /*
     * Step 2: Configure IA32_FIXED_CTR_CTRL (MSR 0x38D)
     *
     * Bits [3:0] control Fixed Counter 0:
     *   Bit 0: count in OS mode (Ring 0)     → 1
     *   Bit 1: count in User mode (Ring 3)   → 1
     *   Bit 3: enable PMI on overflow        → 1
     * = 0xB
     *
     * We only touch bits [3:0], preserving other counters.
     */
    rdmsrl(MSR_IA32_FIXED_CTR_CTRL, val);
    val &= ~0xFULL;            /* Clear bits for CTR0 */
    val |= 0xBULL;             /* OS + User + PMI */
    wrmsrl(MSR_IA32_FIXED_CTR_CTRL, val);

    pr_info("cokernel: IA32_FIXED_CTR_CTRL = 0x%llx\n", val);

    /*
     * Step 3: Preset the counter to overflow after PMI_INTERVAL cycles.
     * IA32_FIXED_CTR0 (MSR 0x309)
     */
    wrmsrl(MSR_IA32_FIXED_CTR0, PMI_RELOAD_VALUE);

    pr_info("cokernel: IA32_FIXED_CTR0 preset to 0x%llx (interval=%llu)\n",
            PMI_RELOAD_VALUE, PMI_INTERVAL);

    /*
     * Step 4: Enable Fixed Counter 0 in IA32_PERF_GLOBAL_CTRL (MSR 0x38F)
     * Bit 32 = enable Fixed CTR0.
     */
    rdmsrl(MSR_IA32_PERF_GLOBAL_CTRL, val);
    val |= (1ULL << 32);
    wrmsrl(MSR_IA32_PERF_GLOBAL_CTRL, val);

    pr_info("cokernel: IA32_PERF_GLOBAL_CTRL = 0x%llx\n", val);

    /*
     * Step 5: Configure LAPIC LVT Performance Counter → our vector.
     * This tells the LAPIC to deliver PMI as interrupt vector 0x42.
     */
    ck_lapic_write(0x340 /* APIC_LVTPC */, PMI_VECTOR);

    pr_info("cokernel: LAPIC LVT_PC → vector 0x%02x\n", PMI_VECTOR);

    return 0;
}
