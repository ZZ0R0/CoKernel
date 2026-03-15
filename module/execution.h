/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COKERNEL_EXECUTION_H
#define _COKERNEL_EXECUTION_H

#include <linux/types.h>

/*
 * Set up PMI-based execution:
 *   1. Install IDT handler at vector PMI_VECTOR (0x42)
 *   2. Configure PMU fixed counter 0 for cycle counting with overflow
 *   3. Configure LAPIC LVT Performance to deliver PMI_VECTOR
 *
 * @handler_addr: virtual address of the PMI handler (trampoline)
 */
int ck_setup_pmi_execution(void *handler_addr);

/*
 * Install IDT entry for our PMI vector.
 * @vector: IDT vector number (0x42)
 * @handler: address of the handler function
 */
int ck_install_idt_handler(unsigned int vector, void *handler);

/* Query CPU vendor / APIC mode (valid after ck_setup_pmi_execution) */
void ck_detect_cpu_features(void);
int ck_get_is_amd(void);
int ck_get_is_x2apic(void);

#endif /* _COKERNEL_EXECUTION_H */
