/* SPDX-License-Identifier: GPL-2.0 */
/*
 * execution.c — PMI/LAPIC-based hardware execution for the co-kernel
 *
 * Configures the Performance Monitoring Unit (PMU) to generate
 * interrupts (PMI) every N CPU cycles. The interrupt fires vector
 * 0x42 which jumps to our handler via the IDT.
 *
 * Supports both Intel (Fixed Counter 0) and AMD (Core PerfEvtSel0/PerfCtr0).
 * Detects CPU vendor and x2APIC mode, patching the trampoline data block
 * so the assembly handler uses the correct MSRs and EOI method.
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

/* ── AMD Core Performance Counter MSRs ───────────────────────── */
#define AMD_MSR_PERF_EVT_SEL0   0xC0010200
#define AMD_MSR_PERF_CTR0       0xC0010201

/*
 * AMD PerfEvtSel0 event selector for "CPU Cycles Not Halted":
 *   Event 0x76 = CPU Clocks Not Halted
 *   UnitMask = 0x00
 *   USR (bit 16) = 1, OS (bit 17) = 1
 *   EN (bit 22) = 1   — enable counter
 *   INT (bit 20) = 1  — interrupt on overflow
 */
#define AMD_EVTSEL_CYCLES_NOT_HALTED  \
    (0x76ULL | (0x00ULL << 8) | (1ULL << 16) | (1ULL << 17) | \
     (1ULL << 20) | (1ULL << 22))

/* ── LAPIC ────────────────────────────────────────────────────── */
#define MSR_X2APIC_BASE        0x800
#define XAPIC_DEFAULT_PHYS     0xFEE00000UL

static int is_amd_cpu;
static int is_x2apic;

void ck_detect_cpu_features(void)
{
    u32 eax, ebx, ecx, edx;
    u64 apic_base;

    cpuid(0, &eax, &ebx, &ecx, &edx);
    /* AMD: ebx='Auth' edx='enti' ecx='cAMD' → check ebx */
    is_amd_cpu = (ebx == 0x68747541); /* "Auth" */

    rdmsrl(MSR_IA32_APICBASE, apic_base);
    is_x2apic = !!(apic_base & (1ULL << 10));

    pr_info("cokernel: CPU vendor=%s, x2APIC=%s\n",
            is_amd_cpu ? "AMD" : "Intel",
            is_x2apic ? "yes" : "no");
}

static void ck_lapic_write(u32 reg, u32 val)
{
    if (is_x2apic) {
        wrmsrl(MSR_X2APIC_BASE + (reg >> 4), val);
    } else {
        void __iomem *base = ioremap(XAPIC_DEFAULT_PHYS, PAGE_SIZE);
        if (base) {
            writel(val, base + reg);
            iounmap(base);
        }
    }
}

/*
 * IDT gate descriptor format (x86-64, 16 bytes)
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

    store_idt(&idtr);

    if ((vector * sizeof(struct idt_gate)) > idtr.size) {
        pr_err("cokernel: vector %u exceeds IDT limit\n", vector);
        return -EINVAL;
    }

    idt = (struct idt_gate *)idtr.address;

    memset(&gate, 0, sizeof(gate));
    gate.offset_low  = (uint16_t)(addr & 0xFFFF);
    gate.segment     = __KERNEL_CS;
    gate.ist         = 0;
    gate.type_attr   = 0x8E;  /* Present, DPL=0, Interrupt Gate */
    gate.offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    gate.offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    gate.reserved    = 0;

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
 * On AMD:  Uses PerfEvtSel0/PerfCtr0 (MSR 0xC0010200/0xC0010201)
 * On Intel: Uses IA32_FIXED_CTR0 (MSR 0x309) + control MSRs
 *
 * The trampoline data block is patched with the correct MSR addresses
 * so the assembly handler reloads the right counter.
 */
int ck_setup_pmi_execution(void *handler_addr)
{
    int ret;
    uint64_t val;

    /* ck_detect_cpu_features() must be called before this function */
    /* so that is_amd_cpu and is_x2apic are already set.            */

    /* Step 1: Install IDT handler */
    ret = ck_install_idt_handler(PMI_VECTOR, handler_addr);
    if (ret)
        return ret;

    if (is_amd_cpu) {
        /*
         * AMD: Use Core Performance Counter 0.
         *
         * 1. Disable counter first (clear EN bit in EvtSel0)
         * 2. Preset counter to -(interval)
         * 3. Configure EvtSel0: event + OS + USR + INT + EN
         */
        wrmsrl(AMD_MSR_PERF_EVT_SEL0, 0);
        wrmsrl(AMD_MSR_PERF_CTR0, PMI_RELOAD_VALUE);
        wrmsrl(AMD_MSR_PERF_EVT_SEL0, AMD_EVTSEL_CYCLES_NOT_HALTED);

        rdmsrl(AMD_MSR_PERF_EVT_SEL0, val);
        pr_info("cokernel: AMD PerfEvtSel0 = 0x%llx\n", val);
        rdmsrl(AMD_MSR_PERF_CTR0, val);
        pr_info("cokernel: AMD PerfCtr0 = 0x%llx\n", val);
    } else {
        /* Intel: Use Fixed Counter 0 */
        rdmsrl(MSR_IA32_FIXED_CTR_CTRL, val);
        val &= ~0xFULL;
        val |= 0xBULL;  /* OS + User + PMI */
        wrmsrl(MSR_IA32_FIXED_CTR_CTRL, val);
        pr_info("cokernel: IA32_FIXED_CTR_CTRL = 0x%llx\n", val);

        wrmsrl(MSR_IA32_FIXED_CTR0, PMI_RELOAD_VALUE);
        pr_info("cokernel: IA32_FIXED_CTR0 = 0x%llx\n", PMI_RELOAD_VALUE);

        rdmsrl(MSR_IA32_PERF_GLOBAL_CTRL, val);
        val |= (1ULL << 32);
        wrmsrl(MSR_IA32_PERF_GLOBAL_CTRL, val);
        pr_info("cokernel: IA32_PERF_GLOBAL_CTRL = 0x%llx\n", val);
    }

    /* Configure LAPIC LVT Performance Counter → our vector */
    ck_lapic_write(0x340, PMI_VECTOR);
    pr_info("cokernel: LAPIC LVT_PC → vector 0x%02x\n", PMI_VECTOR);

    return 0;
}

/*
 * ck_get_is_amd — Expose CPU vendor to the loader for trampoline patching.
 */
int ck_get_is_amd(void)
{
    return is_amd_cpu;
}

/*
 * ck_get_is_x2apic — Expose APIC mode to the loader.
 */
int ck_get_is_x2apic(void)
{
    return is_x2apic;
}
